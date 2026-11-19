#pragma once

#include "litbook/types.hpp"

#include <cstdint>
#include <iosfwd>
#include <limits>

namespace litbook {

// What to do when an incoming order would trade against a resting order with the
// same (non-zero) owner.
enum class SelfTradePolicy : std::uint8_t {
    CancelNewest,  // cancel the incoming order's remaining quantity; the resting order stays
    CancelOldest,  // cancel the resting order; the incoming order keeps matching
    CancelBoth,    // cancel both
    Decrement,  // reduce both by the smaller quantity, with no trade; an order reduced to 0 is gone
};

std::ostream& operator<<(std::ostream& os, SelfTradePolicy policy);

// Per-instrument rules, fixed for the life of a book. Every book implementation
// enforces all of them identically.
// Invariant (checked by validate_config, which every book's constructor calls):
//   tick_size >= 1, lot_size >= 1, min_price <= max_price, max_orders >= 1.
struct BookConfig {
    // Prices are integers in the system's smallest unit; valid prices are multiples of
    // tick_size. E.g. with prices in 1/10000 dollars, tick_size 100 means $0.01 ticks.
    Price tick_size = 1;
    // Valid quantities are multiples of lot_size (e.g. 100-share round lots).
    Qty lot_size = 1;
    SelfTradePolicy self_trade_policy = SelfTradePolicy::CancelNewest;
    // Price band: limit prices outside [min_price, max_price] are rejected
    // (PriceOutOfRange). The defaults mean "no band"; FastOrderBook needs a finite one.
    Price min_price = std::numeric_limits<Price>::min();
    Price max_price = std::numeric_limits<Price>::max();
    // Most orders that may rest at once. A new order whose remainder would rest when the
    // book is full is canceled (CancelReason::BookFull) after any trading it can do.
    std::uint32_t max_orders = std::numeric_limits<std::uint32_t>::max();
};

// Throws std::invalid_argument if `config` breaks the invariant above.
void validate_config(const BookConfig& config);

}  // namespace litbook
