#include "litbook/feed/feed_handler.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <stdexcept>
#include <vector>

#include "feed/feed_test_support.hpp"

namespace litbook::feed {
namespace {

using testing::Bytes;
using testing::level_event;
using testing::RecordingSink;
using testing::ticker;
using testing::trade_event;

constexpr Timestamp kMillisecond = 1'000'000;

// A publisher with two stocks whose packets are kept, so each test can hand the handler
// any of them in any order, or leave some out.
class FeedHandlerTest : public ::testing::Test {
protected:
    FeedHandlerTest() : publisher_(sink_, publisher_config()) {
        publisher_.add_symbol(
            {.symbol = ticker("AAPL"), .tick_size = 100, .lot_size = 1, .max_levels_per_side = 64});
        publisher_.add_symbol(
            {.symbol = ticker("MSFT"), .tick_size = 100, .lot_size = 1, .max_levels_per_side = 64});
        publisher_.start(0);
    }

    static FeedPublisherConfig publisher_config() {
        FeedPublisherConfig c;
        c.session = "TEST1";
        c.snapshot_spacing = kMillisecond;
        return c;
    }

    static FeedHandlerConfig handler_config(std::size_t buffer = 1024) {
        return FeedHandlerConfig{.session = "TEST1",
                                 .max_symbols = 4,
                                 .max_levels_per_side = 64,
                                 .buffer_capacity = buffer};
    }

    // Publishes `n` level updates spread over both stocks and both sides, then flushes, so
    // they leave in one incremental packet (or a few if n is large).
    void batch(int n) {
        for (int i = 0; i < n; ++i) {
            const auto locate = static_cast<Locate>(i % 2);
            const Side side = (i / 2) % 2 == 0 ? Side::Buy : Side::Sell;
            const Price price =
                (side == Side::Buy ? 10'000 - (step_ % 7) * 100 : 10'100 + (step_ % 7) * 100);
            const bool remove = step_ % 5 == 4;
            const auto step = static_cast<std::uint32_t>(step_);
            const AggregateQty qty = remove ? 0 : 10 + step;
            const std::uint32_t count = remove ? 0 : 1 + step % 3;
            publisher_.on_event(level_event(locate, side, price, qty, count));
            ++step_;
        }
        publisher_.flush(now_);
    }

    // Sends the next stock's snapshot.
    void snapshot() {
        now_ += kMillisecond;
        publisher_.tick(now_);
    }

    [[nodiscard]] std::size_t sent() const { return sink_.packets.size(); }

    void deliver(FeedHandler& h, std::size_t index) {
        const testing::SentPacket& p = sink_.packets.at(index);
        const PacketStatus status = p.stream == FeedStream::Incremental ? h.on_incremental(p.bytes)
                                                                        : h.on_snapshot(p.bytes);
        EXPECT_EQ(status, PacketStatus::Ok);
    }

    // Delivers every packet from index `from` on, except those in `skip`.
    void deliver_from(FeedHandler& h, std::size_t from, const std::set<std::size_t>& skip = {}) {
        for (std::size_t i = from; i < sent(); ++i) {
            if (!skip.contains(i)) {
                deliver(h, i);
            }
        }
    }

    // True when both stocks are live in the handler and equal to the publisher's copies.
    [[nodiscard]] bool in_step(const FeedHandler& h) const {
        for (Locate l = 0; l < 2; ++l) {
            if (!h.live(l) || !(*h.book(l) == publisher_.copy(l))) {
                return false;
            }
        }
        return !h.recovering();
    }

    RecordingSink sink_;
    FeedPublisher publisher_;
    Timestamp now_ = 0;
    int step_ = 0;
};

TEST_F(FeedHandlerTest, ApplyingEveryPacketInOrderGivesThePublishersBooks) {
    FeedHandler h(handler_config());
    batch(20);
    batch(30);
    deliver_from(h, 0);
    EXPECT_TRUE(in_step(h));
    EXPECT_TRUE(h.started());
    EXPECT_EQ(h.next_expected(), publisher_.next_sequence(FeedStream::Incremental));
    EXPECT_EQ(h.stats().applied, 50U);
    EXPECT_EQ(h.stats().gaps, 0U);
    ASSERT_TRUE(h.directory(1).has_value());
    EXPECT_EQ(litbook::testing::unwrap(h.directory(1)).symbol, ticker("MSFT"));
    EXPECT_FALSE(h.book(0)->empty());
}

TEST_F(FeedHandlerTest, ADuplicatedPacketChangesNothing) {
    FeedHandler h(handler_config());
    batch(10);
    deliver_from(h, 0);
    deliver(h, 1);
    EXPECT_EQ(h.stats().duplicates, 10U);
    EXPECT_TRUE(in_step(h));
}

TEST_F(FeedHandlerTest, APacketThatOverlapsTheLastOneAppliesOnlyItsNewMessages) {
    const SessionName session = testing::unwrap_session("TEST1");
    PacketBuilder builder(session, kDefaultPacketLength);
    const auto update = [](Price price) {
        return LevelUpdateMessage{
            .locate = 0, .side = Side::Buy, .price = price, .total_qty = 1, .order_count = 1};
    };
    builder.start(1);
    ASSERT_TRUE(builder.append(update(100)));
    ASSERT_TRUE(builder.append(update(200)));
    const Bytes first(builder.finish().begin(), builder.finish().end());
    builder.start(2);
    ASSERT_TRUE(builder.append(update(200)));
    ASSERT_TRUE(builder.append(update(300)));
    ASSERT_TRUE(builder.append(update(400)));
    const Bytes second(builder.finish().begin(), builder.finish().end());

    FeedHandler h(handler_config());
    EXPECT_EQ(h.on_incremental(first), PacketStatus::Ok);
    EXPECT_EQ(h.on_incremental(second), PacketStatus::Ok);
    EXPECT_EQ(h.stats().duplicates, 1U);
    EXPECT_EQ(h.stats().applied, 4U);
    EXPECT_EQ(h.next_expected(), 5U);
    EXPECT_EQ(h.book(0)->level_count(Side::Buy), 4U);
}

TEST_F(FeedHandlerTest, AGapIsRecoveredFromSnapshotsAndBufferedUpdates) {
    FeedHandler h(handler_config());
    batch(10);
    deliver_from(h, 0);
    ASSERT_TRUE(in_step(h));

    batch(10);
    const std::size_t lost = sent() - 1;
    snapshot();  // Stock 0, taken after the lost packet.
    const std::size_t snapshot_0 = sent() - 1;
    batch(10);
    const std::size_t after = sent() - 1;

    deliver(h, after);  // The handler sees the gap here.
    EXPECT_TRUE(h.recovering());
    EXPECT_FALSE(h.live(0));
    EXPECT_FALSE(h.live(1));
    EXPECT_EQ(h.stats().gaps, 1U);
    EXPECT_EQ(h.buffered(), 10U);

    // The snapshot of stock 0 arrives late, after the newer updates, so the buffered ones
    // for stock 0 are applied on top of it.
    deliver(h, snapshot_0);
    EXPECT_TRUE(h.live(0));
    EXPECT_FALSE(h.live(1));
    EXPECT_EQ(*h.book(0), publisher_.copy(0));
    EXPECT_EQ(h.stats().replayed, 5U);

    snapshot();  // Stock 1.
    deliver(h, sent() - 1);
    EXPECT_TRUE(in_step(h));
    EXPECT_EQ(h.stats().snapshots_applied, 2U);
    EXPECT_EQ(h.stats().recoveries, 1U);
    EXPECT_EQ(h.buffered(), 0U);
    EXPECT_NE(lost, after);

    // Afterwards the incremental stream alone keeps it right again.
    batch(12);
    deliver(h, sent() - 1);
    EXPECT_TRUE(in_step(h));
}

TEST_F(FeedHandlerTest, ALateJoinerRecoversWithoutTheStartOfTheStream) {
    batch(40);
    const std::size_t join = sent();
    batch(10);
    snapshot();
    snapshot();
    batch(6);
    FeedHandler h(handler_config());
    deliver_from(h, join);
    EXPECT_TRUE(in_step(h));
    EXPECT_FALSE(h.started());  // It never saw Start of Messages.
    EXPECT_EQ(h.symbol_count(), 2U);
    EXPECT_EQ(h.stats().gaps, 1U);
    ASSERT_TRUE(h.directory(0).has_value());  // Learned from the snapshot stream.
}

TEST_F(FeedHandlerTest, ASnapshotFromBeforeTheGapIsTooOld) {
    batch(10);
    snapshot();
    const std::size_t old_snapshot = sent() - 1;
    batch(10);
    const std::size_t lost = sent() - 1;
    batch(10);

    FeedHandler h(handler_config());
    for (std::size_t i = 0; i < old_snapshot; ++i) {
        deliver(h, i);
    }
    deliver(h, lost + 1);  // Gap.
    deliver(h, old_snapshot);
    EXPECT_FALSE(h.live(0));
    EXPECT_EQ(h.stats().snapshots_too_old, 1U);

    snapshot();
    snapshot();
    snapshot();
    deliver_from(h, lost + 2);
    EXPECT_TRUE(in_step(h));
}

TEST_F(FeedHandlerTest, ASnapshotNewerThanTheIncrementalStreamSkipsWhatItIncludes) {
    FeedHandler h(handler_config());
    batch(10);
    deliver_from(h, 0);
    batch(10);
    const std::size_t lost = sent() - 1;
    batch(10);
    const std::size_t shows_gap = sent() - 1;
    batch(10);
    const std::size_t late = sent() - 1;
    snapshot();
    snapshot();

    // The snapshots include the late packet, and they arrive before it does.
    deliver(h, shows_gap);
    deliver_from(h, late + 1);
    EXPECT_TRUE(in_step(h));
    EXPECT_GT(h.through(0), h.next_expected() - 1);

    // Every message of the late packet is already in the books, so it changes nothing.
    const std::uint64_t applied = h.stats().applied;
    deliver(h, late);
    EXPECT_EQ(h.stats().applied, applied);
    EXPECT_EQ(h.stats().duplicates, 10U);
    EXPECT_TRUE(in_step(h));

    // The lost packet turns up last, and it is older than everything.
    deliver(h, lost);
    EXPECT_EQ(h.stats().duplicates, 20U);
    EXPECT_TRUE(in_step(h));
}

TEST_F(FeedHandlerTest, AGapInTheSnapshotStreamBreaksTheSnapshotBeingBuilt) {
    // Enough levels that one stock's snapshot needs several packets.
    for (int i = 0; i < 60; ++i) {
        publisher_.on_event(level_event(0, Side::Buy, Price{100} * (i + 1), 5, 1));
    }
    publisher_.flush(0);
    const std::size_t lost = sent() - 1;
    const std::size_t first_part = sent();
    snapshot();  // Stock 0.
    ASSERT_GE(sent() - first_part, 2U);
    batch(2);
    const std::size_t shows_gap = sent() - 1;
    snapshot();  // Stock 1.
    const std::size_t stock_1 = sent() - 1;

    FeedHandler h(handler_config());
    for (std::size_t i = 0; i < lost; ++i) {
        deliver(h, i);
    }
    deliver(h, shows_gap);
    deliver(h, first_part);  // Only the first part of stock 0's snapshot.
    deliver(h, stock_1);
    EXPECT_FALSE(h.live(0));
    EXPECT_TRUE(h.live(1));
    EXPECT_EQ(h.stats().snapshot_gaps, 1U);
    EXPECT_EQ(h.stats().snapshots_broken, 1U);

    const std::size_t next = sent();
    snapshot();  // Stock 0 again.
    deliver_from(h, next);
    EXPECT_TRUE(in_step(h));
}

TEST_F(FeedHandlerTest, ASecondGapEmptiesTheBuffer) {
    FeedHandler h(handler_config());
    batch(4);
    deliver_from(h, 0);
    batch(4);  // Lost.
    batch(4);
    deliver(h, sent() - 1);
    EXPECT_EQ(h.buffered(), 4U);
    batch(4);  // Lost.
    batch(6);
    deliver(h, sent() - 1);
    EXPECT_EQ(h.stats().gaps, 2U);
    // The first four buffered updates are older than the second gap, so they are useless.
    EXPECT_EQ(h.buffered(), 6U);
}

// Two hand built snapshot streams that the publisher never sends, each of which must be
// refused by its own check.
class BrokenSnapshotTest : public ::testing::Test {
protected:
    BrokenSnapshotTest() : builder_(testing::unwrap_session("TEST1"), kDefaultPacketLength) {
        // An incremental packet that starts at 5 makes stock 0 stale, as of sequence 4.
        builder_.start(5);
        EXPECT_TRUE(builder_.append(SystemEvent{}));
        EXPECT_EQ(handler_.on_incremental(builder_.finish()), PacketStatus::Ok);
        EXPECT_FALSE(handler_.live(0));
    }

    static LevelUpdateMessage level(Price price) {
        return LevelUpdateMessage{
            .locate = 0, .side = Side::Buy, .price = price, .total_qty = 3, .order_count = 1};
    }
    static SnapshotStart start(std::uint32_t levels) {
        return SnapshotStart{.locate = 0, .as_of = 5, .level_count = levels, .symbol_count = 1};
    }

    void send(Sequence first, const std::vector<FeedMessage>& messages) {
        builder_.start(first);
        for (const FeedMessage& m : messages) {
            ASSERT_TRUE(builder_.append(m));
        }
        EXPECT_EQ(handler_.on_snapshot(builder_.finish()), PacketStatus::Ok);
    }

    FeedHandler handler_{FeedHandlerConfig{.session = "TEST1", .max_symbols = 4}};
    PacketBuilder builder_;
};

TEST_F(BrokenSnapshotTest, AWrongLevelCountIsRefused) {
    send(1, {start(3), level(100), level(200), SnapshotEnd{.locate = 0, .as_of = 5}});
    EXPECT_FALSE(handler_.live(0));
    EXPECT_EQ(handler_.stats().snapshots_broken, 1U);
    // One price twice. The book holds one level, as the count says, but more messages
    // arrived than the count promised.
    send(5, {start(1), level(100), level(100), SnapshotEnd{.locate = 0, .as_of = 5}});
    EXPECT_FALSE(handler_.live(0));
    EXPECT_EQ(handler_.stats().snapshots_broken, 2U);
    send(9, {start(1), level(100), SnapshotEnd{.locate = 0, .as_of = 5}});
    EXPECT_TRUE(handler_.live(0));
    EXPECT_FALSE(handler_.recovering());
}

TEST_F(BrokenSnapshotTest, AHoleIsRefusedEvenWhenTheCountMatches) {
    // Sequence 3 never arrives. The two levels that did arrive match the count, but the
    // missing message could have been anything, so the snapshot cannot be trusted.
    send(1, {start(2), level(100)});
    send(4, {level(300), SnapshotEnd{.locate = 0, .as_of = 5}});
    EXPECT_FALSE(handler_.live(0));
    EXPECT_EQ(handler_.stats().snapshot_gaps, 1U);
    EXPECT_EQ(handler_.stats().snapshots_broken, 1U);
}

TEST_F(FeedHandlerTest, ABufferOverflowNeedsANewerSnapshot) {
    FeedHandler h(handler_config(4));
    batch(4);
    deliver_from(h, 0);
    batch(4);
    const std::size_t lost = sent() - 1;
    snapshot();
    const std::size_t old_snapshot = sent() - 1;
    batch(10);  // More updates than the buffer holds.
    deliver(h, sent() - 1);
    EXPECT_GE(h.stats().buffer_overflows, 1U);
    EXPECT_LE(h.buffered(), 4U);

    deliver(h, old_snapshot);
    EXPECT_EQ(h.stats().snapshots_too_old, 1U);
    EXPECT_NE(lost, old_snapshot);

    const std::size_t next = sent();
    snapshot();
    snapshot();
    deliver_from(h, next);
    EXPECT_TRUE(in_step(h));
}

TEST_F(FeedHandlerTest, AHeartbeatShowsAGapWhenTheMarketIsQuiet) {
    FeedHandler h(handler_config());
    batch(6);
    const std::size_t lost = sent() - 1;
    for (std::size_t i = 0; i < lost; ++i) {
        deliver(h, i);
    }
    publisher_.tick(5'000'000'000);  // Sends a snapshot and, after five quiet seconds, heartbeats.
    for (std::size_t i = lost + 1; i < sent(); ++i) {
        if (sink_.packets[i].stream == FeedStream::Incremental) {
            deliver(h, i);
        }
    }
    EXPECT_EQ(h.stats().gaps, 1U);
    EXPECT_TRUE(h.recovering());
    EXPECT_EQ(h.next_expected(), publisher_.next_sequence(FeedStream::Incremental));
}

TEST_F(FeedHandlerTest, EndOfSessionIsNoticed) {
    FeedHandler h(handler_config());
    batch(4);
    publisher_.end_session(10);
    deliver_from(h, 0);
    EXPECT_TRUE(h.ended());
    EXPECT_TRUE(in_step(h));
}

TEST_F(FeedHandlerTest, TradesAreCountedForLiveStocksAndLostForStaleOnes) {
    FeedHandler h(handler_config());
    publisher_.on_event(trade_event(0, Side::Buy, 10'000, 5, 1));
    publisher_.flush(0);
    deliver_from(h, 0);
    EXPECT_EQ(h.stats().trades, 1U);

    publisher_.on_event(trade_event(1, Side::Sell, 10'000, 5, 2));
    publisher_.flush(0);
    const std::size_t lost = sent() - 1;
    publisher_.on_event(trade_event(1, Side::Sell, 10'000, 5, 3));
    publisher_.flush(0);
    deliver(h, lost + 1);
    EXPECT_EQ(h.stats().trades_lost, 1U);
}

TEST_F(FeedHandlerTest, DamagedAndForeignPacketsAreDropped) {
    FeedHandler h(handler_config());
    batch(4);
    Bytes damaged = sink_.packets[1].bytes;
    damaged.pop_back();
    EXPECT_EQ(h.on_incremental(damaged), PacketStatus::Truncated);
    EXPECT_EQ(h.on_incremental(Bytes(5)), PacketStatus::TooShort);
    EXPECT_EQ(h.stats().malformed, 2U);
    EXPECT_EQ(h.stats().incremental_packets, 0U);

    FeedHandler other(FeedHandlerConfig{.session = "OTHER"});
    EXPECT_EQ(other.on_incremental(sink_.packets[0].bytes), PacketStatus::Ok);
    EXPECT_EQ(other.stats().wrong_session, 1U);
    EXPECT_EQ(other.stats().applied, 0U);
    EXPECT_FALSE(other.started());
}

TEST_F(FeedHandlerTest, MessagesOnTheWrongStreamOrForUnknownStocksAreCounted) {
    const SessionName session = testing::unwrap_session("TEST1");
    PacketBuilder builder(session, kDefaultPacketLength);
    builder.start(1);
    ASSERT_TRUE(builder.append(SnapshotEnd{.locate = 0, .as_of = 0}));
    ASSERT_TRUE(builder.append(LevelUpdateMessage{
        .locate = 9, .side = Side::Buy, .price = 1, .total_qty = 1, .order_count = 1}));
    ASSERT_TRUE(builder.append(TradeMessage{.locate = 9, .price = 1, .qty = 1}));
    const Bytes incremental(builder.finish().begin(), builder.finish().end());
    builder.start(1);
    ASSERT_TRUE(builder.append(TradeMessage{.locate = 0, .price = 1, .qty = 1}));
    ASSERT_TRUE(builder.append(SystemEvent{}));
    const Bytes snapshot(builder.finish().begin(), builder.finish().end());

    FeedHandler h(handler_config());
    EXPECT_EQ(h.on_incremental(incremental), PacketStatus::Ok);
    EXPECT_EQ(h.on_snapshot(snapshot), PacketStatus::Ok);
    EXPECT_EQ(h.stats().unexpected, 3U);
    EXPECT_EQ(h.stats().bad_locate, 2U);
    EXPECT_EQ(h.book(9), nullptr);
}

TEST_F(FeedHandlerTest, AFullBookMakesTheStockStale) {
    FeedHandler h(FeedHandlerConfig{
        .session = "TEST1", .max_symbols = 4, .max_levels_per_side = 2, .buffer_capacity = 64});
    for (int i = 0; i < 3; ++i) {
        publisher_.on_event(level_event(0, Side::Buy, Price{100} * (i + 1), 5, 1));
    }
    publisher_.flush(0);
    deliver_from(h, 0);
    EXPECT_EQ(h.stats().book_full, 1U);
    EXPECT_FALSE(h.live(0));
    EXPECT_TRUE(h.live(1));
    EXPECT_TRUE(h.recovering());
}

TEST(FeedHandlerSetupTest, BadConfigurationThrows) {
    EXPECT_THROW(FeedHandler{FeedHandlerConfig{.session = ""}}, std::invalid_argument);
    EXPECT_THROW(FeedHandler{FeedHandlerConfig{.max_symbols = 0}}, std::invalid_argument);
    EXPECT_THROW(FeedHandler{FeedHandlerConfig{.max_symbols = 70'000}}, std::invalid_argument);
    EXPECT_THROW(FeedHandler{FeedHandlerConfig{.max_levels_per_side = 0}}, std::invalid_argument);
    EXPECT_THROW(FeedHandler{FeedHandlerConfig{.buffer_capacity = 0}}, std::invalid_argument);
    const FeedHandler h(FeedHandlerConfig{});
    EXPECT_FALSE(h.recovering());
    EXPECT_TRUE(h.live(0));
    EXPECT_FALSE(h.live(5'000));
    EXPECT_EQ(h.next_expected(), 1U);
}

}  // namespace
}  // namespace litbook::feed
