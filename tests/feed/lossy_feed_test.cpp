// End to end tests of the feed over a channel that loses packets on purpose. Random orders
// go through the real order entry layer and books, their market data goes through the
// publisher, and the packets reach a feed handler through a LossyChannel. Whatever is
// lost on the way, the handler must end up with exactly the exchange's books.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/feed/feed_handler.hpp"
#include "litbook/feed/feed_publisher.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "book/test_limits.hpp"
#include "feed/feed_forwarder.hpp"
#include "feed/feed_test_support.hpp"
#include "feed/lossy_channel.hpp"
#include "protocol/order_entry_stream.hpp"
#include "support/unwrap.hpp"

namespace litbook::feed {
namespace {

using testing::copy_matches_book;
using testing::DiscardReplies;
using testing::FeedForwarder;
using testing::LossRates;
using testing::LossyChannel;

constexpr Timestamp kMicrosecond = 1'000;

struct MarketRun {
    std::uint64_t seed;
    LossRates rates;
    std::size_t buffer_capacity;
    const char* name;
};

void PrintTo(const MarketRun& run, std::ostream* os) {
    *os << run.name;
}

class LossyFeedTest : public ::testing::TestWithParam<MarketRun> {};

TEST_P(LossyFeedTest, TheHandlerEndsWithTheExchangesBooks) {
    const MarketRun run = GetParam();
    constexpr std::size_t kStocks = litbook::testing::kStreamStocks.size();

    FeedHandler handler(FeedHandlerConfig{.session = "LOSSY",
                                          .max_symbols = 4,
                                          .max_levels_per_side = 256,
                                          .buffer_capacity = run.buffer_capacity});
    LossyChannel channel(handler, run.seed, run.rates);
    FeedPublisherConfig publisher_config;
    publisher_config.session = "LOSSY";
    publisher_config.snapshot_spacing = 20 * kMicrosecond;
    FeedPublisher publisher(channel, publisher_config);
    FeedForwarder forwarder(publisher);
    DiscardReplies replies;
    protocol::OrderEntry<SortedOrderBook> entry(
        replies, {.max_owners = 4, .max_live_orders = 4096, .max_symbols = 2});
    entry.set_market_data_sink(&forwarder);
    for (const char* name : litbook::testing::kStreamStocks) {
        const Ticker ticker = litbook::testing::unwrap(make_ticker(name));
        entry.add_symbol(ticker, litbook::testing::with_test_limits(BookConfig{.tick_size = 100}));
        publisher.add_symbol(
            {.symbol = ticker, .tick_size = 100, .lot_size = 1, .max_levels_per_side = 256});
    }

    Timestamp now = 0;
    publisher.start(now);
    const auto stream = litbook::testing::order_entry_stream(run.seed, 30'000);
    std::uint64_t in_step_checks = 0;
    std::uint64_t frames = 0;
    for (const litbook::testing::InboundFrame& frame : stream) {
        now += kMicrosecond;
        forwarder.set_now(now);
        (void)entry.on_frame(frame.owner, frame.bytes, now);
        ++frames;
        // The publisher thread flushes whenever its queue runs dry, which here is after
        // one to four orders, and ticks between batches.
        if (frames % (1 + run.seed % 4) == 0) {
            publisher.flush(now);
        }
        if (frames % 10 == 0) {
            publisher.tick(now);
        }
        // Every live book must stay valid at all times.
        for (Locate l = 0; l < kStocks; ++l) {
            ASSERT_TRUE(handler.book(l)->check_invariants());
        }
        // When the handler has every message the publisher sent and is not recovering, its
        // books must equal the publisher's copies right now, not only at the end.
        if (!handler.recovering() && !channel.holding_incremental() &&
            handler.next_expected() == publisher.next_sequence(FeedStream::Incremental)) {
            for (Locate l = 0; l < kStocks; ++l) {
                ASSERT_EQ(*handler.book(l), publisher.copy(l)) << "frame " << frames;
            }
            ++in_step_checks;
        }
    }

    // The network heals. A packet lost at the very end shows up only through the next
    // heartbeat, so a quiet second passes. After two full snapshot cycles every stock must
    // be live again.
    channel.set_lossless();
    publisher.flush(now);
    now += publisher_config.heartbeat_interval;
    publisher.tick(now);
    for (int i = 0; i < 4 * static_cast<int>(kStocks); ++i) {
        now += publisher_config.snapshot_spacing;
        publisher.tick(now);
    }
    EXPECT_FALSE(handler.recovering());
    EXPECT_EQ(handler.next_expected(), publisher.next_sequence(FeedStream::Incremental));
    for (Locate l = 0; l < kStocks; ++l) {
        const auto id = static_cast<SymbolId>(l);
        EXPECT_TRUE(handler.live(l));
        EXPECT_EQ(*handler.book(l), publisher.copy(l));
        EXPECT_TRUE(copy_matches_book(publisher.copy(l), *entry.engine().book(id)));
    }
    EXPECT_EQ(publisher.stats().copy_full, 0U);
    EXPECT_EQ(channel.stats().not_ok, 0U);

    // The run must have exercised what it claims to test.
    const FeedHandlerStats& s = handler.stats();
    EXPECT_GT(publisher.stats().events, 10'000U);
    EXPECT_GT(in_step_checks, 100U);
    if (run.rates.drop > 0) {
        EXPECT_GT(s.gaps, 20U);
        EXPECT_GT(s.snapshot_gaps, 5U);
        EXPECT_GT(s.snapshots_applied, 20U);
        EXPECT_GT(s.recoveries, 10U);
    }
    // Buffered updates are applied after a snapshot only when the snapshot arrives after
    // newer incremental messages, which needs the snapshot stream to run behind, and a
    // buffer big enough to keep those messages until the snapshot comes.
    if (run.rates.drop > 0 && run.rates.snapshot_lag > 0 && run.buffer_capacity >= 64) {
        EXPECT_GT(s.replayed, 0U);
    }
    if (run.rates.duplicate > 0 || run.rates.hold > 0) {
        EXPECT_GT(s.duplicates, 0U);
    }
    if (run.buffer_capacity < 64) {
        EXPECT_GT(s.buffer_overflows, 0U);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Runs, LossyFeedTest,
    ::testing::Values(
        MarketRun{.seed = 1, .rates = {}, .buffer_capacity = 4096, .name = "NoLoss"},
        MarketRun{.seed = 2, .rates = {.drop = 20}, .buffer_capacity = 4096, .name = "Drops"},
        MarketRun{.seed = 3,
                  .rates = {.duplicate = 50, .hold = 50},
                  .buffer_capacity = 4096,
                  .name = "DuplicatesAndReorders"},
        MarketRun{.seed = 4,
                  .rates = {.drop = 30, .duplicate = 30, .hold = 30, .snapshot_lag = 3},
                  .buffer_capacity = 4096,
                  .name = "Everything"},
        MarketRun{.seed = 5,
                  .rates = {.drop = 50, .duplicate = 10, .hold = 10, .snapshot_lag = 8},
                  .buffer_capacity = 16,
                  .name = "TinyBuffer"}),
    [](const ::testing::TestParamInfo<MarketRun>& param_info) {
        return std::string(param_info.param.name);
    });

}  // namespace
}  // namespace litbook::feed
