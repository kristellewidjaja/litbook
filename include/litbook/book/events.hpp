#pragma once

#include "litbook/types.hpp"

#include <cstdint>
#include <iosfwd>

namespace litbook {

// Why a new order or a cancel was refused.
enum class RejectReason : std::uint8_t {
    DuplicateOrderId,  // a resting order already has this ID
    ZeroQuantity,      // quantity must be at least 1
    UnknownOrderId,    // cancel for an ID that is not resting (never existed, filled, or canceled)
};

// ---------------------------------------------------------------------------
// Events emitted by an order book. Plain values: cheap to copy, comparable.
//
// Ordering contract for one call (identical for every book implementation):
//   add_limit, rejected:  OrderRejected
//   add_limit, accepted:  OrderAccepted, then per fill {Trade, LevelUpdate(resting side)},
//                         then LevelUpdate(own side) if any quantity rests
//   cancel, found:        OrderCanceled, LevelUpdate
//   cancel, not found:    CancelRejected
// ---------------------------------------------------------------------------

struct OrderAccepted {
    OrderId order_id;
    Side side;
    Price price;
    Qty qty;
    bool operator==(const OrderAccepted&) const = default;
};

struct OrderRejected {
    OrderId order_id;
    RejectReason reason;
    bool operator==(const OrderRejected&) const = default;
};

// One execution between an incoming (aggressor) order and a resting order.
// Invariant: price is the resting order's price; qty >= 1.
struct Trade {
    MatchId match_id;
    OrderId aggressor_id;
    OrderId resting_id;
    Side aggressor_side;
    Price price;
    Qty qty;
    Qty aggressor_remaining;  // aggressor's quantity still unfilled after this trade
    Qty resting_remaining;    // resting order's quantity left after this trade (0 = fully filled)
    bool operator==(const Trade&) const = default;
};

struct OrderCanceled {
    OrderId order_id;
    Qty canceled_qty;  // the quantity that was still resting
    bool operator==(const OrderCanceled&) const = default;
};

struct CancelRejected {
    OrderId order_id;
    RejectReason reason;
    bool operator==(const CancelRejected&) const = default;
};

// New state of one price level. total_qty == 0 and order_count == 0 means the level
// no longer exists.
struct LevelUpdate {
    Side side;
    Price price;
    AggregateQty total_qty;
    std::uint32_t order_count;
    bool operator==(const LevelUpdate&) const = default;
};

std::ostream& operator<<(std::ostream& os, RejectReason reason);
std::ostream& operator<<(std::ostream& os, const OrderAccepted& e);
std::ostream& operator<<(std::ostream& os, const OrderRejected& e);
std::ostream& operator<<(std::ostream& os, const Trade& e);
std::ostream& operator<<(std::ostream& os, const OrderCanceled& e);
std::ostream& operator<<(std::ostream& os, const CancelRejected& e);
std::ostream& operator<<(std::ostream& os, const LevelUpdate& e);

}  // namespace litbook
