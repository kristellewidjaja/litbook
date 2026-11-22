#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/feed/feed_receiver.hpp"
#include "litbook/feed/udp_feed_sink.hpp"
#include "litbook/pipeline/feed_worker.hpp"
#include "litbook/pipeline/threaded_gateway.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "book/test_limits.hpp"
#include "feed/feed_test_support.hpp"
#include "gateway/soup_test_support.hpp"
#include "pipeline/pipeline_test_support.hpp"
#include "pipeline/threaded_test_support.hpp"

// The whole exchange with its market data feed. Clients send orders over loopback TCP to
// a ThreadedGateway whose publisher thread runs FeedWorker, and the feed goes out over
// real multicast on lo to FeedReceivers in the test thread. Every test runs four times,
// with SpscQueue and MutexQueue, spinning and blocking.

namespace litbook::pipeline {
namespace {

using feed::FeedReceiver;
using feed::FeedReceiverConfig;
using feed::Locate;
using feed::testing::copy_matches_book;
using gateway::testing::login;
using gateway::testing::unsequenced;
using testing::limit;
using testing::Peer;
using testing::wait_until;

constexpr Price kTick = 100;
constexpr const char* kIncrementalGroup = "239.255.79.1";
constexpr const char* kSnapshotGroup = "239.255.79.2";

// A clock that moves, so the feed sends snapshots and heartbeats while the test runs.
protocol::Timestamp moving_clock() {
    const auto since_start = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<protocol::Timestamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_start).count());
}

Ticker msft() {
    return litbook::testing::unwrap(make_ticker("MSFT"));
}

protocol::EnterOrder limit_in(Ticker symbol, Side side, protocol::UserRefNum ref, Qty qty,
                              Price price) {
    protocol::EnterOrder order = limit(side, ref, qty, price);
    order.symbol = symbol;
    return order;
}

FeedReceiverConfig receiver_config(std::uint16_t incremental_port, std::uint16_t snapshot_port) {
    FeedReceiverConfig c;
    c.incremental = {.address = kIncrementalGroup, .port = incremental_port};
    c.snapshot = {.address = kSnapshotGroup, .port = snapshot_port};
    c.handler = feed::FeedHandlerConfig{
        .session = "PIPE", .max_symbols = 4, .max_levels_per_side = 256, .buffer_capacity = 4096};
    return c;
}

// Reads everything that is waiting on a receiver's sockets.
void drain(FeedReceiver& receiver) {
    while (receiver.poll_once(0) > 0) {
    }
}

// The level count on both sides of a stock in a receiver's book.
std::size_t levels(const FeedReceiver& receiver, Locate locate) {
    const feed::LevelBook& book = *receiver.handler().book(locate);
    return book.level_count(Side::Buy) + book.level_count(Side::Sell);
}

template <template <concurrency::QueueElement> class Q, WaitMode Mode>
struct Setup {
    static constexpr WaitMode kMode = Mode;
};

template <class S>
struct ExchangeOf;
template <template <concurrency::QueueElement> class Q, WaitMode Mode>
struct ExchangeOf<Setup<Q, Mode>> {
    using type = BasicThreadedGateway<SortedOrderBook, Q, FeedWorker>;
};

template <class S>
class ThreadedFeedTest : public ::testing::Test {
protected:
    using Exchange = typename ExchangeOf<S>::type;

    // The receiver comes first, because the feed must be told the ports it got.
    void make(std::size_t feed_max_symbols = 8) {
        receiver_ = std::make_unique<FeedReceiver>(receiver_config(0, 0));
        feed::FeedNetworkConfig network;
        network.incremental = {.address = kIncrementalGroup,
                               .port = receiver_->incremental().port()};
        network.snapshot = {.address = kSnapshotGroup, .port = receiver_->snapshot().port()};
        sink_ = std::make_unique<feed::UdpFeedSink>(network);

        FeedWorkerOptions feed;
        feed.feed.session = "PIPE";
        feed.feed.max_symbols = feed_max_symbols;
        feed.feed.snapshot_spacing = 1'000'000;  // 1 ms, so a late joiner is quick.
        feed.sink = sink_.get();
        feed.max_levels_per_side = 256;

        gateway::SessionConfig session;
        session.max_connections = 8;
        PipelineConfig pipeline;
        pipeline.wait = S::kMode;
        exchange_ = std::make_unique<Exchange>(
            session,
            protocol::OrderEntryConfig{.max_owners = 4, .max_live_orders = 4096, .max_symbols = 2},
            gateway::GatewayConfig{.clock = &moving_clock}, pipeline, feed);
        exchange_->add_account(
            {.username = "alice", .password = "secret", .cancel_on_disconnect = true});
        exchange_->add_account({.username = "bob", .password = "hunter2"});
    }

    void add_stocks() {
        const BookConfig book = litbook::testing::with_test_limits(BookConfig{.tick_size = kTick});
        EXPECT_EQ(exchange_->add_symbol(testing::aapl(), book), 0U);
        EXPECT_EQ(exchange_->add_symbol(msft(), book), 1U);
    }

    std::unique_ptr<Peer> log_in(std::string_view user, std::string_view password) {
        auto peer = std::make_unique<Peer>(exchange_->port());
        peer->send(login(user, password));
        EXPECT_TRUE(wait_until({peer.get()}, [&] {
            return peer->count(gateway::soup::PacketType::LoginAccepted) == 1;
        }));
        return peer;
    }

    // After stop(), reads the feed until its end of session and checks that every book
    // the receiver built equals the feed's copy and the engine's book.
    void expect_receiver_matches_exchange(FeedReceiver& receiver) {
        ASSERT_TRUE(wait_until({}, [&] {
            drain(receiver);
            return receiver.handler().ended();
        }));
        const auto& feed = exchange_->publisher().feed();
        const auto& engine = exchange_->matching().order_entry().engine();
        for (std::size_t i = 0; i < feed.symbol_count(); ++i) {
            const auto l = static_cast<Locate>(i);
            EXPECT_TRUE(receiver.handler().live(l));
            EXPECT_EQ(*receiver.handler().book(l), feed.copy(l));
            EXPECT_TRUE(copy_matches_book(feed.copy(l), *engine.book(l)));
        }
    }

    std::unique_ptr<FeedReceiver> receiver_;
    std::unique_ptr<feed::UdpFeedSink> sink_;
    std::unique_ptr<Exchange> exchange_;
};

using Setups = ::testing::Types<Setup<concurrency::SpscQueue, WaitMode::Spin>,
                                Setup<concurrency::SpscQueue, WaitMode::Block>,
                                Setup<concurrency::MutexQueue, WaitMode::Spin>,
                                Setup<concurrency::MutexQueue, WaitMode::Block>>;
TYPED_TEST_SUITE(ThreadedFeedTest, Setups);

TYPED_TEST(ThreadedFeedTest, OrdersAndTradesReachTheFeed) {
    this->make();
    this->add_stocks();
    this->exchange_->start();
    auto alice = this->log_in("alice", "secret");
    auto bob = this->log_in("bob", "hunter2");
    FeedReceiver& receiver = *this->receiver_;

    bob->send(unsequenced(limit_in(testing::aapl(), Side::Sell, 1, 50, 50 * kTick)));
    bob->send(unsequenced(limit_in(msft(), Side::Sell, 2, 30, 60 * kTick)));
    ASSERT_TRUE(wait_until(
        {bob.get()}, [&] { return bob->template all<protocol::AcceptedMessage>().size() == 2; }));
    alice->send(unsequenced(limit_in(testing::aapl(), Side::Buy, 1, 20, 51 * kTick)));
    alice->send(unsequenced(limit_in(msft(), Side::Buy, 2, 10, 55 * kTick)));

    // The feed shows the trade, AAPL's offer down to 30, and both MSFT levels.
    ASSERT_TRUE(wait_until({alice.get(), bob.get()}, [&] {
        drain(receiver);
        const feed::FeedHandler& h = receiver.handler();
        const std::optional<feed::PriceLevel> aapl_offer = h.book(0)->best(Side::Sell);
        return h.stats().trades == 1 && aapl_offer.has_value() && aapl_offer->total_qty == 30 &&
               levels(receiver, 0) == 1 && levels(receiver, 1) == 2;
    }));
    const feed::FeedHandler& h = receiver.handler();
    EXPECT_TRUE(h.started());
    ASSERT_TRUE(h.directory(1).has_value());
    const feed::SymbolDirectory msft_entry = litbook::testing::unwrap(h.directory(1));
    EXPECT_EQ(msft_entry.symbol, msft());
    EXPECT_EQ(msft_entry.tick_size, kTick);

    // Stopping closes alice's connection first, so her MSFT bid is canceled on disconnect
    // and that also reaches the feed before End of Session.
    this->exchange_->stop();
    this->expect_receiver_matches_exchange(receiver);
    EXPECT_EQ(levels(receiver, 1), 1U);
    EXPECT_EQ(h.stats().gaps, 0U);
    EXPECT_EQ(h.stats().trades_lost, 0U);
    EXPECT_EQ(this->sink_->sender(feed::FeedStream::Incremental).stats().dropped, 0U);
    EXPECT_GT(this->exchange_->publisher().stats().events, 0U);
}

TYPED_TEST(ThreadedFeedTest, ALateListenerRecoversFromSnapshots) {
    this->make();
    this->add_stocks();
    this->exchange_->start();
    auto bob = this->log_in("bob", "hunter2");
    FeedReceiver& first = *this->receiver_;
    for (protocol::UserRefNum ref = 1; ref <= 5; ++ref) {
        const Ticker symbol = ref % 2 == 0 ? msft() : testing::aapl();
        const auto price = static_cast<Price>(40 + ref) * kTick;
        bob->send(unsequenced(limit_in(symbol, Side::Buy, ref, 10 * ref, price)));
    }
    ASSERT_TRUE(wait_until({bob.get()}, [&] {
        drain(first);
        return levels(first, 0) + levels(first, 1) == 5;
    }));

    // It joins on the same ports after the feed started, so it missed the directory and
    // every level so far.
    auto late = std::make_unique<FeedReceiver>(
        receiver_config(first.incremental().port(), first.snapshot().port()));
    bob->send(unsequenced(limit_in(testing::aapl(), Side::Sell, 6, 15, 60 * kTick)));
    bob->send(unsequenced(limit_in(msft(), Side::Sell, 7, 25, 61 * kTick)));
    ASSERT_TRUE(wait_until({bob.get()}, [&] {
        drain(first);
        drain(*late);
        const feed::FeedHandler& l = late->handler();
        if (!l.symbol_count().has_value() || l.recovering()) {
            return false;
        }
        return levels(first, 0) + levels(first, 1) == 7 && *l.book(0) == *first.handler().book(0) &&
               *l.book(1) == *first.handler().book(1);
    }));
    EXPECT_GE(late->handler().stats().snapshots_applied, 2U);
    EXPECT_FALSE(late->handler().started());  // It never saw Start of Messages.

    this->exchange_->stop();
    this->expect_receiver_matches_exchange(first);
    this->expect_receiver_matches_exchange(*late);
    EXPECT_EQ(first.handler().stats().gaps, 0U);
}

TYPED_TEST(ThreadedFeedTest, AStockTheFeedCannotListStopsTheStart) {
    this->make(1);
    const BookConfig book = litbook::testing::with_test_limits(BookConfig{.tick_size = kTick});
    EXPECT_EQ(this->exchange_->add_symbol(testing::aapl(), book), 0U);
    EXPECT_THROW(this->exchange_->add_symbol(msft(), book), std::length_error);
    EXPECT_THROW(this->exchange_->start(), std::logic_error);
}

}  // namespace
}  // namespace litbook::pipeline
