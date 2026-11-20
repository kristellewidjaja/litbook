#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// SoupBinTCP 3.0, the session protocol that carries OUCH over TCP at Nasdaq.
//
// Every SoupBinTCP packet starts with a 2-byte big-endian length that counts the bytes
// after it, which are a 1-byte packet type and then the payload. TCP may split one packet
// across several reads or put several packets in one read, so the receiver collects bytes
// and cuts them into packets with next_packet(). docs/PROTOCOL.md describes every packet
// and the rules litbook adds on top of the specification.
//
// Nothing in this file allocates or throws.
namespace litbook::gateway::soup {

// The packet type byte. Debug packets may go either way, and every other type is sent by
// only one side.
enum class PacketType : char {
    Debug = '+',            // Free text that the receiver ignores.
    LoginAccepted = 'A',    // The server says the login worked.
    LoginRejected = 'J',    // The server says the login failed, then closes the socket.
    SequencedData = 'S',    // The server sends one OUCH message from the exchange.
    ServerHeartbeat = 'H',  // The server had nothing else to send for 1 second.
    EndOfSession = 'Z',     // The server ends the session, and the socket will close.
    LoginRequest = 'L',     // The client logs in. This must be its first packet.
    UnsequencedData = 'U',  // The client sends one OUCH message to the exchange.
    ClientHeartbeat = 'R',  // The client had nothing else to send for 1 second.
    LogoutRequest = 'O',    // The client asks the server to close the connection.
};

// Why a login was refused. The letters are the specification's.
enum class LoginRejectReason : char {
    NotAuthorized = 'A',        // The username and password do not match.
    SessionNotAvailable = 'S',  // The requested session or sequence number is not available.
};

inline constexpr std::size_t kLengthFieldSize = 2;
inline constexpr std::size_t kHeaderSize = kLengthFieldSize + 1;  // The length and the type.

// The largest value the length field can hold. The specification itself sets no limit
// below that, so each side picks its own.
inline constexpr std::size_t kMaxLengthField = 0xFFFF;

// Field widths of the login packets.
inline constexpr std::size_t kUsernameLength = 6;
inline constexpr std::size_t kPasswordLength = 10;
inline constexpr std::size_t kSessionLength = 10;
inline constexpr std::size_t kSequenceLength = 20;

// Payload lengths (the bytes after the type byte) of the fixed-size packets.
inline constexpr std::size_t kLoginRequestPayload =
    kUsernameLength + kPasswordLength + kSessionLength + kSequenceLength;
inline constexpr std::size_t kLoginAcceptedPayload = kSessionLength + kSequenceLength;
inline constexpr std::size_t kLoginRejectedPayload = 1;

using Username = std::array<char, kUsernameLength>;
using Password = std::array<char, kPasswordLength>;
using SessionName = std::array<char, kSessionLength>;

// What next_packet() found at the front of the buffered bytes.
enum class PacketStatus : std::uint8_t {
    Complete,    // A whole packet is there.
    Incomplete,  // More bytes are needed before the packet can be read.
    BadLength,   // The length field is 0 (no room for a type byte) or above the limit.
};

// One packet at the front of a byte buffer. `type` and `payload` are only meaningful when
// status is Complete. `payload` points into the buffer that was passed in, so it is valid
// only as long as those bytes are. `size` is the number of bytes the whole packet takes,
// including the length field.
struct Packet {
    PacketStatus status = PacketStatus::Incomplete;
    std::byte type{};
    std::span<const std::byte> payload;
    std::size_t size = 0;
};

// Looks at the front of `bytes` and reports the first packet. `max_length` is the largest
// length field the caller accepts. The check happens as soon as the two length bytes have
// arrived, so a peer cannot make the caller wait for (or buffer) a packet that is too big.
[[nodiscard]] Packet next_packet(std::span<const std::byte> bytes, std::size_t max_length) noexcept;

// Writes one packet (length field, type, payload) to the front of `out` and returns the
// number of bytes written. Returns 0 and writes nothing if `out` is too small or the
// payload is too long for the length field.
[[nodiscard]] std::size_t encode_packet(PacketType type, std::span<const std::byte> payload,
                                        std::span<std::byte> out) noexcept;

// The body of a Login Request. The text fields keep their wire form, so a Username is 6
// characters padded on the right with spaces.
struct LoginRequest {
    Username username{};
    Password password{};
    SessionName session{};       // All spaces asks for the session that is running now.
    std::uint64_t sequence = 0;  // The next sequence number wanted, or 0 for "from now on".
    bool operator==(const LoginRequest&) const = default;
};

// The body of a Login Accepted.
struct LoginAccepted {
    SessionName session{};       // Padded on the left with spaces.
    std::uint64_t sequence = 0;  // The sequence number of the next Sequenced Data packet.
    bool operator==(const LoginAccepted&) const = default;
};

// Builds a Login Request from plain strings. Returns nullopt if a string is too long for
// its field. Username and password are padded on the right and the session on the left.
[[nodiscard]] std::optional<LoginRequest> make_login_request(std::string_view username,
                                                             std::string_view password,
                                                             std::string_view session,
                                                             std::uint64_t sequence) noexcept;

// Each decoder takes the payload of one packet (the bytes after the type byte). It returns
// nullopt if the payload has the wrong length or a numeric field is not a number.
[[nodiscard]] std::optional<LoginRequest> decode_login_request(
    std::span<const std::byte> payload) noexcept;
[[nodiscard]] std::optional<LoginAccepted> decode_login_accepted(
    std::span<const std::byte> payload) noexcept;
[[nodiscard]] std::optional<LoginRejectReason> decode_login_rejected(
    std::span<const std::byte> payload) noexcept;

// Each encoder writes a whole packet, length field included, and returns its size, or 0 if
// `out` is too small.
[[nodiscard]] std::size_t encode_login_request(const LoginRequest& m,
                                               std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode_login_accepted(const LoginAccepted& m,
                                                std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode_login_rejected(LoginRejectReason reason,
                                                std::span<std::byte> out) noexcept;

// Helpers for the padded text and number fields.

// Copies `text` into a field of N characters with spaces after it. Returns nullopt if the
// text is longer than N.
template <std::size_t N>
[[nodiscard]] constexpr std::optional<std::array<char, N>> pad_right(
    std::string_view text) noexcept {
    if (text.size() > N) {
        return std::nullopt;
    }
    std::array<char, N> field{};
    field.fill(' ');
    for (std::size_t i = 0; i < text.size(); ++i) {
        field[i] = text[i];
    }
    return field;
}

// Copies `text` into a field of N characters with spaces before it.
template <std::size_t N>
[[nodiscard]] constexpr std::optional<std::array<char, N>> pad_left(
    std::string_view text) noexcept {
    if (text.size() > N) {
        return std::nullopt;
    }
    std::array<char, N> field{};
    field.fill(' ');
    const std::size_t start = N - text.size();
    for (std::size_t i = 0; i < text.size(); ++i) {
        field[start + i] = text[i];
    }
    return field;
}

// The text of a field with the spaces on both ends removed.
[[nodiscard]] std::string_view trimmed(std::span<const char> field) noexcept;

// Reads an ASCII number with optional spaces before and after it. A field of only spaces
// reads as 0. Returns nullopt for any other character, spaces inside the number, or a
// value that does not fit in 64 bits.
[[nodiscard]] std::optional<std::uint64_t> parse_number(std::span<const char> field) noexcept;

// Writes `value` as ASCII digits, padded on the left with spaces. A 20-character field
// always fits, because the largest 64-bit value has 20 digits.
void format_number(std::uint64_t value, std::span<char, kSequenceLength> field) noexcept;

}  // namespace litbook::gateway::soup
