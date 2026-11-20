#include "litbook/gateway/session.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gateway/soup_test_support.hpp"

namespace litbook::gateway {
namespace {

using testing::Bytes;
using testing::parse_packets;

TEST(Session, StartsUnused) {
    const Session s(64, 64);
    EXPECT_FALSE(s.in_use());
    EXPECT_EQ(s.owner(), kNoOwner);
    EXPECT_EQ(s.close_reason(), CloseReason::None);
}

TEST(Session, OpenStartsAFreshConnection) {
    Session s(64, 64);
    s.open(100);
    EXPECT_TRUE(s.in_use());
    EXPECT_EQ(s.state(), SessionState::AwaitingLogin);
    EXPECT_EQ(s.opened_at(), 100U);
    EXPECT_EQ(s.last_received(), 100U);
    EXPECT_EQ(s.last_sent(), 100U);
    s.note_received(150);
    EXPECT_EQ(s.last_received(), 150U);
}

TEST(Session, LogInSetsTheOwner) {
    Session s(64, 64);
    s.open(0);
    s.log_in(3);
    EXPECT_EQ(s.state(), SessionState::LoggedIn);
    EXPECT_EQ(s.owner(), 3U);
}

TEST(Session, SendPacketQueuesAWholePacket) {
    Session s(64, 64);
    s.open(0);
    const Bytes payload{std::byte{'h'}, std::byte{'i'}};
    ASSERT_TRUE(s.send_packet(soup::PacketType::SequencedData, payload, 42));
    EXPECT_EQ(s.last_sent(), 42U);
    const auto packets = parse_packets(s.output().readable());
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].type, soup::PacketType::SequencedData);
    EXPECT_EQ(packets[0].payload, payload);
}

TEST(Session, SendBytesQueuesThemAsGiven) {
    Session s(64, 64);
    s.open(0);
    const Bytes hb = testing::packet(soup::PacketType::ServerHeartbeat);
    ASSERT_TRUE(s.send_bytes(hb, 7));
    EXPECT_EQ(Bytes(s.output().readable().begin(), s.output().readable().end()), hb);
    EXPECT_EQ(s.last_sent(), 7U);
}

TEST(Session, CloseKeepsTheFirstReason) {
    Session s(64, 64);
    s.open(0);
    s.close(CloseReason::Logout);
    s.close(CloseReason::PeerClosed);
    EXPECT_TRUE(s.closing());
    EXPECT_EQ(s.close_reason(), CloseReason::Logout);
}

TEST(Session, NothingIsQueuedWhileClosing) {
    Session s(64, 64);
    s.open(0);
    s.close(CloseReason::Logout);
    EXPECT_FALSE(s.send_packet(soup::PacketType::ServerHeartbeat, {}, 1));
    EXPECT_FALSE(s.send_bytes(testing::packet(soup::PacketType::ServerHeartbeat), 1));
    EXPECT_TRUE(s.output().empty());
    EXPECT_EQ(s.last_sent(), 0U);
}

TEST(Session, FullOutputClosesTheSessionAndKeepsOnlyWholePackets) {
    Session s(64, 10);  // Room for three heartbeats (3 bytes each) and no more.
    s.open(0);
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(s.send_packet(soup::PacketType::ServerHeartbeat, {}, 1));
    }
    EXPECT_FALSE(s.send_packet(soup::PacketType::ServerHeartbeat, {}, 2));
    EXPECT_EQ(s.close_reason(), CloseReason::OutputFull);
    EXPECT_EQ(parse_packets(s.output().readable()).size(), 3U);
    EXPECT_EQ(s.output().size(), 9U);
}

TEST(Session, FullOutputAlsoClosesForSendBytes) {
    Session s(64, 4);
    s.open(0);
    const Bytes big(5, std::byte{0});
    EXPECT_FALSE(s.send_bytes(big, 1));
    EXPECT_EQ(s.close_reason(), CloseReason::OutputFull);
    EXPECT_TRUE(s.output().empty());
}

TEST(Session, ReleaseAndReopenStartsClean) {
    Session s(64, 64);
    s.open(0);
    s.log_in(2);
    ASSERT_TRUE(s.input().append(Bytes(5, std::byte{1})));
    ASSERT_TRUE(s.send_packet(soup::PacketType::ServerHeartbeat, {}, 1));
    s.close(CloseReason::Logout);
    s.release();
    EXPECT_FALSE(s.in_use());
    s.open(10);
    EXPECT_EQ(s.state(), SessionState::AwaitingLogin);
    EXPECT_EQ(s.owner(), kNoOwner);
    EXPECT_EQ(s.close_reason(), CloseReason::None);
    EXPECT_TRUE(s.input().empty());
    EXPECT_TRUE(s.output().empty());
}

TEST(CloseReason, EveryValueHasItsOwnName) {
    std::set<std::string> names;
    for (std::size_t i = 0; i < kCloseReasonCount; ++i) {
        std::ostringstream os;
        os << static_cast<CloseReason>(i);
        EXPECT_EQ(os.str().find("CloseReason("), std::string::npos) << i;
        names.insert(os.str());
    }
    EXPECT_EQ(names.size(), kCloseReasonCount);
    std::ostringstream os;
    os << static_cast<CloseReason>(kCloseReasonCount);
    EXPECT_EQ(os.str(), "CloseReason(14)");
}

}  // namespace
}  // namespace litbook::gateway
