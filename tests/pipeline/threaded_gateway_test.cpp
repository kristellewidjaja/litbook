#include "litbook/pipeline/threaded_gateway.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/concurrency/thread_affinity.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include "book/test_limits.hpp"
#include "gateway/soup_test_support.hpp"
#include "gateway/test_client.hpp"
#include "pipeline/pipeline_test_support.hpp"
#include "pipeline/threaded_test_support.hpp"

// The threaded pipeline over real loopback sockets. Every test runs four times, with
// SpscQueue and MutexQueue, spinning and blocking. Under the tsan presets
// ThreadSanitizer watches all three threads.

namespace litbook::pipeline {
namespace {

using gateway::soup::PacketType;
using gateway::testing::Bytes;
using gateway::testing::login;
using gateway::testing::unsequenced;
using protocol::OutboundMessage;
using testing::limit;
using testing::Peer;
using testing::wait_until;

constexpr Price kTick = 100;
constexpr OwnerId kAlice = 1;  // Cancel on disconnect is on.
constexpr OwnerId kBob = 2;

// A clock that never moves, so no heartbeat or timeout gets in the way.
protocol::Timestamp still_clock() {
    return 1'000'000;
}

// One queue type and one wait mode.
template <template <concurrency::QueueElement> class Q, WaitMode Mode>
struct Setup {
    static constexpr WaitMode kMode = Mode;
};

// The pipeline type for a Setup. The partial specialization takes the queue template
// apart again, so ThreadedGateway gets the queue template itself.
template <class S>
struct ExchangeOf;
template <template <concurrency::QueueElement> class Q, WaitMode Mode>
struct ExchangeOf<Setup<Q, Mode>> {
    using type = ThreadedGateway<SortedOrderBook, Q>;
};

template <class S>
class ThreadedGatewayTest : public ::testing::Test {
protected:
    using Exchange = typename ExchangeOf<S>::type;

    void make(PipelineConfig pipeline = {}, gateway::SessionConfig session = {}) {
        pipeline.wait = S::kMode;
        session.max_connections = 8;
        exchange_ = std::make_unique<Exchange>(
            session,
            protocol::OrderEntryConfig{.max_owners = 4, .max_live_orders = 4096, .max_symbols = 1},
            gateway::GatewayConfig{.clock = &still_clock}, pipeline);
        exchange_->add_account(
            {.username = "alice", .password = "secret", .cancel_on_disconnect = true});
        exchange_->add_account({.username = "bob", .password = "hunter2"});
        exchange_->add_symbol(testing::aapl(),
                              litbook::testing::with_test_limits(BookConfig{.tick_size = kTick}));
    }

    // Connects and logs in, and waits for Login Accepted.
    std::unique_ptr<Peer> log_in(std::string_view user, std::string_view password) {
        auto peer = std::make_unique<Peer>(exchange_->port());
        peer->send(login(user, password));
        EXPECT_TRUE(
            wait_until({peer.get()}, [&] { return peer->count(PacketType::LoginAccepted) == 1; }));
        return peer;
    }

    std::unique_ptr<Exchange> exchange_;
};

using Setups = ::testing::Types<Setup<concurrency::SpscQueue, WaitMode::Spin>,
                                Setup<concurrency::SpscQueue, WaitMode::Block>,
                                Setup<concurrency::MutexQueue, WaitMode::Spin>,
                                Setup<concurrency::MutexQueue, WaitMode::Block>>;
TYPED_TEST_SUITE(ThreadedGatewayTest, Setups);

TYPED_TEST(ThreadedGatewayTest, AnOrderIsAcceptedThroughAllThreeThreads) {
    this->make();
    this->exchange_->start();
    auto alice = this->log_in("alice", "secret");
    alice->send(unsequenced(limit(Side::Buy, 1, 100, 50 * kTick)));
    ASSERT_TRUE(wait_until({alice.get()}, [&] {
        return alice->template all<protocol::AcceptedMessage>().size() == 1;
    }));
    const auto accepted = alice->template all<protocol::AcceptedMessage>().front();
    EXPECT_EQ(accepted.user_ref, 1U);
    EXPECT_EQ(accepted.timestamp, still_clock());
    this->exchange_->stop();
    EXPECT_EQ(this->exchange_->matching().stats().frames, 1U);
    EXPECT_EQ(this->exchange_->publisher().stats().level_updates, 2U);  // Added, then canceled.
    EXPECT_TRUE(this->exchange_->publisher().stopped());
    EXPECT_EQ(this->exchange_->sessions().in_flight(kAlice), 0U);
}

TYPED_TEST(ThreadedGatewayTest, TwoClientsTradeAndThePublisherSeesTheTrade) {
    this->make();
    this->exchange_->start();
    auto alice = this->log_in("alice", "secret");
    auto bob = this->log_in("bob", "hunter2");
    bob->send(unsequenced(limit(Side::Sell, 1, 50, 50 * kTick)));
    ASSERT_TRUE(wait_until(
        {bob.get()}, [&] { return bob->template all<protocol::AcceptedMessage>().size() == 1; }));
    alice->send(unsequenced(limit(Side::Buy, 1, 20, 51 * kTick)));
    ASSERT_TRUE(wait_until({alice.get(), bob.get()}, [&] {
        return alice->template all<protocol::ExecutedMessage>().size() == 1 &&
               bob->template all<protocol::ExecutedMessage>().size() == 1;
    }));
    const auto a = alice->template all<protocol::ExecutedMessage>().front();
    const auto b = bob->template all<protocol::ExecutedMessage>().front();
    EXPECT_EQ(a.match, b.match);
    EXPECT_EQ(a.qty, 20U);
    EXPECT_EQ(a.price, 50 * kTick);
    this->exchange_->stop();
    EXPECT_EQ(this->exchange_->publisher().stats().trades, 1U);
    EXPECT_EQ(this->exchange_->publisher().stats().traded_qty, 20U);
    // bob has no cancel on disconnect, so his 30 remaining shares still rest.
    EXPECT_EQ(this->exchange_->matching().order_entry().open_orders(kBob), 1U);
}

TYPED_TEST(ThreadedGatewayTest, AHangUpCancelsTheOrdersOfAnAccountWithCancelOnDisconnect) {
    this->make();
    this->exchange_->start();
    auto alice = this->log_in("alice", "secret");
    auto bob = this->log_in("bob", "hunter2");
    for (protocol::UserRefNum ref = 1; ref <= 3; ++ref) {
        alice->send(unsequenced(limit(Side::Sell, ref, 10, 60 * kTick)));
    }
    ASSERT_TRUE(wait_until({alice.get()}, [&] {
        return alice->template all<protocol::AcceptedMessage>().size() == 3;
    }));
    alice->client->disconnect();
    // A second login can only succeed after the gateway closed the first connection,
    // and the close queued the cancel. Anything bob sends after that is behind it.
    alice = this->log_in("alice", "secret");
    bob->send(unsequenced(limit(Side::Buy, 1, 10, 60 * kTick)));
    ASSERT_TRUE(wait_until(
        {bob.get()}, [&] { return bob->template all<protocol::AcceptedMessage>().size() == 1; }));
    this->exchange_->stop();
    bob->collect();
    EXPECT_TRUE(bob->template all<protocol::ExecutedMessage>().empty());
    EXPECT_EQ(this->exchange_->matching().stats().cancel_alls, 2U);  // Hang up and stop.
    EXPECT_EQ(this->exchange_->matching().order_entry().open_orders(kAlice), 0U);
    EXPECT_EQ(this->exchange_->matching().order_entry().open_orders(kBob), 1U);
}

// 300 orders in one write go through a request queue with room for two and at most four
// requests in flight. Nothing may be lost or reordered, and the client stays connected.
TYPED_TEST(ThreadedGatewayTest, ABurstThroughTinyQueuesArrivesCompleteAndInOrder) {
    this->make(PipelineConfig{.request_capacity = 2,
                              .reply_capacity = 4,
                              .event_capacity = 4,
                              .max_replies_per_drain = 1},
               gateway::SessionConfig{.max_in_flight = 4});
    this->exchange_->start();
    auto alice = this->log_in("alice", "secret");
    constexpr protocol::UserRefNum kOrders = 300;
    Bytes burst;
    for (protocol::UserRefNum ref = 1; ref <= kOrders; ++ref) {
        const Bytes p =
            unsequenced(limit(Side::Buy, ref, 1, static_cast<Price>(40 + ref % 10) * kTick));
        burst.insert(burst.end(), p.begin(), p.end());
    }
    alice->send(burst);
    ASSERT_TRUE(wait_until({alice.get()}, [&] {
        return alice->template all<protocol::AcceptedMessage>().size() == kOrders;
    }));
    const auto accepted = alice->template all<protocol::AcceptedMessage>();
    for (protocol::UserRefNum i = 0; i < kOrders; ++i) {
        EXPECT_EQ(accepted[i].user_ref, i + 1);
    }
    EXPECT_FALSE(alice->client->closed());
    this->exchange_->stop();
    EXPECT_GT(this->exchange_->sessions().stats().backend_busy +
                  this->exchange_->matching().stats().reply_waits,
              0U);
    EXPECT_EQ(this->exchange_->sessions().stats().messages_dropped, kOrders);  // The COD cancels.
}

TYPED_TEST(ThreadedGatewayTest, StoppingSendsEndOfSessionToEveryClient) {
    this->make();
    this->exchange_->start();
    auto alice = this->log_in("alice", "secret");
    auto bob = this->log_in("bob", "hunter2");
    this->exchange_->stop();
    ASSERT_TRUE(wait_until({alice.get(), bob.get()}, [&] {
        return alice->count(PacketType::EndOfSession) == 1 &&
               bob->count(PacketType::EndOfSession) == 1;
    }));
    this->exchange_->stop();  // A second stop does nothing.
}

TYPED_TEST(ThreadedGatewayTest, ThreadsCanBePinned) {
    const std::vector<int> cpus = concurrency::allowed_cpus();
    ASSERT_FALSE(cpus.empty());
    const auto cpu = [&](std::size_t i) { return cpus[i % cpus.size()]; };
    this->make(
        PipelineConfig{.gateway_cpu = cpu(0), .matching_cpu = cpu(1), .publisher_cpu = cpu(2)});
    this->exchange_->start();
    auto alice = this->log_in("alice", "secret");
    this->exchange_->stop();
    EXPECT_EQ(this->exchange_->pin_result(PipelineThread::Gateway), 0);
    EXPECT_EQ(this->exchange_->pin_result(PipelineThread::Matching), 0);
    EXPECT_EQ(this->exchange_->pin_result(PipelineThread::Publisher), 0);
}

TYPED_TEST(ThreadedGatewayTest, SetupIsRefusedOnceTheThreadsRun) {
    this->make();
    this->exchange_->start();
    EXPECT_THROW(this->exchange_->add_account({.username = "carol", .password = "pw"}),
                 std::logic_error);
    EXPECT_THROW(this->exchange_->add_symbol(testing::aapl(), BookConfig{}), std::logic_error);
    EXPECT_THROW(this->exchange_->start(), std::logic_error);
    this->exchange_->stop();
}

TYPED_TEST(ThreadedGatewayTest, APipelineThatNeverStartedStopsAndDestroysCleanly) {
    this->make();
    this->exchange_->stop();
    this->exchange_.reset();
}

TYPED_TEST(ThreadedGatewayTest, DestroyingARunningPipelineStopsIt) {
    this->make();
    this->exchange_->start();
    auto alice = this->log_in("alice", "secret");
    this->exchange_.reset();  // Joins the threads, so nothing is left running.
}

}  // namespace
}  // namespace litbook::pipeline
