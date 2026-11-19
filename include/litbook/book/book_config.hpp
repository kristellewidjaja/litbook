#pragma once

#include "litbook/types.hpp"

#include <cstdint>
#include <iosfwd>
#include <limits>

namespace litbook {

// What to do when an incoming order would trade against a resting order with the
// same (non-zero) owner.
enum class SelfTradePolicy : std::uint8_t {
    CancelNewest,  // Cancel the incoming order's remaining quantity. The resting order stays.
    CancelOldest,  // Cancel the resting order. The incoming order keeps matching.
    CancelBoth,    // Cancel both orders.
    Decrement,  // Reduce both by the smaller quantity without a trade; a 0 quantity order is gone.
};

std::ostream& operator<<(std::ostream& os, SelfTradePolicy policy);

// Per-instrument rules, fixed for the life of a book. Every book implementation
// enforces all of them in the same way. validate_config, which every book's constructor
// calls, checks the invariants tick_size >= 1, lot_size >= 1, min_price <= max_price
// and max_orders >= 1.
struct BookConfig {
    // Prices are integers in the system's smallest unit, and valid prices are multiples of
    // tick_size. For example, with prices in 1/10000 dollars, tick_size 100 means $0.01 ticks.
    Price tick_size = 1;
    // Valid quantities are multiples of lot_size (for example, 100-share round lots).
    Qty lot_size = 1;
    SelfTradePolicy self_trade_policy = SelfTradePolicy::CancelNewest;
    // Limit prices outside the band [min_price, max_price] are rejected with PriceOutOfRange.
    // The defaults mean there is no band. FastOrderBook needs a finite one.
    Price min_price = std::numeric_limits<Price>::min();
    Price max_price = std::numeric_limits<Price>::max();
    // The most orders that may rest at once. If a new order's remainder would rest while the
    // book is full, it is canceled with CancelReason::BookFull after any trading it can do.
    std::uint32_t max_orders = std::numeric_limits<std::uint32_t>::max();
};

// Throws std::invalid_argument if `config` breaks any of the invariants above.
void validate_config(const BookConfig& config);

}  // namespace litbook
