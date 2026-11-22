#include "litbook/feed/feed_publisher.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "feed/feed_test_support.hpp"

namespace litbook::feed {
namespace {

using testing::decode_packet;
using testing::DecodedPacket;
using testing::level_event;
using testing::messages_of;
using testing::RecordingSink;
using testing::ticker;
using testing::trade_event;

constexpr Timestamp kSecond = 1'000'000'000;

class FeedPublisherTest : public ::testing::Test {
protected:
    FeedPublisherTest() : publisher_(sink_, config()) {
        publisher_.add_symbol(
            {.symbol = ticker("AAPL"), .tick_size = 100, .lot_size = 1, .max_levels_per_side = 64});
        publisher_.add_symbol({.symbol = ticker("MSFT"),
                               .tick_size = 100,
                               .lot_size = 10,
                               .max_levels_per_side = 64});
    }

    static FeedPublisherConfig config() {
        FeedPublisherConfig c;
        c.session = "TEST1";
        c.snapshot_spacing = 1'000'000;
        return c;
    }

    RecordingSink sink_;
    FeedPublisher publisher_;
};

TEST_F(FeedPublisherTest, StartSendsStartOfMessagesAndTheDirectory) {
    publisher_.start(500);
    ASSERT_EQ(sink_.packets.size(), 1U);
    EXPECT_EQ(sink_.packets[0].stream, FeedStream::Incremental);
    const DecodedPacket p = decode_packet(sink_.packets[0].bytes);
    EXPECT_EQ(std::string(p.header.session.begin(), p.header.session.end()), "     TEST1");
    EXPECT_EQ(p.header.sequence, 1U);
    ASSERT_EQ(p.messages.size(), 3U);
    EXPECT_EQ(p.messages[0],
              FeedMessage(SystemEvent{.timestamp = 500, .code = SystemEventCode::StartOfMessages}));
    EXPECT_EQ(p.messages[2], FeedMessage(SymbolDirectory{.timestamp = 500,
                                                         .locate = 1,
                                                         .symbol = ticker("MSFT"),
                                                         .tick_size = 100,
                                                         .lot_size = 10}));
    EXPECT_EQ(publisher_.next_sequence(FeedStream::Incremental), 4U);
    EXPECT_EQ(publisher_.next_sequence(FeedStream::Snapshot), 1U);
    EXPECT_TRUE(publisher_.started());
}

TEST_F(FeedPublisherTest, EventsWaitInOnePacketUntilFlush) {
    publisher_.start(0);
    publisher_.on_event(trade_event(0, Side::Buy, 500'000, 30, 9, 1'000));
    publisher_.on_event(level_event(0, Side::Sell, 500'000, 70, 2, 1'000));
    EXPECT_EQ(sink_.packets.size(), 1U);  // Nothing new is sent before the flush.
    publisher_.flush(2'000);
    ASSERT_EQ(sink_.packets.size(), 2U);
    const DecodedPacket p = decode_packet(sink_.packets[1].bytes);
    EXPECT_EQ(p.header.sequence, 4U);
    ASSERT_EQ(p.messages.size(), 2U);
    EXPECT_EQ(p.messages[0], FeedMessage(TradeMessage{.timestamp = 1'000,
                                                      .locate = 0,
                                                      .aggressor_side = Side::Buy,
                                                      .price = 500'000,
                                                      .qty = 30,
                                                      .match_number = 9}));
    EXPECT_EQ(p.messages[1], FeedMessage(LevelUpdateMessage{.timestamp = 1'000,
                                                            .locate = 0,
                                                            .side = Side::Sell,
                                                            .price = 500'000,
                                                            .total_qty = 70,
                                                            .order_count = 2}));
    EXPECT_EQ(publisher_.copy(0).find(Side::Sell, 500'000),
              (PriceLevel{.price = 500'000, .total_qty = 70, .order_count = 2}));
    EXPECT_EQ(publisher_.stats().events, 2U);

    publisher_.flush(3'000);
    EXPECT_EQ(sink_.packets.size(), 2U);  // An empty packet is never sent.
}

TEST_F(FeedPublisherTest, AFullPacketGoesOutAndSequenceNumbersStayContiguous) {
    publisher_.start(0);
    for (int i = 0; i < 100; ++i) {
        publisher_.on_event(level_event(0, Side::Buy, Price{100} * (i + 1), 10, 1));
    }
    publisher_.flush(0);
    Sequence expected = 1;
    std::size_t messages = 0;
    for (const auto& packet : sink_.of(FeedStream::Incremental)) {
        EXPECT_LE(packet.size(), kDefaultPacketLength);
        const DecodedPacket p = decode_packet(packet);
        EXPECT_EQ(p.header.sequence, expected);
        expected += p.messages.size();
        messages += p.messages.size();
    }
    EXPECT_EQ(messages, 103U);
    EXPECT_GE(sink_.of(FeedStream::Incremental).size(), 4U);
    EXPECT_EQ(expected, publisher_.next_sequence(FeedStream::Incremental));
}

TEST_F(FeedPublisherTest, ASnapshotIsTheCopyAsOfTheLastIncrementalMessage) {
    publisher_.start(0);
    publisher_.on_event(level_event(0, Side::Buy, 9'900, 10, 1));
    publisher_.on_event(level_event(0, Side::Buy, 10'000, 20, 2));
    publisher_.on_event(level_event(0, Side::Sell, 10'200, 5, 1));
    publisher_.on_event(level_event(0, Side::Sell, 10'100, 6, 1));
    publisher_.tick(7);  // Not flushed yet, so the snapshot must send it first.

    ASSERT_EQ(sink_.packets.size(), 3U);
    EXPECT_EQ(sink_.packets[1].stream, FeedStream::Incremental);
    EXPECT_EQ(sink_.packets[2].stream, FeedStream::Snapshot);
    const DecodedPacket s = decode_packet(sink_.packets[2].bytes);
    EXPECT_EQ(s.header.sequence, 1U);
    ASSERT_EQ(s.messages.size(), 7U);
    EXPECT_TRUE(std::holds_alternative<SymbolDirectory>(s.messages[0]));
    EXPECT_EQ(s.messages[1],
              FeedMessage(SnapshotStart{
                  .timestamp = 7, .locate = 0, .as_of = 7, .level_count = 4, .symbol_count = 2}));
    // Bids from best to worst, then asks from best to worst.
    EXPECT_EQ(std::get<LevelUpdateMessage>(s.messages[2]).price, 10'000);
    EXPECT_EQ(std::get<LevelUpdateMessage>(s.messages[3]).price, 9'900);
    EXPECT_EQ(std::get<LevelUpdateMessage>(s.messages[4]).price, 10'100);
    EXPECT_EQ(std::get<LevelUpdateMessage>(s.messages[5]).price, 10'200);
    EXPECT_EQ(s.messages[6], FeedMessage(SnapshotEnd{.timestamp = 7, .locate = 0, .as_of = 7}));
    EXPECT_EQ(publisher_.stats().snapshots, 1U);
}

TEST_F(FeedPublisherTest, RestoredLevelsAreSentInTheFirstSnapshotAndNowhereElse) {
    publisher_.restore(level_event(0, Side::Buy, 10'000, 20, 2));
    publisher_.restore(level_event(0, Side::Sell, 10'100, 6, 1));
    publisher_.restore(level_event(0, Side::Sell, 10'100, 0, 0));  // Removed again.
    publisher_.restore(level_event(0, Side::Sell, 10'200, 5, 1));
    publisher_.restore(trade_event(0, Side::Buy, 10'000, 3, 1));  // Trades are ignored.
    publisher_.restore(level_event(9, Side::Buy, 10'000, 1, 1));  // No such stock.
    EXPECT_TRUE(sink_.packets.empty());
    EXPECT_EQ(publisher_.stats().events, 0U);

    publisher_.start(0);
    publisher_.tick(7);
    ASSERT_EQ(sink_.packets.size(), 2U);
    EXPECT_EQ(decode_packet(sink_.packets[0].bytes).messages.size(), 3U);  // Only the start.
    const DecodedPacket s = decode_packet(sink_.packets[1].bytes);
    ASSERT_EQ(s.messages.size(), 5U);
    EXPECT_EQ(std::get<SnapshotStart>(s.messages[1]).level_count, 2U);
    EXPECT_EQ(std::get<LevelUpdateMessage>(s.messages[2]).price, 10'000);
    EXPECT_EQ(std::get<LevelUpdateMessage>(s.messages[2]).total_qty, 20U);
    EXPECT_EQ(std::get<LevelUpdateMessage>(s.messages[3]).price, 10'200);
}

TEST_F(FeedPublisherTest, ARenamedSessionIsWhatEveryPacketCarries) {
    publisher_.rename_session("42");
    EXPECT_EQ(publisher_.session(), "42");
    publisher_.start(0);
    publisher_.tick(7);
    ASSERT_EQ(sink_.packets.size(), 2U);
    for (const auto& packet : sink_.packets) {
        const DecodedPacket p = decode_packet(packet.bytes);
        EXPECT_EQ(std::string(p.header.session.begin(), p.header.session.end()), "        42");
    }
}

TEST_F(FeedPublisherTest, RecoveryCallsAreOnlyAllowedBeforeStart) {
    EXPECT_THROW(publisher_.rename_session(""), std::invalid_argument);
    EXPECT_THROW(publisher_.rename_session("TOO-LONG-NAME"), std::invalid_argument);
    EXPECT_EQ(publisher_.session(), "TEST1");
    publisher_.start(0);
    EXPECT_THROW(publisher_.rename_session("2"), std::logic_error);
    EXPECT_THROW(publisher_.restore(level_event(0, Side::Buy, 10'000, 1, 1)), std::logic_error);
}

TEST_F(FeedPublisherTest, SnapshotsCycleThroughTheStocksAtTheirSpacing) {
    publisher_.start(0);
    std::vector<Locate> order;
    for (Timestamp t = 0; t <= 5'000'000; t += 250'000) {
        const std::size_t before = sink_.of(FeedStream::Snapshot).size();
        publisher_.tick(t);
        const auto snapshot_packets = sink_.of(FeedStream::Snapshot);
        for (std::size_t i = before; i < snapshot_packets.size(); ++i) {
            for (const FeedMessage& m : decode_packet(snapshot_packets[i]).messages) {
                if (const auto* start = std::get_if<SnapshotStart>(&m)) {
                    order.push_back(start->locate);
                }
            }
        }
    }
    // One snapshot per millisecond, starting at time 0, for 5 ms.
    EXPECT_EQ(order, (std::vector<Locate>{0, 1, 0, 1, 0, 1}));
}

TEST_F(FeedPublisherTest, AQuietStreamSendsHeartbeats) {
    FeedPublisherConfig c = config();
    c.snapshot_spacing = 100 * kSecond;  // Keep snapshots out of the way.
    RecordingSink sink;
    FeedPublisher publisher(sink, c);
    publisher.add_symbol({.symbol = ticker("AAPL")});
    publisher.start(0);
    publisher.tick(1);  // The first snapshot.
    sink.packets.clear();

    publisher.tick(kSecond / 2);
    EXPECT_TRUE(sink.packets.empty());
    publisher.tick(kSecond + 1);
    ASSERT_EQ(sink.packets.size(), 2U);
    const DecodedPacket hb = decode_packet(sink.packets[0].bytes);
    EXPECT_TRUE(hb.header.heartbeat());
    EXPECT_EQ(hb.header.sequence, 3U);  // Start of Messages and one directory came before.
    const DecodedPacket shb = decode_packet(sink.packets[1].bytes);
    EXPECT_TRUE(shb.header.heartbeat());
    EXPECT_EQ(shb.header.sequence, 4U);  // The snapshot had three messages and no levels.
    EXPECT_EQ(publisher.stats().heartbeats[0], 1U);

    // A stream that just sent data does not also send a heartbeat.
    sink.packets.clear();
    publisher.on_event(level_event(0, Side::Buy, 100, 1, 1));
    publisher.flush(2 * kSecond + 5);
    publisher.tick(2 * kSecond + 5);
    ASSERT_EQ(sink.packets.size(), 2U);
    EXPECT_FALSE(decode_packet(sink.packets[0].bytes).header.heartbeat());
    EXPECT_TRUE(decode_packet(sink.packets[1].bytes).header.heartbeat());
    EXPECT_EQ(sink.packets[1].stream, FeedStream::Snapshot);
}

TEST_F(FeedPublisherTest, TheNextDeadlineIsTheNextSnapshotOrHeartbeat) {
    constexpr Timestamp kNever = std::numeric_limits<Timestamp>::max();
    EXPECT_EQ(publisher_.next_deadline(), kNever);  // Nothing is due before start.

    publisher_.start(1'000);
    EXPECT_EQ(publisher_.next_deadline(), 1'000U);  // The first snapshot is due at once.
    publisher_.tick(1'000);
    // The next stock's snapshot comes one spacing later, before any heartbeat.
    EXPECT_EQ(publisher_.next_deadline(), 1'000U + 1'000'000U);
    publisher_.tick(publisher_.next_deadline());
    publisher_.tick(publisher_.next_deadline());
    EXPECT_EQ(publisher_.stats().snapshots, 3U);
    EXPECT_EQ(publisher_.stats().heartbeats[0], 0U);

    publisher_.end_session(5'000'000);
    EXPECT_EQ(publisher_.next_deadline(), kNever);  // Nothing is due after the end either.
}

TEST(FeedPublisherSetupTest, WithoutStocksTheNextDeadlineIsAHeartbeat) {
    RecordingSink sink;
    FeedPublisherConfig c;
    c.heartbeat_interval = kSecond;
    FeedPublisher publisher(sink, c);
    publisher.start(7);
    // The incremental stream sent Start of Messages at 7, and the snapshot stream counts
    // as having sent at 7 too.
    EXPECT_EQ(publisher.next_deadline(), 7 + kSecond);
    publisher.tick(7 + kSecond);
    EXPECT_EQ(publisher.stats().heartbeats[0], 1U);
    EXPECT_EQ(publisher.stats().heartbeats[1], 1U);
    EXPECT_EQ(publisher.next_deadline(), 7 + 2 * kSecond);
}

TEST(FeedPublisherSetupTest, AHugeHeartbeatIntervalDoesNotWrapTheDeadline) {
    RecordingSink sink;
    FeedPublisherConfig c;
    c.heartbeat_interval = std::numeric_limits<Timestamp>::max();
    FeedPublisher publisher(sink, c);
    publisher.start(kSecond);
    EXPECT_EQ(publisher.next_deadline(), std::numeric_limits<Timestamp>::max());
}

TEST_F(FeedPublisherTest, EndOfSessionClosesBothStreams) {
    publisher_.start(0);
    publisher_.on_event(level_event(1, Side::Buy, 100, 1, 1));
    publisher_.end_session(10);
    const auto incremental = sink_.of(FeedStream::Incremental);
    const auto snapshot = sink_.of(FeedStream::Snapshot);
    ASSERT_EQ(incremental.size(), 2U + 3U);
    ASSERT_EQ(snapshot.size(), 3U);
    const DecodedPacket last = decode_packet(incremental[1]);
    EXPECT_EQ(last.messages.back(),
              FeedMessage(SystemEvent{.timestamp = 10, .code = SystemEventCode::EndOfMessages}));
    for (std::size_t i = 2; i < incremental.size(); ++i) {
        const DecodedPacket p = decode_packet(incremental[i]);
        EXPECT_TRUE(p.header.end_of_session());
        EXPECT_EQ(p.header.sequence, publisher_.next_sequence(FeedStream::Incremental));
    }
    EXPECT_TRUE(decode_packet(snapshot[0]).header.end_of_session());
    EXPECT_TRUE(publisher_.ended());

    const std::size_t sent = sink_.packets.size();
    publisher_.on_event(level_event(0, Side::Buy, 100, 1, 1));
    publisher_.tick(10 * kSecond);
    publisher_.end_session(11);
    EXPECT_EQ(sink_.packets.size(), sent);
    EXPECT_EQ(publisher_.stats().ignored, 1U);
}

TEST_F(FeedPublisherTest, BadEventsAreCountedAndNothingIsSent) {
    publisher_.on_event(level_event(0, Side::Buy, 100, 1, 1));
    EXPECT_EQ(publisher_.stats().ignored, 1U);  // Before start.
    publisher_.start(0);
    publisher_.on_event(level_event(9, Side::Buy, 100, 1, 1));
    EXPECT_EQ(publisher_.stats().unknown_symbol, 1U);
    publisher_.on_event(trade_event(0, Side::Buy, -100, 1, 1));
    EXPECT_EQ(publisher_.stats().unencodable, 1U);
    pipeline::MarketEvent stop;
    stop.kind = pipeline::MarketEventKind::Stop;
    publisher_.on_event(stop);
    publisher_.flush(0);
    EXPECT_EQ(sink_.packets.size(), 1U);
    EXPECT_EQ(publisher_.stats().events, 0U);
}

TEST(FeedPublisherSetupTest, CopyFullStopsThatStocksSnapshots) {
    RecordingSink sink;
    FeedPublisher publisher(sink, FeedPublisherConfig{});
    publisher.add_symbol({.symbol = ticker("A"), .max_levels_per_side = 1});
    publisher.start(0);
    publisher.on_event(level_event(0, Side::Buy, 100, 1, 1));
    publisher.on_event(level_event(0, Side::Buy, 200, 1, 1));
    EXPECT_EQ(publisher.stats().copy_full, 1U);
    EXPECT_EQ(publisher.stats().events, 2U);  // The incremental stream still has both.
    publisher.tick(0);
    EXPECT_EQ(publisher.stats().snapshots, 0U);
    EXPECT_TRUE(sink.of(FeedStream::Snapshot).empty());
}

TEST(FeedPublisherSetupTest, ABigSnapshotSpansSeveralPackets) {
    RecordingSink sink;
    FeedPublisher publisher(sink, FeedPublisherConfig{});
    publisher.add_symbol({.symbol = ticker("A"), .max_levels_per_side = 500});
    publisher.start(0);
    for (int i = 1; i <= 200; ++i) {
        publisher.on_event(level_event(0, Side::Buy, i, 1, 1));
        publisher.on_event(level_event(0, Side::Sell, 1'000 + i, 1, 1));
    }
    publisher.tick(0);
    const auto packets = sink.of(FeedStream::Snapshot);
    EXPECT_GT(packets.size(), 5U);
    const std::vector<FeedMessage> all = messages_of(sink, FeedStream::Snapshot);
    ASSERT_EQ(all.size(), 1U + 1U + 400U + 1U);
    EXPECT_EQ(std::get<SnapshotStart>(all[1]).level_count, 400U);
    EXPECT_EQ(std::get<SnapshotEnd>(all.back()).as_of,
              publisher.next_sequence(FeedStream::Incremental) - 1);
}

TEST(FeedPublisherSetupTest, BadConfigurationThrows) {
    RecordingSink sink;
    FeedPublisherConfig bad_session;
    bad_session.session = "toolongname1";
    EXPECT_THROW((FeedPublisher{sink, bad_session}), std::invalid_argument);
    FeedPublisherConfig bad_packet;
    bad_packet.max_packet_length = 10;
    EXPECT_THROW((FeedPublisher{sink, bad_packet}), std::invalid_argument);
    FeedPublisherConfig no_symbols;
    no_symbols.max_symbols = 0;
    EXPECT_THROW((FeedPublisher{sink, no_symbols}), std::invalid_argument);
    FeedPublisherConfig no_heartbeat;
    no_heartbeat.heartbeat_interval = 0;
    EXPECT_THROW((FeedPublisher{sink, no_heartbeat}), std::invalid_argument);

    FeedPublisherConfig one;
    one.max_symbols = 1;
    FeedPublisher publisher(sink, one);
    EXPECT_THROW(publisher.add_symbol({.symbol = ticker("A"), .tick_size = 0}),
                 std::invalid_argument);
    EXPECT_THROW(publisher.add_symbol({.symbol = ticker("A"), .lot_size = 0}),
                 std::invalid_argument);
    EXPECT_THROW(publisher.add_symbol({.symbol = ticker("A"), .max_levels_per_side = 0}),
                 std::invalid_argument);
    EXPECT_EQ(publisher.add_symbol({.symbol = ticker("A")}), 0U);
    EXPECT_THROW(publisher.add_symbol({.symbol = ticker("B")}), std::length_error);
    publisher.start(0);
    FeedPublisher late(sink, FeedPublisherConfig{});
    late.start(0);
    EXPECT_THROW(late.add_symbol({.symbol = ticker("A")}), std::logic_error);
}

}  // namespace
}  // namespace litbook::feed
