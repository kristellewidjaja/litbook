#pragma once

#include "litbook/protocol/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>

namespace litbook::protocol {

// Why a frame could not be decoded. A frame that fails here is malformed, meaning the
// bytes themselves are wrong. That is different from a well-formed order that breaks a
// trading rule (a zero quantity, a price off the tick), which decodes fine and is then
// refused with a RejectedMessage by the order entry layer.
enum class DecodeStatus : std::uint8_t {
    Ok,
    Empty,           // The frame has no bytes, so there is no type byte.
    UnknownType,     // The first byte is not a message type for this direction.
    TooShort,        // Fewer bytes than this message type needs.
    TooLong,         // More bytes than this message type has.
    BadSide,         // Not 'B' or 'S'.
    BadSymbol,       // Not 1 to 8 printable characters, left-justified and space-padded.
    BadPrice,        // Above kMaxWirePrice.
    BadTimeInForce,  // Not '0', '3' or '4'.
    BadOrderType,    // Not 'L' or 'M'.
    BadFlag,         // A yes/no field that is not 'Y' or 'N'.
};

std::ostream& operator<<(std::ostream& os, DecodeStatus status);

// The result of decoding one frame. `message` holds a value exactly when status is Ok.
template <class Message>
struct Decoded {
    DecodeStatus status;
    std::optional<Message> message;
};

// Decodes one whole frame. The transport (SoupBinTCP style length framing in Phase 6)
// says where a frame starts and ends, so the frame must be exactly one message long.
//
// Fields with a fixed set of values (side, time in force, order type, yes/no flags,
// symbol) are checked strictly in both directions. The reason codes and the liquidity flag
// in outbound messages are open sets and any byte value is accepted, so a client built
// against this codec keeps working if the exchange adds a new code. Every frame that
// decodes re-encodes to exactly the same bytes, which the fuzz target checks.
//
// Neither function allocates or throws.
[[nodiscard]] Decoded<InboundMessage> decode_inbound(std::span<const std::byte> frame) noexcept;
[[nodiscard]] Decoded<OutboundMessage> decode_outbound(std::span<const std::byte> frame) noexcept;

// The length of the message whose type byte is `type`, or 0 if no such message exists in
// that direction. A framing layer can use it to check a length prefix early.
[[nodiscard]] std::size_t inbound_length(std::byte type) noexcept;
[[nodiscard]] std::size_t outbound_length(std::byte type) noexcept;

// Each encode function writes one message to the front of `out` and returns the number of
// bytes written. It returns 0 and leaves `out` unchanged if `out` is too small or if a
// field cannot be put on the wire (a price below 0 or above kMaxWirePrice). The encoders
// never allocate or throw.
[[nodiscard]] std::size_t encode(const EnterOrder& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const ReplaceOrder& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const CancelOrder& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const AcceptedMessage& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const ReplacedMessage& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const CanceledMessage& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const ExecutedMessage& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const RejectedMessage& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const InboundMessage& m, std::span<std::byte> out) noexcept;
[[nodiscard]] std::size_t encode(const OutboundMessage& m, std::span<std::byte> out) noexcept;

}  // namespace litbook::protocol
