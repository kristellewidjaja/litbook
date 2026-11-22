#pragma once

#include "litbook/feed/feed_codec.hpp"
#include "litbook/feed/feed_messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string_view>

namespace litbook::feed {

// The MoldUDP64 packet layer of the feed (Nasdaq MoldUDP64 1.00, see docs/FEED.md). A
// packet is a 20-byte header (session, the first message's sequence number, a message
// count) followed by message blocks, each a 2-byte big-endian length and the message.

inline constexpr std::size_t kSessionLength = 10;
inline constexpr std::size_t kHeaderLength = 20;
inline constexpr std::size_t kBlockLengthSize = 2;

// A Message Count of 0 is a heartbeat and 0xFFFF is end of session.
inline constexpr std::uint16_t kHeartbeatCount = 0;
inline constexpr std::uint16_t kEndOfSessionCount = 0xFFFF;

// The most UDP payload that fits in one Ethernet frame with an MTU of 1,500 bytes
// (1,500 minus 20 for the IP header and 8 for the UDP header).
inline constexpr std::size_t kMaxPacketLength = 1472;

// litbook sends at most this much, which leaves room for tunnels and virtual networks
// with a smaller MTU, so no packet is ever split into IP fragments.
inline constexpr std::size_t kDefaultPacketLength = 1200;

// The smallest packet limit that still holds one message of every type.
inline constexpr std::size_t kMinPacketLength =
    kHeaderLength + kBlockLengthSize + kMaxMessageLength;

// A session name as it goes on the wire, padded on the left with spaces.
using SessionName = std::array<char, kSessionLength>;

// Returns the wire form of `name`, or nothing unless it is 1 to 10 letters and digits.
[[nodiscard]] std::optional<SessionName> make_session_name(std::string_view name) noexcept;

struct PacketHeader {
    SessionName session{};
    Sequence sequence = 0;
    std::uint16_t count = 0;

    [[nodiscard]] bool heartbeat() const noexcept { return count == kHeartbeatCount; }
    [[nodiscard]] bool end_of_session() const noexcept { return count == kEndOfSessionCount; }
    // How many messages the packet carries. Heartbeats and end of session carry none.
    [[nodiscard]] std::uint16_t messages() const noexcept {
        return (heartbeat() || end_of_session()) ? std::uint16_t{0} : count;
    }
    bool operator==(const PacketHeader&) const = default;
};

enum class PacketStatus : std::uint8_t {
    Ok,
    TooShort,       // Fewer bytes than a header.
    Truncated,      // A message block runs past the end, or there are fewer blocks than count.
    TrailingBytes,  // Bytes left over after the last block the count promises.
    BadMessage,     // A block holds a message that does not decode.
};

std::ostream& operator<<(std::ostream& os, PacketStatus status);

struct CheckedPacket {
    PacketStatus status = PacketStatus::TooShort;
    PacketHeader header;                                     // Valid unless status is TooShort.
    FeedDecodeStatus message_status = FeedDecodeStatus::Ok;  // Why a BadMessage failed.
};

// Checks a whole packet before any of it is used. It reads the header, walks every block,
// and decodes every message. A receiver applies a packet only if this says Ok, so a bad
// packet is dropped as a whole, the same as a lost one.
[[nodiscard]] CheckedPacket check_packet(std::span<const std::byte> packet) noexcept;

// Walks the message blocks of a packet that check_packet accepted.
class BlockReader {
public:
    explicit BlockReader(std::span<const std::byte> packet) noexcept;

    // Sets `message` to the next message's bytes and returns true, or returns false after
    // the last one.
    bool next(std::span<const std::byte>& message) noexcept;

private:
    std::span<const std::byte> packet_;
    std::size_t offset_ = kHeaderLength;
    std::size_t left_ = 0;
};

// Builds one packet at a time in a fixed buffer, so it never allocates.
//
// Invariants. The buffer always starts with a header for the current packet. size() is at
// most limit(). Every appended message is a whole block, so the bytes are always a valid
// packet once finish() writes the count.
class PacketBuilder {
public:
    // Throws std::invalid_argument unless limit is between kMinPacketLength and
    // kMaxPacketLength.
    PacketBuilder(const SessionName& session, std::size_t limit);

    // Starts a new, empty packet whose first message will have sequence number `first`.
    void start(Sequence first) noexcept;

    // Adds one message as a block. Returns false and leaves the packet unchanged if the
    // message does not fit in the rest of the packet or cannot be encoded.
    template <class Message>
    bool append(const Message& message) noexcept {
        // A count of 0xFFFF would mean end of session, so a packet holds fewer messages.
        if (count_ == kEndOfSessionCount - 1 || size_ + kBlockLengthSize > limit_) {
            return false;
        }
        // The encoder gets exactly the room left before the limit, and it refuses a
        // message that does not fit in it.
        const std::size_t start = size_ + kBlockLengthSize;
        const std::span<std::byte> room =
            std::span<std::byte>(buffer_).subspan(start, limit_ - start);
        const std::size_t length = encode(message, room);
        if (length == 0) {
            return false;
        }
        write_block_length(length);
        size_ += kBlockLengthSize + length;
        ++count_;
        return true;
    }

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
    [[nodiscard]] Sequence first() const noexcept { return first_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

    // Writes the message count into the header and returns the finished packet. The bytes
    // stay valid until the next start() or control packet.
    [[nodiscard]] std::span<const std::byte> finish() noexcept;

    // Builds a heartbeat or an end of session packet with no messages. `next` is the
    // sequence number the stream will use next. The current packet is thrown away.
    [[nodiscard]] std::span<const std::byte> heartbeat(Sequence next) noexcept;
    [[nodiscard]] std::span<const std::byte> end_of_session(Sequence next) noexcept;

private:
    void write_block_length(std::size_t length) noexcept;
    std::span<const std::byte> control(Sequence next, std::uint16_t count) noexcept;

    std::array<std::byte, kMaxPacketLength> buffer_{};
    std::size_t limit_;
    std::size_t size_ = kHeaderLength;
    std::uint16_t count_ = 0;
    Sequence first_ = 1;
};

}  // namespace litbook::feed
