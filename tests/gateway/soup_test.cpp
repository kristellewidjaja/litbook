#include "litbook/gateway/soup.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "gateway/soup_test_support.hpp"
#include "support/unwrap.hpp"

namespace litbook::gateway::soup {
namespace {

using litbook::gateway::testing::Bytes;
using litbook::testing::unwrap;

// Turns text into bytes, so golden packets can be written out readably.
Bytes text(std::string_view s) {
    Bytes out;
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

Bytes join(std::initializer_list<Bytes> parts) {
    return litbook::gateway::testing::concat(parts);
}

Bytes length_field(std::uint16_t length) {
    return {static_cast<std::byte>(length >> 8U), static_cast<std::byte>(length & 0xFFU)};
}

// next_packet()

TEST(SoupNextPacket, NeedsBothLengthBytes) {
    EXPECT_EQ(next_packet({}, 100).status, PacketStatus::Incomplete);
    const Bytes one{std::byte{0}};
    EXPECT_EQ(next_packet(one, 100).status, PacketStatus::Incomplete);
}

TEST(SoupNextPacket, ZeroLengthIsBad) {
    EXPECT_EQ(next_packet(length_field(0), 100).status, PacketStatus::BadLength);
}

TEST(SoupNextPacket, TooLongIsBadAsSoonAsTheLengthArrives) {
    EXPECT_EQ(next_packet(length_field(101), 100).status, PacketStatus::BadLength);
    EXPECT_EQ(next_packet(length_field(0xFFFF), 100).status, PacketStatus::BadLength);
    // The limit itself is fine, so this one just needs more bytes.
    EXPECT_EQ(next_packet(length_field(100), 100).status, PacketStatus::Incomplete);
}

TEST(SoupNextPacket, ReadsOneWholePacket) {
    const Bytes bytes = join({length_field(4), text("Uabc")});
    const Packet p = next_packet(bytes, 100);
    ASSERT_EQ(p.status, PacketStatus::Complete);
    EXPECT_EQ(p.type, std::byte{'U'});
    EXPECT_EQ(Bytes(p.payload.begin(), p.payload.end()), text("abc"));
    EXPECT_EQ(p.size, 6U);
    EXPECT_EQ(p.payload.data(), bytes.data() + 3);  // It points into the input, no copy.
}

TEST(SoupNextPacket, EveryShorterPrefixIsIncomplete) {
    const Bytes bytes = join({length_field(4), text("Uabc")});
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        EXPECT_EQ(next_packet(std::span(bytes).first(n), 100).status, PacketStatus::Incomplete)
            << n;
    }
}

TEST(SoupNextPacket, StopsAtTheEndOfTheFirstPacket) {
    const Bytes bytes = join({length_field(1), text("R"), length_field(2), text("+x")});
    const Packet first = next_packet(bytes, 100);
    ASSERT_EQ(first.status, PacketStatus::Complete);
    EXPECT_EQ(first.type, std::byte{'R'});
    EXPECT_TRUE(first.payload.empty());
    EXPECT_EQ(first.size, 3U);
    const Packet second = next_packet(std::span(bytes).subspan(first.size), 100);
    ASSERT_EQ(second.status, PacketStatus::Complete);
    EXPECT_EQ(second.type, std::byte{'+'});
    EXPECT_EQ(second.size, 4U);
}

// encode_packet()

TEST(SoupEncodePacket, WritesLengthTypeAndPayload) {
    std::array<std::byte, 8> out{};
    const Bytes payload = text("hey");
    ASSERT_EQ(encode_packet(PacketType::SequencedData, payload, out), 6U);
    EXPECT_EQ(Bytes(out.begin(), out.begin() + 6), join({length_field(4), text("Shey")}));
}

TEST(SoupEncodePacket, EmptyPayload) {
    std::array<std::byte, 3> out{};
    ASSERT_EQ(encode_packet(PacketType::ServerHeartbeat, {}, out), 3U);
    EXPECT_EQ(Bytes(out.begin(), out.end()), join({length_field(1), text("H")}));
}

TEST(SoupEncodePacket, RefusesASmallBufferAndWritesNothing) {
    std::array<std::byte, 5> out{};
    out.fill(std::byte{0x55});
    EXPECT_EQ(encode_packet(PacketType::SequencedData, text("hey"), out), 0U);
    for (const std::byte b : out) {
        EXPECT_EQ(b, std::byte{0x55});
    }
}

TEST(SoupEncodePacket, RefusesAPayloadTooLongForTheLengthField) {
    const Bytes biggest(kMaxLengthField - 1);
    Bytes out(kHeaderSize + kMaxLengthField);
    EXPECT_EQ(encode_packet(PacketType::Debug, biggest, out), kHeaderSize + biggest.size());
    const Bytes too_big(kMaxLengthField);
    EXPECT_EQ(encode_packet(PacketType::Debug, too_big, out), 0U);
}

// Login Request

TEST(SoupLoginRequest, GoldenBytes) {
    const LoginRequest request = unwrap(make_login_request("alice", "secret", "1", 7));
    std::array<std::byte, kHeaderSize + kLoginRequestPayload> out{};
    ASSERT_EQ(encode_login_request(request, out), out.size());
    const Bytes expected = join({length_field(47), text("L"), text("alice "), text("secret    "),
                                 text("         1"), text("                   7")});
    EXPECT_EQ(Bytes(out.begin(), out.end()), expected);
}

TEST(SoupLoginRequest, RoundTrips) {
    const LoginRequest request = unwrap(make_login_request("BOB", "pw", "", 123456));
    std::array<std::byte, kHeaderSize + kLoginRequestPayload> out{};
    ASSERT_EQ(encode_login_request(request, out), out.size());
    const Packet p = next_packet(out, kMaxLengthField);
    ASSERT_EQ(p.status, PacketStatus::Complete);
    EXPECT_EQ(p.type, std::byte{'L'});
    EXPECT_EQ(decode_login_request(p.payload), request);
}

TEST(SoupLoginRequest, TooLongFieldsCannotBeMade) {
    EXPECT_FALSE(make_login_request("sevench", "pw", "", 0).has_value());
    EXPECT_FALSE(make_login_request("bob", "elevenchars", "", 0).has_value());
    EXPECT_FALSE(make_login_request("bob", "pw", "elevenchars", 0).has_value());
    EXPECT_TRUE(make_login_request("sixchr", "tenchars10", "tenchars10", 0).has_value());
}

TEST(SoupLoginRequest, WrongPayloadLengthDoesNotDecode) {
    const Bytes short_payload(kLoginRequestPayload - 1, std::byte{' '});
    const Bytes long_payload(kLoginRequestPayload + 1, std::byte{' '});
    EXPECT_FALSE(decode_login_request(short_payload).has_value());
    EXPECT_FALSE(decode_login_request(long_payload).has_value());
}

// Builds a Login Request payload with the given 20-character sequence field.
Bytes login_payload_with_sequence(std::string_view sequence) {
    return join({text("alice "), text("secret    "), text("          "), text(sequence)});
}

TEST(SoupLoginRequest, SequenceFieldParsing) {
    struct Case {
        std::string_view field;
        std::optional<std::uint64_t> value;
    };
    const std::vector<Case> cases = {
        {"                    ", 0},  // All spaces counts as 0.
        {"                   0", 0},
        {"                  42", 42},
        {"42                  ", 42},  // Padding on the other side is accepted too.
        {"   42               ", 42},
        {"00000000000000000042", 42},
        {"18446744073709551615", 18446744073709551615ULL},  // The largest 64-bit value.
        {"18446744073709551616", std::nullopt},             // One more does not fit.
        {"99999999999999999999", std::nullopt},
        {"                 4 2", std::nullopt},  // A space inside the number.
        {"                  -1", std::nullopt},
        {"                 0x1", std::nullopt},
    };
    for (const Case& c : cases) {
        const auto decoded = decode_login_request(login_payload_with_sequence(c.field));
        if (c.value) {
            ASSERT_TRUE(decoded.has_value()) << c.field;
            EXPECT_EQ(unwrap(decoded).sequence, unwrap(c.value)) << c.field;
        } else {
            EXPECT_FALSE(decoded.has_value()) << c.field;
        }
    }
}

// Login Accepted and Login Rejected

TEST(SoupLoginAccepted, GoldenBytesAndRoundTrip) {
    const LoginAccepted accepted{.session = unwrap(pad_left<kSessionLength>("1")), .sequence = 1};
    std::array<std::byte, kHeaderSize + kLoginAcceptedPayload> out{};
    ASSERT_EQ(encode_login_accepted(accepted, out), out.size());
    const Bytes expected =
        join({length_field(31), text("A"), text("         1"), text("                   1")});
    EXPECT_EQ(Bytes(out.begin(), out.end()), expected);
    const Packet p = next_packet(out, kMaxLengthField);
    EXPECT_EQ(decode_login_accepted(p.payload), accepted);
    EXPECT_FALSE(decode_login_accepted(std::span(p.payload).first(kLoginAcceptedPayload - 1)));
}

TEST(SoupLoginAccepted, BadSequenceDoesNotDecode) {
    const Bytes payload = join({text("         1"), text("                  1x")});
    EXPECT_FALSE(decode_login_accepted(payload).has_value());
}

TEST(SoupLoginRejected, GoldenBytesAndRoundTrip) {
    for (const LoginRejectReason reason :
         {LoginRejectReason::NotAuthorized, LoginRejectReason::SessionNotAvailable}) {
        std::array<std::byte, kHeaderSize + kLoginRejectedPayload> out{};
        ASSERT_EQ(encode_login_rejected(reason, out), out.size());
        EXPECT_EQ(out[0], std::byte{0});
        EXPECT_EQ(out[1], std::byte{2});
        EXPECT_EQ(out[2], std::byte{'J'});
        EXPECT_EQ(out[3], static_cast<std::byte>(reason));
        EXPECT_EQ(decode_login_rejected(std::span(out).subspan(kHeaderSize)), reason);
    }
}

TEST(SoupLoginRejected, UnknownReasonsPassThroughAndBadLengthsDoNot) {
    const Bytes unknown = text("X");
    EXPECT_EQ(static_cast<char>(unwrap(decode_login_rejected(unknown))), 'X');
    EXPECT_FALSE(decode_login_rejected({}).has_value());
    EXPECT_FALSE(decode_login_rejected(text("AS")).has_value());
}

TEST(SoupEncoders, RefuseSmallBuffers) {
    std::array<std::byte, kHeaderSize> small{};
    EXPECT_EQ(encode_login_request(LoginRequest{}, small), 0U);
    EXPECT_EQ(encode_login_accepted(LoginAccepted{}, small), 0U);
    EXPECT_EQ(encode_login_rejected(LoginRejectReason::NotAuthorized, small), 0U);
}

// Field helpers

TEST(SoupFields, PaddingAndTrimming) {
    const auto right = unwrap(pad_right<6>("ab"));
    EXPECT_EQ(std::string_view(right.data(), right.size()), "ab    ");
    const auto left = unwrap(pad_left<6>("ab"));
    EXPECT_EQ(std::string_view(left.data(), left.size()), "    ab");
    EXPECT_FALSE(pad_right<2>("abc").has_value());
    EXPECT_FALSE(pad_left<2>("abc").has_value());
    EXPECT_EQ(trimmed(right), "ab");
    EXPECT_EQ(trimmed(left), "ab");
    const auto blank = unwrap(pad_left<4>(""));
    EXPECT_EQ(trimmed(blank), "");
}

TEST(SoupFields, FormatNumber) {
    std::array<char, kSequenceLength> field{};
    format_number(0, field);
    EXPECT_EQ(std::string_view(field.data(), field.size()), "                   0");
    format_number(1234, field);
    EXPECT_EQ(std::string_view(field.data(), field.size()), "                1234");
    format_number(18446744073709551615ULL, field);
    EXPECT_EQ(std::string_view(field.data(), field.size()), "18446744073709551615");
    EXPECT_EQ(parse_number(field), 18446744073709551615ULL);
}

}  // namespace
}  // namespace litbook::gateway::soup
