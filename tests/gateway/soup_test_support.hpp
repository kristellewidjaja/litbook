#pragma once

// Helpers that play the client side of SoupBinTCP in tests. They build the packets a
// client sends and split the server's output back into packets.

#include "litbook/gateway/soup.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace litbook::gateway::testing {

using Bytes = std::vector<std::byte>;

inline Bytes packet(soup::PacketType type, std::span<const std::byte> payload = {}) {
    Bytes out(soup::kHeaderSize + payload.size());
    const std::size_t n = soup::encode_packet(type, payload, out);
    EXPECT_EQ(n, out.size());
    return out;
}

inline Bytes login(std::string_view username, std::string_view password,
                   std::string_view session = "", std::uint64_t sequence = 0) {
    const auto request = soup::make_login_request(username, password, session, sequence);
    EXPECT_TRUE(request.has_value());
    Bytes out(soup::kHeaderSize + soup::kLoginRequestPayload);
    if (request) {
        EXPECT_EQ(soup::encode_login_request(*request, out), out.size());
    }
    return out;
}

// An Unsequenced Data packet carrying one OUCH message.
inline Bytes unsequenced(const protocol::InboundMessage& message) {
    std::array<std::byte, protocol::kMaxMessageLength> frame{};
    const std::size_t n = protocol::encode(message, frame);
    EXPECT_GT(n, 0U);
    return packet(soup::PacketType::UnsequencedData, std::span<const std::byte>(frame.data(), n));
}

inline Bytes heartbeat() {
    return packet(soup::PacketType::ClientHeartbeat);
}
inline Bytes logout() {
    return packet(soup::PacketType::LogoutRequest);
}

inline Bytes debug(std::string_view text) {
    Bytes payload;
    for (const char c : text) {
        payload.push_back(static_cast<std::byte>(c));
    }
    return packet(soup::PacketType::Debug, payload);
}

// Joins several byte arrays into one.
inline Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// One packet sent by the server, copied out of its buffer.
struct ServerPacket {
    soup::PacketType type{};
    Bytes payload;

    // The OUCH message inside a Sequenced Data packet.
    [[nodiscard]] std::optional<protocol::OutboundMessage> message() const {
        if (type != soup::PacketType::SequencedData) {
            return std::nullopt;
        }
        return protocol::decode_outbound(payload).message;
    }
};

// Splits `bytes` into whole packets. The test fails if any bytes are left over, because
// the server must only ever queue whole packets.
inline std::vector<ServerPacket> parse_packets(std::span<const std::byte> bytes) {
    std::vector<ServerPacket> packets;
    while (!bytes.empty()) {
        const soup::Packet p = soup::next_packet(bytes, soup::kMaxLengthField);
        EXPECT_EQ(p.status, soup::PacketStatus::Complete) << "a cut off packet in the output";
        if (p.status != soup::PacketStatus::Complete) {
            break;
        }
        packets.push_back(
            ServerPacket{.type = static_cast<soup::PacketType>(static_cast<char>(p.type)),
                         .payload = Bytes(p.payload.begin(), p.payload.end())});
        bytes = bytes.subspan(p.size);
    }
    return packets;
}

}  // namespace litbook::gateway::testing
