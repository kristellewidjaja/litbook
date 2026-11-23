#include "litbook/loadgen/client_session.hpp"

#include "litbook/engine/ticker.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "support/unwrap.hpp"

namespace litbook::loadgen {
namespace {

namespace soup = gateway::soup;
using Bytes = std::vector<std::byte>;

Bytes packet(soup::PacketType type, std::span<const std::byte> payload = {}) {
    Bytes out(soup::kHeaderSize + payload.size());
    EXPECT_EQ(soup::encode_packet(type, payload, out), out.size());
    return out;
}

Bytes login_accepted(std::uint64_t sequence) {
    Bytes out(soup::kHeaderSize + soup::kLoginAcceptedPayload);
    const auto session = soup::pad_left<soup::kSessionLength>("1");
    EXPECT_TRUE(session.has_value());
    const soup::LoginAccepted m{.session = session.value_or(soup::SessionName{}),
                                .sequence = sequence};
    EXPECT_EQ(soup::encode_login_accepted(m, out), out.size());
    return out;
}

Bytes login_rejected(soup::LoginRejectReason reason) {
    Bytes out(soup::kHeaderSize + soup::kLoginRejectedPayload);
    EXPECT_EQ(soup::encode_login_rejected(reason, out), out.size());
    return out;
}

Bytes sequenced(const protocol::OutboundMessage& m) {
    std::array<std::byte, protocol::kMaxMessageLength> frame{};
    const std::size_t n = protocol::encode(m, frame);
    return packet(soup::PacketType::SequencedData, std::span<const std::byte>(frame.data(), n));
}

Ticker ticker(std::string_view name) {
    return testing::unwrap(make_ticker(name));
}

protocol::OutboundMessage accepted(protocol::UserRefNum ref) {
    return protocol::AcceptedMessage{.timestamp = 5,
                                     .user_ref = ref,
                                     .side = Side::Buy,
                                     .qty = 10,
                                     .symbol = ticker("AAPL"),
                                     .price = 990'000,
                                     .tif = TimeInForce::GoodTillCancel,
                                     .type = OrderType::Limit,
                                     .post_only = false,
                                     .order_ref = 77};
}

void feed(ClientSession& s, std::span<const std::byte> bytes) {
    ASSERT_TRUE(s.input().make_room(bytes.size()));
    ASSERT_EQ(s.input().append_some(bytes), bytes.size());
}

// Takes everything the session queued and splits it into packets.
std::vector<soup::Packet> drain_output(ClientSession& s, Bytes& storage) {
    const auto readable = s.output().readable();
    storage.assign(readable.begin(), readable.end());
    s.output().consume(readable.size());
    std::vector<soup::Packet> packets;
    std::span<const std::byte> rest(storage);
    while (!rest.empty()) {
        const soup::Packet p = soup::next_packet(rest, soup::kMaxLengthField);
        EXPECT_EQ(p.status, soup::PacketStatus::Complete);
        if (p.status != soup::PacketStatus::Complete) {
            break;
        }
        packets.push_back(p);
        rest = rest.subspan(p.size);
    }
    return packets;
}

soup::PacketType type_of(const soup::Packet& p) {
    return static_cast<soup::PacketType>(static_cast<char>(p.type));
}

ClientSession logged_in(std::uint64_t sequence = 1) {
    ClientSession s("alice", "secret");
    s.begin(0);
    s.output().clear();
    const Bytes a = login_accepted(sequence);
    EXPECT_EQ(s.input().append_some(a), a.size());
    s.process_input([](const protocol::OutboundMessage&) {});
    return s;
}

TEST(ClientSessionTest, LongUsernameOrPasswordIsRefused) {
    EXPECT_THROW(ClientSession("toolongname", "x"), std::invalid_argument);
    EXPECT_THROW(ClientSession("bob", "a password that is too long"), std::invalid_argument);
}

TEST(ClientSessionTest, BeginQueuesTheLoginRequest) {
    ClientSession s("alice", "secret");
    EXPECT_EQ(s.state(), SessionState::LoggingIn);
    s.begin(0);
    Bytes storage;
    const auto packets = drain_output(s, storage);
    ASSERT_EQ(packets.size(), 1U);
    ASSERT_EQ(type_of(packets[0]), soup::PacketType::LoginRequest);
    const soup::LoginRequest login =
        testing::unwrap(soup::decode_login_request(packets[0].payload));
    EXPECT_EQ(soup::trimmed(login.username), "alice");
    EXPECT_EQ(soup::trimmed(login.password), "secret");
    EXPECT_EQ(soup::trimmed(login.session), "");
    EXPECT_EQ(login.sequence, 0U);
}

TEST(ClientSessionTest, LoginAcceptedMakesItActive) {
    const ClientSession s = logged_in(42);
    EXPECT_TRUE(s.active());
    EXPECT_EQ(s.first_sequence(), 42U);
    EXPECT_EQ(s.failure(), SessionFailure::None);
}

TEST(ClientSessionTest, LoginRejectedSaysWhy) {
    for (const auto& [reason, failure] :
         {std::pair{soup::LoginRejectReason::NotAuthorized, SessionFailure::NotAuthorized},
          std::pair{soup::LoginRejectReason::SessionNotAvailable,
                    SessionFailure::SessionNotAvailable}}) {
        ClientSession s("alice", "secret");
        s.begin(0);
        feed(s, login_rejected(reason));
        s.process_input([](const protocol::OutboundMessage&) {});
        EXPECT_EQ(s.state(), SessionState::Failed);
        EXPECT_EQ(s.failure(), failure);
        EXPECT_TRUE(s.finished());
    }
}

TEST(ClientSessionTest, SendWaitsForTheLogin) {
    ClientSession s("alice", "secret");
    s.begin(0);
    EXPECT_FALSE(s.can_send());
    EXPECT_FALSE(s.send(protocol::CancelOrder{.user_ref = 1, .qty = 0}, 0));
}

TEST(ClientSessionTest, SendQueuesUnsequencedData) {
    ClientSession s = logged_in();
    const protocol::InboundMessage cancel = protocol::CancelOrder{.user_ref = 9, .qty = 0};
    ASSERT_TRUE(s.can_send());
    ASSERT_TRUE(s.send(cancel, 10));
    Bytes storage;
    const auto packets = drain_output(s, storage);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(type_of(packets[0]), soup::PacketType::UnsequencedData);
    const auto decoded = protocol::decode_inbound(packets[0].payload);
    ASSERT_TRUE(decoded.message.has_value());
    EXPECT_EQ(decoded.message, cancel);
}

TEST(ClientSessionTest, RepliesArriveOneByteAtATime) {
    ClientSession s = logged_in();
    Bytes bytes = sequenced(accepted(3));
    const Bytes heartbeat = packet(soup::PacketType::ServerHeartbeat);
    bytes.insert(bytes.begin(), heartbeat.begin(), heartbeat.end());
    const Bytes second = sequenced(accepted(4));
    bytes.insert(bytes.end(), second.begin(), second.end());
    std::vector<protocol::OutboundMessage> seen;
    for (const std::byte b : bytes) {
        feed(s, std::span<const std::byte>(&b, 1));
        s.process_input([&](const protocol::OutboundMessage& m) { seen.push_back(m); });
    }
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[0], accepted(3));
    EXPECT_EQ(seen[1], accepted(4));
    EXPECT_EQ(s.replies(), 2U);
    EXPECT_TRUE(s.active());
    EXPECT_EQ(s.input().size(), 0U);
}

TEST(ClientSessionTest, ManyRepliesInOneRead) {
    ClientSession s = logged_in();
    Bytes bytes;
    for (protocol::UserRefNum ref = 1; ref <= 20; ++ref) {
        const Bytes p = sequenced(accepted(ref));
        bytes.insert(bytes.end(), p.begin(), p.end());
    }
    feed(s, bytes);
    protocol::UserRefNum expected = 1;
    s.process_input([&](const protocol::OutboundMessage& m) {
        EXPECT_EQ(m, accepted(expected));
        ++expected;
    });
    EXPECT_EQ(expected, 21U);
    EXPECT_EQ(s.replies(), 20U);
}

TEST(ClientSessionTest, EndOfSessionEndsIt) {
    ClientSession s = logged_in();
    feed(s, packet(soup::PacketType::EndOfSession));
    s.process_input([](const protocol::OutboundMessage&) {});
    EXPECT_EQ(s.state(), SessionState::Ended);
    EXPECT_EQ(s.failure(), SessionFailure::None);
    // Nothing is taken after the end.
    feed(s, sequenced(accepted(1)));
    int calls = 0;
    s.process_input([&](const protocol::OutboundMessage&) { ++calls; });
    EXPECT_EQ(calls, 0);
    EXPECT_FALSE(s.send(protocol::CancelOrder{.user_ref = 1, .qty = 0}, 0));
}

TEST(ClientSessionTest, SequencedDataBeforeTheLoginFails) {
    ClientSession s("alice", "secret");
    s.begin(0);
    feed(s, sequenced(accepted(1)));
    s.process_input([](const protocol::OutboundMessage&) { FAIL(); });
    EXPECT_EQ(s.failure(), SessionFailure::UnexpectedPacket);
}

TEST(ClientSessionTest, ClientPacketTypesFromTheServerFail) {
    ClientSession s = logged_in();
    feed(s, packet(soup::PacketType::ClientHeartbeat));
    s.process_input([](const protocol::OutboundMessage&) {});
    EXPECT_EQ(s.failure(), SessionFailure::UnexpectedPacket);
}

TEST(ClientSessionTest, SecondLoginAcceptedFails) {
    ClientSession s = logged_in();
    feed(s, login_accepted(1));
    s.process_input([](const protocol::OutboundMessage&) {});
    EXPECT_EQ(s.failure(), SessionFailure::UnexpectedPacket);
}

TEST(ClientSessionTest, ZeroLengthFieldIsABadPacket) {
    ClientSession s = logged_in();
    const std::array<std::byte, 2> zero{};
    feed(s, zero);
    s.process_input([](const protocol::OutboundMessage&) {});
    EXPECT_EQ(s.failure(), SessionFailure::BadPacket);
}

TEST(ClientSessionTest, LoginAcceptedWithAShortPayloadIsABadPacket) {
    ClientSession s("alice", "secret");
    s.begin(0);
    const std::array<std::byte, 3> payload{};
    feed(s, packet(soup::PacketType::LoginAccepted, payload));
    s.process_input([](const protocol::OutboundMessage&) {});
    EXPECT_EQ(s.failure(), SessionFailure::BadPacket);
}

TEST(ClientSessionTest, MessageThatDoesNotDecodeIsABadMessage) {
    ClientSession s = logged_in();
    const std::array<std::byte, 2> nonsense{std::byte{'Q'}, std::byte{0}};
    feed(s, packet(soup::PacketType::SequencedData, nonsense));
    s.process_input([](const protocol::OutboundMessage&) { FAIL(); });
    EXPECT_EQ(s.failure(), SessionFailure::BadMessage);
}

TEST(ClientSessionTest, PacketBiggerThanTheInputBufferFails) {
    ClientSession s("alice", "secret",
                    ClientSessionConfig{
                        .input_bytes = 16, .output_bytes = 1024, .heartbeat_interval_ns = 1'000});
    s.begin(0);
    const Bytes a = login_accepted(1);
    feed(s, std::span<const std::byte>(a).first(16));  // Only part of a 33-byte packet fits.
    s.process_input([](const protocol::OutboundMessage&) {});
    EXPECT_EQ(s.failure(), SessionFailure::InputFull);
}

TEST(ClientSessionTest, HeartbeatOnlyAfterAQuietInterval) {
    ClientSession s = logged_in();
    ASSERT_TRUE(s.send(protocol::CancelOrder{.user_ref = 1, .qty = 0}, 1'000));
    s.output().clear();
    s.tick(1'000 + 999'999'999);
    EXPECT_EQ(s.output().size(), 0U);
    s.tick(1'000 + 1'000'000'000);
    Bytes storage;
    const auto packets = drain_output(s, storage);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(type_of(packets[0]), soup::PacketType::ClientHeartbeat);
    EXPECT_EQ(s.heartbeats_sent(), 1U);
    s.tick(1'000 + 1'500'000'000);  // The heartbeat itself counts as sending something.
    EXPECT_EQ(s.output().size(), 0U);
}

TEST(ClientSessionTest, LogoutQueuesTheRequestAndEnds) {
    ClientSession s = logged_in();
    s.logout(0);
    EXPECT_EQ(s.state(), SessionState::Ended);
    Bytes storage;
    const auto packets = drain_output(s, storage);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(type_of(packets[0]), soup::PacketType::LogoutRequest);
}

TEST(ClientSessionTest, FailIsFinal) {
    ClientSession s = logged_in();
    s.fail(SessionFailure::Closed);
    s.fail(SessionFailure::BadPacket);
    s.logout(0);
    EXPECT_EQ(s.state(), SessionState::Failed);
    EXPECT_EQ(s.failure(), SessionFailure::Closed);
    EXPECT_STREQ(to_string(SessionFailure::Closed), "connection closed");
}

TEST(ClientSessionTest, FullOutputRefusesToSend) {
    ClientSession small(
        "alice", "secret",
        ClientSessionConfig{
            .input_bytes = 1024, .output_bytes = 60, .heartbeat_interval_ns = 1'000'000'000});
    small.begin(0);
    small.output().clear();
    feed(small, login_accepted(1));
    small.process_input([](const protocol::OutboundMessage&) {});
    ASSERT_TRUE(small.active());
    const protocol::InboundMessage cancel = protocol::CancelOrder{.user_ref = 1, .qty = 0};
    ASSERT_TRUE(small.send(cancel, 0));  // 12 bytes, so 48 are left, below 3 + 47.
    EXPECT_FALSE(small.can_send());
}

}  // namespace
}  // namespace litbook::loadgen
