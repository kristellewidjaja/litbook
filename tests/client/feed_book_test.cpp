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
#include <stdexcept>
#include <string>
#include <thread>
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

class FeedBookTest : public ::testing::Test {
protected:
    // Makes the book first, because the publisher must be told the ports it got.
    void make(const char* incremental, const char* snapshot, int receive_buffer = 0) {
        FeedBookConfig c = book_config(incremental, snapshot);
        c.receiver.receive_buffer = receive_buffer;
        book_ = std::make_unique<FeedBook>(c);
        FeedNetworkConfig network;
        network.incremental = {.address = incremental, .port = book_->incremental_port()};
        network.snapshot = {.address = snapshot, .port = book_->snapshot_port()};
        sink_ = std::make_unique<UdpFeedSink>(network);
        FeedPublisherConfig config;
        config.session = "BOOK";
        config.snapshot_spacing = kSpacing;
        publisher_ = std::make_unique<FeedPublisher>(*sink_, config);
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

// A receive buffer far too small for a burst makes the kernel drop packets. The book must
// say a stock is stale while it waits, and come back live from the snapshots.
TEST_F(FeedBookTest, LostPacketsMakeStocksStaleUntilSnapshotsRepairThem) {
    make("239.255.79.7", "239.255.79.8", 4096);
    publisher_->start(now_);
    ASSERT_TRUE(wait_for([&] { return book_->symbols().size() == 2; }));
    // The reader thread cannot keep up with this many packets in a 4 KB buffer.
    for (int i = 0; i < 4'000; ++i) {
        const auto locate = static_cast<feed::Locate>(i % 2);
        const Price price = (900 + (i % 40)) * kTick;
        level(locate, Side::Buy, price, 100U + static_cast<unsigned>(i), 1);
        if (i % 10 == 9) {
            publisher_->flush(now_);
        }
    }
    publisher_->flush(now_);
    // A final packet after a pause shows the book where the stream is now.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    level(1, Side::Sell, 1'500 * kTick, 5, 1);
    publisher_->flush(now_);

    ASSERT_TRUE(wait_for([&] { return book_->stats().handler.gaps >= 1; }));
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
