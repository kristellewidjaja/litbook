#pragma once

#include "litbook/feed/feed_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>

namespace litbook::feed {

// Why a feed message could not be decoded.
enum class FeedDecodeStatus : std::uint8_t {
    Ok,
    Empty,         // No bytes, so there is no type byte.
    UnknownType,   // The first byte is not a feed message type.
    BadLength,     // The length is not exactly the length of this message type.
    BadEventCode,  // A System Event code that is not 'O' or 'C'.
    BadSymbol,     // Not 1 to 8 printable characters, left-justified and space-padded.
    BadPrice,      // Above kMaxWirePrice, or a tick size of 0.
    BadSide,       // Not 'B' or 'S'.
    BadLotSize,    // A lot size of 0.
    BadLevel,      // Exactly one of total quantity and order count is 0.
    BadQuantity,   // A trade of 0 shares.
    BadLocate,     // A Snapshot Start whose locate is not below its symbol count.
};

std::ostream& operator<<(std::ostream& os, FeedDecodeStatus status);

struct FeedDecoded {
    FeedDecodeStatus status = FeedDecodeStatus::Ok;
    std::optional<FeedMessage> message;  // Set exactly when status is Ok.
};

// Decodes one whole message. The MoldUDP64 message block says where it starts and ends,
// so `bytes` must be exactly one message long. Every message that decodes re-encodes to
// the same bytes. Neither function allocates or throws.
[[nodiscard]] FeedDecoded decode_message(std::span<const std::byte> bytes) noexcept;

// The length of the message whose type byte is `type`, or 0 if there is no such type.
[[nodiscard]] std::size_t message_length(std::byte type) noexcept;

// Each encode function writes one message to the front of `out` and returns its length.
// It returns 0 and writes nothing if `out` is too small or the message could not be
// decoded again (for example a negative price, a lot size of 0, or a level where only one
// of quantity and order count is 0).
[[nodiscard]] std::size_t encode(const SystemEvent& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const SymbolDirectory& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const LevelUpdateMessage& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const TradeMessage& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const SnapshotStart& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const SnapshotEnd& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const FeedMessage& m, std::span<std::byte> out) noexcept;

}  // namespace litbook::feed
