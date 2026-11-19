#pragma once

#include "litbook/types.hpp"

#include <cstdint>
#include <iosfwd>

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

// Per-instrument rules, fixed for the life of a book.
// Invariant (enforced by the book's constructor): tick_size >= 1 and lot_size >= 1.
struct BookConfig {
    // Prices are integers in the system's smallest unit; valid prices are multiples of
    // tick_size. E.g. with prices in 1/10000 dollars, tick_size 100 means $0.01 ticks.
    Price tick_size = 1;
    // Valid quantities are multiples of lot_size (e.g. 100-share round lots).
    Qty lot_size = 1;
    SelfTradePolicy self_trade_policy = SelfTradePolicy::CancelNewest;
};

}  // namespace litbook
