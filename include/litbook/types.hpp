#pragma once

#include <cstdint>
#include <iosfwd>

namespace litbook {

// Price in integer ticks. Signed: some instruments (spreads, some futures) trade at
// negative prices, and price differences are naturally signed. No floating point.
using Price = std::int64_t;

// Quantity of a single order, in shares/contracts. 32 bits, matching the 4-byte
// quantity fields of NASDAQ OUCH and ITCH.
using Qty = std::uint32_t;

// Sum of many order quantities (e.g. a whole price level). 64 bits so it cannot
// overflow even if billions of maximum-size orders rest at one price.
using AggregateQty = std::uint64_t;

// Exchange-assigned order identifier. Unique among resting orders in a book.
using OrderId = std::uint64_t;

// Identifier of one trade (execution). Strictly increasing within a book, starting at 1.
using MatchId = std::uint64_t;

// Who submitted an order (a firm or trading session), used for self-trade prevention.
// kNoOwner is exempt: orders with owner 0 never count as self-trades.
using OwnerId = std::uint32_t;
inline constexpr OwnerId kNoOwner = 0;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
    Limit,   // trades at `price` or better; may rest
    Market,  // no price limit; never rests (any unfilled remainder is canceled)
};

enum class TimeInForce : std::uint8_t {
    GoodTillCancel,     // unfilled remainder rests until filled or canceled
    ImmediateOrCancel,  // trade what is possible now; cancel the rest
    FillOrKill,         // fill the whole quantity now, or cancel it all without trading
};

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

std::ostream& operator<<(std::ostream& os, Side side);
std::ostream& operator<<(std::ostream& os, OrderType type);
std::ostream& operator<<(std::ostream& os, TimeInForce tif);

}  // namespace litbook
