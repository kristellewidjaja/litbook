#pragma once

#include "litbook/engine/ticker.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <variant>

namespace litbook::protocol {

// The order entry protocol, modeled on Nasdaq OUCH 5.0. docs/PROTOCOL.md has the byte
// layout of every message and a list of every place where it differs from OUCH.
//
// These structs hold decoded values in the engine's own types. They are not the wire
// layout, and their size and padding do not matter. The codec (codec.hpp) is the only
// code that knows where each field sits on the wire.

// A number the client picks for each order it sends. OUCH requires it to be unique and
// strictly increasing per account for the whole day. That rule is enforced by the order
// entry layer, not the codec.
using UserRefNum = std::uint32_t;

// Nanoseconds since midnight, as in OUCH and ITCH.
using Timestamp = std::uint64_t;

// The exchange's own number for an order, sent back in AcceptedMessage. It is unique for
// the whole day.
using OrderRef = std::uint64_t;

// Prices on the wire are unsigned with 4 implied decimals, so 1'000'000 means $100.0000.
// That is the same unit the books use, so no conversion is needed. OUCH supports
// prices up to $199,999.9900, and anything above it is rejected by the decoder.
inline constexpr Price kMaxWirePrice = 1'999'999'900;

// Message type bytes. Inbound messages go from client to exchange, outbound the other way.
// The same letter can mean different things in the two directions ('U'), so every decode
// call says which direction it expects.
inline constexpr char kEnterOrderType = 'O';
inline constexpr char kReplaceOrderType = 'U';
inline constexpr char kCancelOrderType = 'X';
inline constexpr char kAcceptedType = 'A';
inline constexpr char kReplacedType = 'U';
inline constexpr char kCanceledType = 'C';
inline constexpr char kExecutedType = 'E';
inline constexpr char kRejectedType = 'J';

// The exact length in bytes of each message, type byte included. Every message has a fixed
// length, so a frame of any other length is malformed.
inline constexpr std::size_t kEnterOrderLength = 29;
inline constexpr std::size_t kReplaceOrderLength = 21;
inline constexpr std::size_t kCancelOrderLength = 9;
inline constexpr std::size_t kAcceptedLength = 45;
inline constexpr std::size_t kReplacedLength = 47;
inline constexpr std::size_t kCanceledLength = 18;
inline constexpr std::size_t kExecutedLength = 34;
inline constexpr std::size_t kRejectedLength = 15;

// The longest message in either direction, so a caller can size one buffer for any message.
inline constexpr std::size_t kMaxMessageLength = 47;

// Why the exchange quantity left an order without trading. The letters are OUCH's.
enum class WireCancelReason : char {
    UserRequested = 'U',        // The client sent a cancel.
    ImmediateOrCancel = 'I',    // The unfilled part of an IOC, FOK or market order.
    SelfMatchPrevention = 'Q',  // It would have traded with the same owner.
    // The exchange canceled it, because the book was full or because the account's
    // connection dropped and the account asked for cancel on disconnect.
    System = 'Z',
};

// Why an order or a replace was refused. The values below 0x0100 are OUCH's own codes.
// Values from 0x0100 up are litbook extensions for rules OUCH has no code for.
enum class WireRejectReason : std::uint16_t {
    Other = 0x000F,
    InvalidQuantity = 0x0013,
    InvalidSymbol = 0x0017,
    InvalidPrice = 0x001D,
    MaxQuantityExceeded = 0x0031,  // OUCH's "Risk: Max Quantity Exceeded".
    PostOnlyWouldCross = 0x0101,
    InvalidOrderType = 0x0102,
    MaxOpenOrdersExceeded = 0x0103,  // The account already has its limit of live orders.
};

// Whether an execution added liquidity (the order was resting) or removed it (the order
// was the incoming aggressor). The letters are OUCH's.
enum class Liquidity : char {
    Added = 'A',
    Removed = 'R',
};

// Client to exchange.

struct EnterOrder {
    UserRefNum user_ref;
    Side side;
    Qty qty;
    Ticker symbol;
    Price price;  // 0 for a market order.
    TimeInForce tif;
    OrderType type;
    bool post_only;
    bool operator==(const EnterOrder&) const = default;
};

// Changes the price and size of a live order and gives it a new UserRefNum. As in OUCH,
// qty is the total number of shares the order may ever execute, including shares that
// already executed.
struct ReplaceOrder {
    UserRefNum orig_user_ref;
    UserRefNum user_ref;
    Qty qty;
    Price price;
    bool operator==(const ReplaceOrder&) const = default;
};

// Reduces an order so that at most qty shares can execute in total, counting shares that
// already executed. A qty of 0 cancels everything that is still open.
struct CancelOrder {
    UserRefNum user_ref;
    Qty qty;
    bool operator==(const CancelOrder&) const = default;
};

// Exchange to client.

struct AcceptedMessage {
    Timestamp timestamp;
    UserRefNum user_ref;
    Side side;
    Qty qty;
    Ticker symbol;
    Price price;
    TimeInForce tif;
    OrderType type;
    bool post_only;
    OrderRef order_ref;
    bool operator==(const AcceptedMessage&) const = default;
};

struct ReplacedMessage {
    Timestamp timestamp;
    UserRefNum orig_user_ref;
    UserRefNum user_ref;
    Side side;
    Qty qty;  // Shares still open after the replace.
    Ticker symbol;
    Price price;
    OrderRef order_ref;
    bool kept_priority;  // True if the order kept its place in the queue.
    bool operator==(const ReplacedMessage&) const = default;
};

struct CanceledMessage {
    Timestamp timestamp;
    UserRefNum user_ref;
    Qty qty;  // Shares taken off by this cancel only, not a running total.
    WireCancelReason reason;
    bool operator==(const CanceledMessage&) const = default;
};

struct ExecutedMessage {
    Timestamp timestamp;
    UserRefNum user_ref;
    Qty qty;  // Shares in this execution only.
    Price price;
    Liquidity liquidity;
    MatchId match;  // Both sides of a trade get the same match number.
    bool operator==(const ExecutedMessage&) const = default;
};

struct RejectedMessage {
    Timestamp timestamp;
    UserRefNum user_ref;
    WireRejectReason reason;
    bool operator==(const RejectedMessage&) const = default;
};

using InboundMessage = std::variant<EnterOrder, ReplaceOrder, CancelOrder>;
using OutboundMessage = std::variant<AcceptedMessage, ReplacedMessage, CanceledMessage,
                                     ExecutedMessage, RejectedMessage>;

}  // namespace litbook::protocol
