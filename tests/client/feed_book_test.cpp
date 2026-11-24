// Tests of the feed book over real multicast sockets on loopback. The publisher runs on
// the test thread with time moved by hand, and the feed book reads on its own thread. The
// tests only use the feed book's locked calls, so ThreadSanitizer checks the locking.

#include "litbook/client/feed_book.hpp"

#include "litbook/feed/feed_publisher.hpp"
#include "litbook/feed/udp_feed_sink.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "feed/feed_test_support.hpp"
#include "support/unwrap.hpp"

namespace litbook::client {
namespace {

using feed::FeedNetworkConfig;
using feed::FeedPublisher;
using feed::FeedPublisherConfig;
using feed::PriceLevel;
using feed::Timestamp;
using feed::UdpFeedSink;
using feed::testing::level_event;
using feed::testing::ticker;
using feed::testing::trade_event;
using Clock = std::chrono::steady_clock;

constexpr Price kTick = 100;
constexpr Timestamp kSpacing = 1'000'000;

FeedBookConfig book_config(const char* incremental, const char* snapshot) {
    FeedBookConfig c;
    c.receiver.incremental = {.address = incremental, .port = 0};
    c.receiver.snapshot = {.address = snapshot, .port = 0};
    c.receiver.handler.session = "BOOK";
    c.receiver.handler.max_symbols = 4;
    c.receiver.handler.max_levels_per_side = 64;
    c.receiver.handler.buffer_capacity = 4096;
    return c;
}

// Passes packets on to the real sink only while connected is true, like a network cable,
// so a test can lose packets for certain instead of hoping the kernel drops some.
class SwitchSink final : public feed::FeedSink {
public:
    explicit SwitchSink(feed::FeedSink& target) : target_(&target) {}
    void send(feed::FeedStream stream, std::span<const std::byte> packet) override {
        if (connected) {
            target_->send(stream, packet);
        }
    }
    bool connected = true;

private:
    feed::FeedSink* target_;
};

class FeedBookTest : public ::testing::Test {
protected:
    // Makes the book first, because the publisher must be told the ports it got.
    void make(const char* incremental, const char* snapshot) {
        const FeedBookConfig c = book_config(incremental, snapshot);
        book_ = std::make_unique<FeedBook>(c);
        FeedNetworkConfig network;
        network.incremental = {.address = incremental, .port = book_->incremental_port()};
        network.snapshot = {.address = snapshot, .port = book_->snapshot_port()};
        sink_ = std::make_unique<UdpFeedSink>(network);
        cable_ = std::make_unique<SwitchSink>(*sink_);
        FeedPublisherConfig config;
        config.session = "BOOK";
        config.snapshot_spacing = kSpacing;
        publisher_ = std::make_unique<FeedPublisher>(*cable_, config);
        for (const char* name : {"AAPL", "MSFT"}) {
            (void)publisher_->add_symbol({.symbol = ticker(name),
                                          .tick_size = kTick,
                                          .lot_size = 1,
                                          .max_levels_per_side = 64});
        }
    }

    // Waits on the book's own update signal until `done` is true or five seconds pass.
    // `between` runs while waiting, for example to send the next snapshot.
    bool wait_for(const std::function<bool()>& done, const std::function<void()>& between = [] {}) {
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        std::uint64_t seen = book_->version();
        while (Clock::now() < deadline) {
            if (done()) {
                return true;
            }
            between();
            seen = book_->wait_update(seen, 5);
        }
        return done();
    }

    void level(feed::Locate locate, Side side, Price price, AggregateQty qty,
               std::uint32_t orders) {
        publisher_->on_event(level_event(locate, side, price, qty, orders, now_));
    }

    void next_snapshot() {
        now_ += kSpacing;
        publisher_->tick(now_);
    }

    Timestamp now_ = 1'000;
    std::unique_ptr<FeedBook> book_;
    std::unique_ptr<UdpFeedSink> sink_;
    std::unique_ptr<SwitchSink> cable_;
    std::unique_ptr<FeedPublisher> publisher_;
};

TEST_F(FeedBookTest, SymbolsAreUnknownUntilTheFeedNamesThem) {
    make("239.255.79.1", "239.255.79.2");
    EXPECT_EQ(book_->status("AAPL"), SymbolStatus::Unknown);
    EXPECT_FALSE(book_->best("AAPL").has_value());
    EXPECT_TRUE(book_->depth("AAPL", Side::Buy, 5).empty());
    EXPECT_TRUE(book_->symbols().empty());
    EXPECT_FALSE(book_->stats().started);

    publisher_->start(now_);
    ASSERT_TRUE(wait_for([&] { return book_->symbols().size() == 2; }));
    EXPECT_EQ(book_->symbols(), (std::vector<std::string>{"AAPL", "MSFT"}));
    EXPECT_EQ(book_->status("AAPL"), SymbolStatus::Live);
    EXPECT_TRUE(book_->stats().started);
    // An empty book has no best prices yet.
    const Quote q = testing::unwrap(book_->best("AAPL"));
    EXPECT_FALSE(q.bid.has_value());
    EXPECT_FALSE(q.ask.has_value());
    EXPECT_TRUE(q.live);
    // Names that are not tickers, or not on the feed, stay unknown.
    EXPECT_EQ(book_->status("IBM"), SymbolStatus::Unknown);
    EXPECT_EQ(book_->status(""), SymbolStatus::Unknown);
    EXPECT_EQ(book_->status("WAY TOO LONG"), SymbolStatus::Unknown);
}

TEST_F(FeedBookTest, BestAndDepthFollowTheLevelUpdates) {
    make("239.255.79.3", "239.255.79.4");
    publisher_->start(now_);
    level(0, Side::Buy, 1'000 * kTick, 100, 1);
    level(0, Side::Buy, 999 * kTick, 200, 2);
    level(0, Side::Buy, 998 * kTick, 300, 3);
    level(0, Side::Sell, 1'001 * kTick, 50, 1);
    level(1, Side::Sell, 2'000 * kTick, 70, 1);
    publisher_->flush(now_);

    ASSERT_TRUE(wait_for([&] { return book_->depth("AAPL", Side::Buy, 10).size() == 3; }));
    ASSERT_TRUE(wait_for([&] { return !book_->depth("MSFT", Side::Sell, 10).empty(); }));
    const Quote q = testing::unwrap(book_->best("AAPL"));
    EXPECT_EQ(testing::unwrap(q.bid),
              (PriceLevel{.price = 1'000 * kTick, .total_qty = 100, .order_count = 1}));
    EXPECT_EQ(testing::unwrap(q.ask),
              (PriceLevel{.price = 1'001 * kTick, .total_qty = 50, .order_count = 1}));
    const std::vector<PriceLevel> bids = book_->depth("AAPL", Side::Buy, 2);
    ASSERT_EQ(bids.size(), 2U);
    EXPECT_EQ(bids[0].price, 1'000 * kTick);  // Best first.
    EXPECT_EQ(bids[1].price, 999 * kTick);
    EXPECT_EQ(book_->depth("AAPL", Side::Buy, 0).size(), 0U);
    EXPECT_FALSE(testing::unwrap(book_->best("MSFT")).bid.has_value());

    // A level with no orders left is gone.
    level(0, Side::Buy, 1'000 * kTick, 0, 0);
    publisher_->on_event(trade_event(0, Side::Buy, 1'000 * kTick, 100, 1, now_));
    publisher_->flush(now_);
    ASSERT_TRUE(wait_for([&] {
        const Quote now = testing::unwrap(book_->best("AAPL"));
        return now.bid && now.bid->price == 999 * kTick;
    }));
    ASSERT_TRUE(wait_for([&] { return book_->stats().handler.trades == 1; }));
}

TEST_F(FeedBookTest, WaitUpdateReturnsAtOnceForANewVersionAndWaitsOtherwise) {
    make("239.255.79.5", "239.255.79.6");
    const std::uint64_t before = book_->version();
    const auto begin = Clock::now();
    EXPECT_EQ(book_->wait_update(before, 50), before);
    EXPECT_GE(Clock::now() - begin, std::chrono::milliseconds(50));

    publisher_->start(now_);
    const std::uint64_t after = book_->wait_update(before, 5'000);
    EXPECT_GT(after, before);
    EXPECT_EQ(book_->wait_update(before, 5'000), book_->version());
}

// Packets lost on the way make the book see a gap. Every stock must then say it is stale,
// keep answering from its last copy, and come back live from the snapshots. (A first
// version hoped the kernel would drop packets from a burst into a tiny receive buffer, and
// on CI's release build the reader kept up, so nothing was lost.)
TEST_F(FeedBookTest, LostPacketsMakeStocksStaleUntilSnapshotsRepairThem) {
    make("239.255.79.7", "239.255.79.8");
    publisher_->start(now_);
    level(0, Side::Buy, 1'000 * kTick, 100, 1);
    publisher_->flush(now_);
    ASSERT_TRUE(wait_for([&] { return !book_->depth("AAPL", Side::Buy, 10).empty(); }));

    // These packets never arrive.
    cable_->connected = false;
    for (int i = 0; i < 20; ++i) {
        const auto locate = static_cast<feed::Locate>(i % 2);
        level(locate, Side::Buy, (900 + i) * kTick, 100U + static_cast<unsigned>(i), 1);
        publisher_->flush(now_);
    }
    cable_->connected = true;
    // The next packet shows the book that it missed some.
    level(1, Side::Sell, 1'500 * kTick, 5, 1);
    publisher_->flush(now_);

    ASSERT_TRUE(wait_for([&] { return book_->stats().handler.gaps >= 1; }));
    // No snapshot has been sent yet, so both stocks wait for one.
    EXPECT_EQ(book_->status("AAPL"), SymbolStatus::Stale);
    EXPECT_EQ(book_->status("MSFT"), SymbolStatus::Stale);
    EXPECT_TRUE(book_->stats().recovering);
    const Quote stale = testing::unwrap(book_->best("AAPL"));
    EXPECT_FALSE(stale.live);
    EXPECT_EQ(testing::unwrap(stale.bid).price, 1'000 * kTick);  // The last good copy.

    ASSERT_TRUE(wait_for(
        [&] {
            const FeedBookStats s = book_->stats();
            return !s.recovering && s.handler.recoveries >= 1 &&
                   book_->status("AAPL") == SymbolStatus::Live &&
                   book_->status("MSFT") == SymbolStatus::Live;
        },
        [&] { next_snapshot(); }));
    // Back in step with the publisher, level for level.
    for (feed::Locate l = 0; l < 2; ++l) {
        const char* name = l == 0 ? "AAPL" : "MSFT";
        const feed::LevelBook& copy = publisher_->copy(l);
        ASSERT_EQ(book_->depth(name, Side::Buy, 64).size(), copy.level_count(Side::Buy));
        for (std::size_t r = 0; r < copy.level_count(Side::Buy); ++r) {
            EXPECT_EQ(book_->depth(name, Side::Buy, 64)[r], copy.level(Side::Buy, r));
        }
    }
}

// A receiver that joins after the session started can learn the stocks from the snapshot
// stream before it has read anything on the incremental stream. Until it has, it cannot know
// that it missed level updates, so no stock may say it is live. (Found by the Python book
// view, which printed an empty book as live for about half a second after it started.)
TEST_F(FeedBookTest, ALateJoinerIsNotLiveBeforeItHasReadTheIncrementalStream) {
    make("239.255.79.21", "239.255.79.22");
    // Everything up to here is lost, as if the book had not joined yet.
    cable_->connected = false;
    publisher_->start(now_);
    level(0, Side::Buy, 1'000 * kTick, 100, 1);
    publisher_->flush(now_);
    cable_->connected = true;

    // Only the snapshot stream speaks, so the book learns the stocks from it.
    ASSERT_TRUE(wait_for([&] { return book_->symbols().size() == 2; }, [&] { next_snapshot(); }));
    EXPECT_EQ(book_->stats().handler.incremental_packets, 0U);
    EXPECT_EQ(book_->status("AAPL"), SymbolStatus::Stale);
    EXPECT_EQ(book_->status("MSFT"), SymbolStatus::Stale);
    EXPECT_FALSE(testing::unwrap(book_->best("AAPL")).live);

    // The first incremental packet shows the gap, and the snapshots then make both stocks live,
    // with the level that never arrived on the incremental stream.
    level(1, Side::Sell, 1'500 * kTick, 5, 1);
    publisher_->flush(now_);
    ASSERT_TRUE(wait_for(
        [&] {
            return book_->status("AAPL") == SymbolStatus::Live &&
                   book_->status("MSFT") == SymbolStatus::Live;
        },
        [&] { next_snapshot(); }));
    EXPECT_GE(book_->stats().handler.gaps, 1U);
    const Quote aapl = testing::unwrap(book_->best("AAPL"));
    EXPECT_TRUE(aapl.live);
    EXPECT_EQ(testing::unwrap(aapl.bid).price, 1'000 * kTick);
    EXPECT_EQ(testing::unwrap(testing::unwrap(book_->best("MSFT")).ask).price, 1'500 * kTick);
}

TEST_F(FeedBookTest, EndOfSessionIsSeenAndStopKeepsTheLastBooks) {
    make("239.255.79.9", "239.255.79.10");
    publisher_->start(now_);
    level(0, Side::Sell, 1'001 * kTick, 50, 1);
    publisher_->end_session(now_);
    ASSERT_TRUE(wait_for([&] { return book_->stats().ended; }));
    book_->stop();
    book_->stop();  // Twice is fine.
    EXPECT_EQ(testing::unwrap(testing::unwrap(book_->best("AAPL")).ask).total_qty, 50U);
}

TEST_F(FeedBookTest, AQuietFeedMakesTheBookSilent) {
    FeedBookConfig c = book_config("239.255.79.11", "239.255.79.12");
    c.receiver.silence_limit = 50'000'000;  // 50 ms
    book_ = std::make_unique<FeedBook>(c);
    FeedNetworkConfig network;
    network.incremental = {.address = "239.255.79.11", .port = book_->incremental_port()};
    network.snapshot = {.address = "239.255.79.12", .port = book_->snapshot_port()};
    sink_ = std::make_unique<UdpFeedSink>(network);
    FeedPublisherConfig config;
    config.session = "BOOK";
    publisher_ = std::make_unique<FeedPublisher>(*sink_, config);
    publisher_->start(now_);
    ASSERT_TRUE(wait_for([&] { return book_->stats().started; }));
    // Nothing more is sent, so after 50 ms the reader says the feed went silent.
    ASSERT_TRUE(wait_for([&] { return book_->stats().silent; }));
    EXPECT_GE(book_->stats().receiver.silences, 1U);
}

TEST(FeedBookConfigTest, BadConfigsAreRefused) {
    FeedBookConfig c = book_config("239.255.79.13", "239.255.79.14");
    c.wait_ms = 0;
    EXPECT_THROW(FeedBook{c}, std::invalid_argument);
    c = book_config("10.0.0.1", "239.255.79.14");  // Not a multicast group.
    EXPECT_THROW(FeedBook{c}, std::invalid_argument);
    c = book_config("239.255.79.13", "239.255.79.14");
    c.receiver.interface_name = "nosuchif0";
    EXPECT_THROW(FeedBook{c}, std::invalid_argument);
}

TEST(FeedBookStatusTest, StatusNames) {
    EXPECT_STREQ(to_string(SymbolStatus::Unknown), "unknown");
    EXPECT_STREQ(to_string(SymbolStatus::Live), "live");
    EXPECT_STREQ(to_string(SymbolStatus::Stale), "stale");
}

}  // namespace
}  // namespace litbook::client
