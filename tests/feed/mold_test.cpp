#include "litbook/feed/mold.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "feed/feed_test_support.hpp"
#include "protocol/frame_builder.hpp"
#include "support/unwrap.hpp"

namespace litbook::feed {
namespace {

using protocol::testing::FrameBuilder;
using testing::Bytes;
using testing::decode_packet;

SessionName session(std::string_view name) {
    return litbook::testing::unwrap(make_session_name(name));
}

LevelUpdateMessage level(Price price) {
    return LevelUpdateMessage{.timestamp = 7,
                              .locate = 0,
                              .side = Side::Buy,
                              .price = price,
                              .total_qty = 10,
                              .order_count = 1};
}

Bytes to_bytes(std::span<const std::byte> s) {
    return {s.begin(), s.end()};
}

TEST(MoldTest, SessionNamesArePaddedOnTheLeft) {
    const SessionName s = session("1");
    EXPECT_EQ(std::string(s.begin(), s.end()), "         1");
    const SessionName full = session("ABCDE12345");
    EXPECT_EQ(std::string(full.begin(), full.end()), "ABCDE12345");
    EXPECT_FALSE(make_session_name("").has_value());
    EXPECT_FALSE(make_session_name("ABCDE123456").has_value());
    EXPECT_FALSE(make_session_name("A B").has_value());
    EXPECT_FALSE(make_session_name("A-1").has_value());
}

TEST(MoldTest, APacketHasTheMoldUdp64HeaderAndBlocks) {
    PacketBuilder builder(session("7"), kDefaultPacketLength);
    builder.start(41);
    ASSERT_TRUE(
        builder.append(SystemEvent{.timestamp = 5, .code = SystemEventCode::StartOfMessages}));
    ASSERT_TRUE(builder.append(level(100)));
    EXPECT_EQ(builder.count(), 2U);
    const Bytes packet = to_bytes(builder.finish());

    // Header, then a block for each message: a 2-byte length and the message bytes.
    Bytes expected = FrameBuilder().text("         7").u64(41).u16(2).u16(10).build();
    const Bytes event = FrameBuilder().ch('S').u64(5).ch('O').build();
    expected.insert(expected.end(), event.begin(), event.end());
    const Bytes length = FrameBuilder().u16(32).build();
    expected.insert(expected.end(), length.begin(), length.end());
    const Bytes update =
        FrameBuilder().ch('L').u64(7).u16(0).ch('B').u64(100).u64(10).u32(1).build();
    expected.insert(expected.end(), update.begin(), update.end());
    EXPECT_EQ(packet, expected);

    const testing::DecodedPacket decoded = decode_packet(packet);
    EXPECT_EQ(decoded.header.sequence, 41U);
    EXPECT_EQ(decoded.header.count, 2U);
    ASSERT_EQ(decoded.messages.size(), 2U);
    EXPECT_EQ(decoded.messages[1], FeedMessage(level(100)));
}

TEST(MoldTest, HeartbeatAndEndOfSessionAreBareHeaders) {
    PacketBuilder builder(session("1"), kDefaultPacketLength);
    ASSERT_TRUE(builder.append(level(1)));
    const Bytes heartbeat = to_bytes(builder.heartbeat(99));
    EXPECT_EQ(heartbeat, FrameBuilder().text("         1").u64(99).u16(0).build());
    EXPECT_TRUE(builder.empty());
    const Bytes end = to_bytes(builder.end_of_session(99));
    EXPECT_EQ(end, FrameBuilder().text("         1").u64(99).u16(0xFFFF).build());

    const CheckedPacket hb = check_packet(heartbeat);
    EXPECT_EQ(hb.status, PacketStatus::Ok);
    EXPECT_TRUE(hb.header.heartbeat());
    EXPECT_EQ(hb.header.messages(), 0U);
    const CheckedPacket eos = check_packet(end);
    EXPECT_EQ(eos.status, PacketStatus::Ok);
    EXPECT_TRUE(eos.header.end_of_session());
    EXPECT_EQ(eos.header.messages(), 0U);
}

TEST(MoldTest, APacketNeverGrowsPastItsLimit) {
    PacketBuilder builder(session("1"), kMinPacketLength + 30);
    builder.start(1);
    // 20 header bytes + 34 for the first level leaves 30, too little for a second level
    // (34) but enough for a System Event (12).
    ASSERT_TRUE(builder.append(level(1)));
    EXPECT_FALSE(builder.append(level(2)));
    EXPECT_EQ(builder.count(), 1U);
    EXPECT_TRUE(builder.append(SystemEvent{}));
    EXPECT_EQ(builder.size(), 20U + 34U + 12U);
    EXPECT_LE(builder.size(), builder.limit());

    PacketBuilder full(session("1"), kDefaultPacketLength);
    full.start(1);
    int added = 0;
    while (full.append(level(added + 1))) {
        ++added;
    }
    EXPECT_EQ(added, static_cast<int>((kDefaultPacketLength - kHeaderLength) / 34));
    EXPECT_LE(full.size(), kDefaultPacketLength);
    EXPECT_EQ(check_packet(full.finish()).status, PacketStatus::Ok);
}

TEST(MoldTest, AppendRefusesAMessageThatCannotBeEncoded) {
    PacketBuilder builder(session("1"), kDefaultPacketLength);
    builder.start(1);
    EXPECT_FALSE(builder.append(level(-5)));
    EXPECT_TRUE(builder.empty());
    EXPECT_EQ(builder.size(), kHeaderLength);
}

TEST(MoldTest, PacketLimitsOutsideTheRangeThrow) {
    EXPECT_THROW(PacketBuilder(session("1"), kMinPacketLength - 1), std::invalid_argument);
    EXPECT_THROW(PacketBuilder(session("1"), kMaxPacketLength + 1), std::invalid_argument);
    EXPECT_NO_THROW(PacketBuilder(session("1"), kMinPacketLength));
    EXPECT_NO_THROW(PacketBuilder(session("1"), kMaxPacketLength));
}

TEST(MoldTest, CheckPacketFindsEveryKindOfDamage) {
    PacketBuilder builder(session("1"), kDefaultPacketLength);
    builder.start(5);
    ASSERT_TRUE(builder.append(level(1)));
    ASSERT_TRUE(builder.append(level(2)));
    const Bytes good = to_bytes(builder.finish());
    ASSERT_EQ(check_packet(good).status, PacketStatus::Ok);

    EXPECT_EQ(check_packet(Bytes(good.begin(), good.begin() + 19)).status, PacketStatus::TooShort);
    EXPECT_EQ(check_packet(Bytes(good.begin(), good.end() - 1)).status, PacketStatus::Truncated);
    EXPECT_EQ(check_packet(Bytes(good.begin(), good.begin() + 21)).status, PacketStatus::Truncated);

    Bytes extra = good;
    extra.push_back(std::byte{0});
    EXPECT_EQ(check_packet(extra).status, PacketStatus::TrailingBytes);

    Bytes count_too_high = good;
    count_too_high[19] = std::byte{3};
    EXPECT_EQ(check_packet(count_too_high).status, PacketStatus::Truncated);

    Bytes count_too_low = good;
    count_too_low[19] = std::byte{1};
    EXPECT_EQ(check_packet(count_too_low).status, PacketStatus::TrailingBytes);

    Bytes heartbeat_with_data = good;
    heartbeat_with_data[19] = std::byte{0};
    EXPECT_EQ(check_packet(heartbeat_with_data).status, PacketStatus::TrailingBytes);

    Bytes bad_side = good;
    bad_side[20 + 2 + 11] = std::byte{'X'};
    const CheckedPacket bad = check_packet(bad_side);
    EXPECT_EQ(bad.status, PacketStatus::BadMessage);
    EXPECT_EQ(bad.message_status, FeedDecodeStatus::BadSide);

    Bytes long_block = good;
    long_block[21] = std::byte{33};  // The first block claims 33 bytes, but a level has 32.
    EXPECT_NE(check_packet(long_block).status, PacketStatus::Ok);
}

TEST(MoldTest, BlockReaderWalksEveryMessageOnce) {
    PacketBuilder builder(session("1"), kDefaultPacketLength);
    builder.start(1);
    ASSERT_TRUE(builder.append(SystemEvent{}));
    ASSERT_TRUE(builder.append(level(3)));
    ASSERT_TRUE(builder.append(SnapshotEnd{.timestamp = 1, .locate = 2, .as_of = 3}));
    const Bytes packet = to_bytes(builder.finish());
    BlockReader reader(packet);
    std::span<const std::byte> m;
    std::vector<std::size_t> lengths;
    while (reader.next(m)) {
        lengths.push_back(m.size());
    }
    EXPECT_EQ(lengths, (std::vector<std::size_t>{10, 32, 19}));
    EXPECT_FALSE(reader.next(m));

    BlockReader heartbeat(to_bytes(builder.heartbeat(4)));
    EXPECT_FALSE(heartbeat.next(m));
}

TEST(MoldTest, StatusesPrintTheirNames) {
    std::ostringstream os;
    os << PacketStatus::Truncated << ' ' << PacketStatus::BadMessage;
    EXPECT_EQ(os.str(), "Truncated BadMessage");
}

}  // namespace
}  // namespace litbook::feed
