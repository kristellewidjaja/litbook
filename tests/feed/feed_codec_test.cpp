#include "litbook/feed/feed_codec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <sstream>
#include <vector>

#include "feed/feed_test_support.hpp"
#include "protocol/frame_builder.hpp"
#include "support/unwrap.hpp"

namespace litbook::feed {
namespace {

using protocol::testing::FrameBuilder;
using testing::Bytes;
using testing::ticker;

// The golden frames are built field by field with FrameBuilder, which writes big-endian
// on its own, so they follow the tables in docs/FEED.md and not the code under test.

Bytes golden_system_event() {
    return FrameBuilder().ch('S').u64(1'000).ch('O').build();
}

Bytes golden_directory() {
    return FrameBuilder().ch('R').u64(2'000).u16(3).text("AAPL    ").u64(100).u32(1).build();
}

Bytes golden_level() {
    return FrameBuilder().ch('L').u64(3'000).u16(1).ch('S').u64(1'234'500).u64(700).u32(3).build();
}

Bytes golden_trade() {
    return FrameBuilder().ch('P').u64(4'000).u16(2).ch('B').u64(500'000).u32(250).u64(77).build();
}

Bytes golden_snapshot_start() {
    return FrameBuilder().ch('Y').u64(5'000).u16(1).u64(1'000'000).u32(12).u16(4).build();
}

Bytes golden_snapshot_end() {
    return FrameBuilder().ch('Z').u64(6'000).u16(1).u64(1'000'000).build();
}

FeedMessage decode_ok(const Bytes& bytes) {
    const FeedDecoded decoded = decode_message(bytes);
    EXPECT_EQ(decoded.status, FeedDecodeStatus::Ok);
    return litbook::testing::unwrap(decoded.message);
}

Bytes encode_bytes(const FeedMessage& m) {
    Bytes out(kMaxMessageLength);
    out.resize(encode(m, out));
    return out;
}

TEST(FeedCodecTest, LengthsMatchTheProtocolTables) {
    EXPECT_EQ(golden_system_event().size(), kSystemEventLength);
    EXPECT_EQ(golden_directory().size(), kSymbolDirectoryLength);
    EXPECT_EQ(golden_level().size(), kLevelUpdateLength);
    EXPECT_EQ(golden_trade().size(), kTradeLength);
    EXPECT_EQ(golden_snapshot_start().size(), kSnapshotStartLength);
    EXPECT_EQ(golden_snapshot_end().size(), kSnapshotEndLength);
    EXPECT_EQ(message_length(std::byte{'S'}), 10U);
    EXPECT_EQ(message_length(std::byte{'R'}), 31U);
    EXPECT_EQ(message_length(std::byte{'L'}), 32U);
    EXPECT_EQ(message_length(std::byte{'P'}), 32U);
    EXPECT_EQ(message_length(std::byte{'Y'}), 25U);
    EXPECT_EQ(message_length(std::byte{'Z'}), 19U);
    EXPECT_EQ(message_length(std::byte{'X'}), 0U);
}

TEST(FeedCodecTest, DecodesEveryGoldenFrame) {
    EXPECT_EQ(
        decode_ok(golden_system_event()),
        FeedMessage(SystemEvent{.timestamp = 1'000, .code = SystemEventCode::StartOfMessages}));
    EXPECT_EQ(decode_ok(golden_directory()), FeedMessage(SymbolDirectory{.timestamp = 2'000,
                                                                         .locate = 3,
                                                                         .symbol = ticker("AAPL"),
                                                                         .tick_size = 100,
                                                                         .lot_size = 1}));
    EXPECT_EQ(decode_ok(golden_level()), FeedMessage(LevelUpdateMessage{.timestamp = 3'000,
                                                                        .locate = 1,
                                                                        .side = Side::Sell,
                                                                        .price = 1'234'500,
                                                                        .total_qty = 700,
                                                                        .order_count = 3}));
    EXPECT_EQ(decode_ok(golden_trade()), FeedMessage(TradeMessage{.timestamp = 4'000,
                                                                  .locate = 2,
                                                                  .aggressor_side = Side::Buy,
                                                                  .price = 500'000,
                                                                  .qty = 250,
                                                                  .match_number = 77}));
    EXPECT_EQ(decode_ok(golden_snapshot_start()), FeedMessage(SnapshotStart{.timestamp = 5'000,
                                                                            .locate = 1,
                                                                            .as_of = 1'000'000,
                                                                            .level_count = 12,
                                                                            .symbol_count = 4}));
    EXPECT_EQ(decode_ok(golden_snapshot_end()),
              FeedMessage(SnapshotEnd{.timestamp = 6'000, .locate = 1, .as_of = 1'000'000}));
}

TEST(FeedCodecTest, EncodingAGoldenMessageGivesTheGoldenBytes) {
    for (const Bytes& golden : {golden_system_event(), golden_directory(), golden_level(),
                                golden_trade(), golden_snapshot_start(), golden_snapshot_end()}) {
        EXPECT_EQ(encode_bytes(decode_ok(golden)), golden);
    }
}

TEST(FeedCodecTest, EndOfMessagesAndAnEmptyLevelDecode) {
    const Bytes end = FrameBuilder().ch('S').u64(9).ch('C').build();
    EXPECT_EQ(decode_ok(end),
              FeedMessage(SystemEvent{.timestamp = 9, .code = SystemEventCode::EndOfMessages}));
    const Bytes removed =
        FrameBuilder().ch('L').u64(1).u16(0).ch('B').u64(100).u64(0).u32(0).build();
    const auto level = std::get<LevelUpdateMessage>(decode_ok(removed));
    EXPECT_EQ(level.total_qty, 0U);
    EXPECT_EQ(level.order_count, 0U);
}

TEST(FeedCodecTest, LargestPriceDecodesAndOneMoreDoesNot) {
    const Bytes largest =
        FrameBuilder().ch('L').u64(1).u16(0).ch('B').u64(kMaxWirePrice).u64(1).u32(1).build();
    EXPECT_EQ(decode_message(largest).status, FeedDecodeStatus::Ok);
    const Bytes above =
        FrameBuilder().ch('L').u64(1).u16(0).ch('B').u64(kMaxWirePrice + 1).u64(1).u32(1).build();
    EXPECT_EQ(decode_message(above).status, FeedDecodeStatus::BadPrice);
}

struct BadFrame {
    const char* name;
    Bytes bytes;
    FeedDecodeStatus status;
};

TEST(FeedCodecTest, MalformedFramesReportWhy) {
    Bytes too_long = golden_trade();
    too_long.push_back(std::byte{0});
    Bytes too_short = golden_level();
    too_short.pop_back();
    const std::vector<BadFrame> cases{
        {"empty", {}, FeedDecodeStatus::Empty},
        {"unknown type", FrameBuilder().ch('Q').u64(0).build(), FeedDecodeStatus::UnknownType},
        {"one byte short", too_short, FeedDecodeStatus::BadLength},
        {"one byte long", too_long, FeedDecodeStatus::BadLength},
        {"event code", FrameBuilder().ch('S').u64(1).ch('X').build(),
         FeedDecodeStatus::BadEventCode},
        {"symbol with inner space",
         FrameBuilder().ch('R').u64(1).u16(0).text("A B     ").u64(1).u32(1).build(),
         FeedDecodeStatus::BadSymbol},
        {"blank symbol",
         FrameBuilder().ch('R').u64(1).u16(0).text("        ").u64(1).u32(1).build(),
         FeedDecodeStatus::BadSymbol},
        {"tick size 0", FrameBuilder().ch('R').u64(1).u16(0).text("AAPL    ").u64(0).u32(1).build(),
         FeedDecodeStatus::BadPrice},
        {"lot size 0", FrameBuilder().ch('R').u64(1).u16(0).text("AAPL    ").u64(1).u32(0).build(),
         FeedDecodeStatus::BadLotSize},
        {"level side", FrameBuilder().ch('L').u64(1).u16(0).ch('X').u64(1).u64(1).u32(1).build(),
         FeedDecodeStatus::BadSide},
        {"level quantity without orders",
         FrameBuilder().ch('L').u64(1).u16(0).ch('B').u64(1).u64(5).u32(0).build(),
         FeedDecodeStatus::BadLevel},
        {"level orders without quantity",
         FrameBuilder().ch('L').u64(1).u16(0).ch('S').u64(1).u64(0).u32(2).build(),
         FeedDecodeStatus::BadLevel},
        {"trade side", FrameBuilder().ch('P').u64(1).u16(0).ch('b').u64(1).u32(1).u64(1).build(),
         FeedDecodeStatus::BadSide},
        {"trade price",
         FrameBuilder().ch('P').u64(1).u16(0).ch('B').u64(kMaxWirePrice + 1).u32(1).u64(1).build(),
         FeedDecodeStatus::BadPrice},
        {"trade of 0", FrameBuilder().ch('P').u64(1).u16(0).ch('B').u64(1).u32(0).u64(1).build(),
         FeedDecodeStatus::BadQuantity},
        {"snapshot locate at the count",
         FrameBuilder().ch('Y').u64(1).u16(4).u64(1).u32(0).u16(4).build(),
         FeedDecodeStatus::BadLocate},
        {"snapshot with no stocks",
         FrameBuilder().ch('Y').u64(1).u16(0).u64(1).u32(0).u16(0).build(),
         FeedDecodeStatus::BadLocate},
    };
    for (const BadFrame& c : cases) {
        const FeedDecoded decoded = decode_message(c.bytes);
        EXPECT_EQ(decoded.status, c.status) << c.name;
        EXPECT_FALSE(decoded.message.has_value()) << c.name;
    }
}

TEST(FeedCodecTest, EncodeRefusesWhatCouldNotBeDecoded) {
    std::array<std::byte, kMaxMessageLength> out{};
    out.fill(std::byte{0xAB});
    const auto untouched = out;

    EXPECT_EQ(encode(LevelUpdateMessage{.price = -1, .total_qty = 1, .order_count = 1}, out), 0U);
    EXPECT_EQ(encode(LevelUpdateMessage{.price = 1, .total_qty = 1, .order_count = 0}, out), 0U);
    EXPECT_EQ(encode(TradeMessage{.price = 1, .qty = 0}, out), 0U);
    EXPECT_EQ(encode(TradeMessage{.price = static_cast<Price>(kMaxWirePrice) + 1, .qty = 1}, out),
              0U);
    EXPECT_EQ(
        encode(
            SymbolDirectory{
                .timestamp = 0, .locate = 0, .symbol = ticker("A"), .tick_size = 0, .lot_size = 1},
            out),
        0U);
    EXPECT_EQ(
        encode(
            SymbolDirectory{
                .timestamp = 0, .locate = 0, .symbol = ticker("A"), .tick_size = 1, .lot_size = 0},
            out),
        0U);
    EXPECT_EQ(encode(SnapshotStart{.locate = 2, .symbol_count = 2}, out), 0U);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): an invalid code on purpose
    EXPECT_EQ(encode(SystemEvent{.code = static_cast<SystemEventCode>('X')}, out), 0U);
    EXPECT_EQ(out, untouched);
}

TEST(FeedCodecTest, EncodeRefusesABufferOneByteTooSmall) {
    for (const Bytes& golden : {golden_system_event(), golden_directory(), golden_level(),
                                golden_trade(), golden_snapshot_start(), golden_snapshot_end()}) {
        const FeedMessage m = decode_ok(golden);
        Bytes out(golden.size() - 1, std::byte{0});
        EXPECT_EQ(encode(m, out), 0U);
        out.push_back(std::byte{0});
        EXPECT_EQ(encode(m, out), golden.size());
        EXPECT_EQ(out, golden);
    }
}

TEST(FeedCodecTest, StatusesPrintTheirNames) {
    std::ostringstream os;
    os << FeedDecodeStatus::BadLevel << ' ' << FeedDecodeStatus::Ok << ' '
       << FeedDecodeStatus::BadLocate;
    EXPECT_EQ(os.str(), "BadLevel Ok BadLocate");
}

}  // namespace
}  // namespace litbook::feed
