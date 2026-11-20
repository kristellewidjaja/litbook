#include "litbook/gateway/soup.hpp"

#include "litbook/protocol/byte_order.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace litbook::gateway::soup {

namespace {

// Field offsets inside the payloads (counted from the byte after the type byte).
namespace login_request {
constexpr std::size_t kUsername = 0;
constexpr std::size_t kPassword = kUsername + kUsernameLength;
constexpr std::size_t kSession = kPassword + kPasswordLength;
constexpr std::size_t kSequence = kSession + kSessionLength;
static_assert(kSequence + kSequenceLength == kLoginRequestPayload);
}  // namespace login_request

namespace login_accepted {
constexpr std::size_t kSession = 0;
constexpr std::size_t kSequence = kSession + kSessionLength;
static_assert(kSequence + kSequenceLength == kLoginAcceptedPayload);
}  // namespace login_accepted

// Reads N characters of a payload as text.
template <std::size_t N>
std::array<char, N> read_text(std::span<const std::byte> payload, std::size_t offset) noexcept {
    std::array<char, N> field{};
    std::memcpy(field.data(), payload.data() + offset, N);
    return field;
}

template <std::size_t N>
void write_text(std::span<std::byte> payload, std::size_t offset,
                const std::array<char, N>& field) noexcept {
    std::memcpy(payload.data() + offset, field.data(), N);
}

std::optional<std::uint64_t> read_number(std::span<const std::byte> payload,
                                         std::size_t offset) noexcept {
    const auto text = read_text<kSequenceLength>(payload, offset);
    return parse_number(text);
}

void write_number(std::span<std::byte> payload, std::size_t offset, std::uint64_t value) noexcept {
    std::array<char, kSequenceLength> text{};
    format_number(value, text);
    write_text(payload, offset, text);
}

// Writes the length field and the type byte of a packet whose payload is `payload_size`
// bytes long, and returns the span where the payload goes. The caller has already checked
// that `out` is big enough.
std::span<std::byte> write_header(PacketType type, std::size_t payload_size,
                                  std::span<std::byte> out) noexcept {
    protocol::store_be(out, 0, static_cast<std::uint16_t>(payload_size + 1));
    out[kLengthFieldSize] = static_cast<std::byte>(type);
    return out.subspan(kHeaderSize, payload_size);
}

// A result with no packet in it, for the Incomplete and BadLength cases.
Packet no_packet(PacketStatus status) noexcept {
    return Packet{.status = status, .type = std::byte{0}, .payload = {}, .size = 0};
}

}  // namespace

Packet next_packet(std::span<const std::byte> bytes, std::size_t max_length) noexcept {
    if (bytes.size() < kLengthFieldSize) {
        return no_packet(PacketStatus::Incomplete);
    }
    const std::size_t length = protocol::load_be<std::uint16_t>(bytes, 0);
    if (length == 0 || length > max_length) {
        return no_packet(PacketStatus::BadLength);
    }
    const std::size_t size = kLengthFieldSize + length;
    if (bytes.size() < size) {
        return no_packet(PacketStatus::Incomplete);
    }
    return Packet{.status = PacketStatus::Complete,
                  .type = bytes[kLengthFieldSize],
                  .payload = bytes.subspan(kHeaderSize, length - 1),
                  .size = size};
}

std::size_t encode_packet(PacketType type, std::span<const std::byte> payload,
                          std::span<std::byte> out) noexcept {
    const std::size_t size = kHeaderSize + payload.size();
    if (payload.size() + 1 > kMaxLengthField || out.size() < size) {
        return 0;
    }
    const std::span<std::byte> body = write_header(type, payload.size(), out);
    // An empty payload may come with a null data pointer, which memcpy must not get.
    if (!payload.empty()) {
        std::memcpy(body.data(), payload.data(), payload.size());
    }
    return size;
}

std::optional<LoginRequest> make_login_request(std::string_view username, std::string_view password,
                                               std::string_view session,
                                               std::uint64_t sequence) noexcept {
    const auto user = pad_right<kUsernameLength>(username);
    const auto pass = pad_right<kPasswordLength>(password);
    const auto name = pad_left<kSessionLength>(session);
    if (!user || !pass || !name) {
        return std::nullopt;
    }
    return LoginRequest{
        .username = *user, .password = *pass, .session = *name, .sequence = sequence};
}

std::optional<LoginRequest> decode_login_request(std::span<const std::byte> payload) noexcept {
    if (payload.size() != kLoginRequestPayload) {
        return std::nullopt;
    }
    const auto sequence = read_number(payload, login_request::kSequence);
    if (!sequence) {
        return std::nullopt;
    }
    return LoginRequest{.username = read_text<kUsernameLength>(payload, login_request::kUsername),
                        .password = read_text<kPasswordLength>(payload, login_request::kPassword),
                        .session = read_text<kSessionLength>(payload, login_request::kSession),
                        .sequence = *sequence};
}

std::optional<LoginAccepted> decode_login_accepted(std::span<const std::byte> payload) noexcept {
    if (payload.size() != kLoginAcceptedPayload) {
        return std::nullopt;
    }
    const auto sequence = read_number(payload, login_accepted::kSequence);
    if (!sequence) {
        return std::nullopt;
    }
    return LoginAccepted{.session = read_text<kSessionLength>(payload, login_accepted::kSession),
                         .sequence = *sequence};
}

std::optional<LoginRejectReason> decode_login_rejected(
    std::span<const std::byte> payload) noexcept {
    if (payload.size() != kLoginRejectedPayload) {
        return std::nullopt;
    }
    // The reason is an open set, like the reason codes in OUCH replies, so a client keeps
    // working if a new code is added.
    return static_cast<LoginRejectReason>(static_cast<char>(payload[0]));
}

std::size_t encode_login_request(const LoginRequest& m, std::span<std::byte> out) noexcept {
    const std::size_t size = kHeaderSize + kLoginRequestPayload;
    if (out.size() < size) {
        return 0;
    }
    const std::span<std::byte> body =
        write_header(PacketType::LoginRequest, kLoginRequestPayload, out);
    write_text(body, login_request::kUsername, m.username);
    write_text(body, login_request::kPassword, m.password);
    write_text(body, login_request::kSession, m.session);
    write_number(body, login_request::kSequence, m.sequence);
    return size;
}

std::size_t encode_login_accepted(const LoginAccepted& m, std::span<std::byte> out) noexcept {
    const std::size_t size = kHeaderSize + kLoginAcceptedPayload;
    if (out.size() < size) {
        return 0;
    }
    const std::span<std::byte> body =
        write_header(PacketType::LoginAccepted, kLoginAcceptedPayload, out);
    write_text(body, login_accepted::kSession, m.session);
    write_number(body, login_accepted::kSequence, m.sequence);
    return size;
}

std::size_t encode_login_rejected(LoginRejectReason reason, std::span<std::byte> out) noexcept {
    const std::size_t size = kHeaderSize + kLoginRejectedPayload;
    if (out.size() < size) {
        return 0;
    }
    const std::span<std::byte> body =
        write_header(PacketType::LoginRejected, kLoginRejectedPayload, out);
    body[0] = static_cast<std::byte>(reason);
    return size;
}

std::string_view trimmed(std::span<const char> field) noexcept {
    std::size_t begin = 0;
    std::size_t end = field.size();
    while (begin < end && field[begin] == ' ') {
        ++begin;
    }
    while (end > begin && field[end - 1] == ' ') {
        --end;
    }
    return {field.data() + begin, end - begin};
}

std::optional<std::uint64_t> parse_number(std::span<const char> field) noexcept {
    const std::string_view digits = trimmed(field);
    std::uint64_t value = 0;
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        // Refuse the digit if value * 10 + digit would go past the largest 64-bit value.
        if (value > (kMax - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

void format_number(std::uint64_t value, std::span<char, kSequenceLength> field) noexcept {
    std::ranges::fill(field, ' ');
    // Write the digits from the right end of the field towards the left. The loop runs at
    // least once, so 0 is written as "0".
    std::size_t i = field.size();
    do {
        --i;
        field[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
}

}  // namespace litbook::gateway::soup
