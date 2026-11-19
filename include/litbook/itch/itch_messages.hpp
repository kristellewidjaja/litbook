#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace litbook::itch {

// Decoded Nasdaq TotalView-ITCH 5.0 messages. Only the messages that change an order
// book are decoded field by field, plus the two that a book builder needs for context
// (the stock directory, which names each stock locate code, and system events). Every
// other message type is recognized by its type byte and length and then skipped.
//
// The field layouts come from the official specification, "Nasdaq TotalView-ITCH 5.0",
// sections 1.1, 1.2.1, 1.2.2, 1.3 and 1.4. All integers on the wire are big-endian and
// unsigned. Prices are "Price (4)", an integer with 4 implied decimal places, so
// 1234500 means $123.45. Timestamps are nanoseconds since midnight, sent as 6 bytes.

// Exact payload length of each message type, in bytes, from the specification's field
// tables (the offset of the last field plus its length).
inline constexpr std::size_t kSystemEventLength = 12;             // 'S'
inline constexpr std::size_t kStockDirectoryLength = 39;          // 'R'
inline constexpr std::size_t kStockTradingActionLength = 25;      // 'H'
inline constexpr std::size_t kAddOrderLength = 36;                // 'A'
inline constexpr std::size_t kAddOrderMpidLength = 40;            // 'F'
inline constexpr std::size_t kOrderExecutedLength = 31;           // 'E'
inline constexpr std::size_t kOrderExecutedWithPriceLength = 36;  // 'C'
inline constexpr std::size_t kOrderCancelLength = 23;             // 'X'
inline constexpr std::size_t kOrderDeleteLength = 19;             // 'D'
inline constexpr std::size_t kOrderReplaceLength = 35;            // 'U'

// Every message starts with these fields at offsets 0 to 10.
struct Header {
    char type;
    std::uint16_t stock_locate;     // which stock, 0 for system messages
    std::uint16_t tracking_number;  // Nasdaq internal
    std::uint64_t timestamp_ns;     // nanoseconds since midnight
};

struct SystemEvent {  // 'S'
    Header header;
    char event_code;  // 'O' start of messages, 'Q' market open, 'M' market close, ...
};

struct StockDirectory {  // 'R'. Only the fields a book builder uses are decoded.
    Header header;
    std::array<char, 8> stock;  // symbol, padded on the right with spaces
    std::uint32_t round_lot_size;
};

// 'H'. A change in a stock's trading state. During a halt, pause or quotation-only
// period Nasdaq accepts orders but does not match them, so the book can cross until a
// reopening auction clears it.
struct StockTradingAction {
    Header header;
    std::array<char, 8> stock;
    char trading_state;  // 'H' halted, 'P' paused, 'Q' quotation only, 'T' trading
    std::array<char, 4> reason;
};

// 'A' (no attribution) and 'F' (with a market participant ID) differ only in that last
// field, so both decode into this struct.
struct AddOrder {
    Header header;
    std::uint64_t order_ref;  // unique for the day
    char side;                // 'B' buy or 'S' sell
    std::uint32_t shares;
    std::array<char, 8> stock;
    std::uint32_t price;       // Price (4)
    bool has_mpid;             // true for 'F'
    std::array<char, 4> mpid;  // spaces when has_mpid is false
};

struct OrderExecuted {  // 'E', executed at the order's own display price
    Header header;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};

struct OrderExecutedWithPrice {  // 'C', executed at a different price
    Header header;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
    char printable;                 // 'Y' or 'N'
    std::uint32_t execution_price;  // Price (4)
};

struct OrderCancel {  // 'X', a partial cancel
    Header header;
    std::uint64_t order_ref;
    std::uint32_t canceled_shares;
};

struct OrderDelete {  // 'D', the whole order is gone
    Header header;
    std::uint64_t order_ref;
};

// 'U'. The original order is removed and a new order with a new reference number takes
// its place, with the same side and stock but a new size and price (and no time
// priority from the original).
struct OrderReplace {
    Header header;
    std::uint64_t original_order_ref;
    std::uint64_t new_order_ref;
    std::uint32_t shares;
    std::uint32_t price;  // Price (4)
};

}  // namespace litbook::itch
