#pragma once

#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace litbook {

// Why a new order, cancel or modify was refused.
enum class RejectReason : std::uint8_t {
    DuplicateOrderId,    // a resting order already has this ID
    ZeroQuantity,        // quantity must be at least 1
    UnknownOrderId,      // cancel/modify for an ID that is not resting
    InvalidPrice,        // limit price is not a multiple of the tick size
    InvalidLotSize,      // quantity is not a multiple of the lot size
    InvalidOrderType,    // contradictory combination, e.g. post-only market order
    PostOnlyWouldCross,  // post-only order (or modify of one) would trade on arrival
    PriceOutOfRange,     // limit price outside the book's [min_price, max_price] band
};

// Number of RejectReason values (for tables indexed by reason).
inline constexpr std::size_t kRejectReasonCount = 8;

// Why quantity left the book without trading.
enum class CancelReason : std::uint8_t {
    UserRequested,        // cancel() was called
    Unfilled,             // IOC / FOK / market remainder that could not trade immediately
    SelfTradePrevention,  // removed by the self-trade prevention policy
    BookFull,             // would rest, but the book already holds max_orders resting orders
};

// ---------------------------------------------------------------------------
// Events emitted by an order book. Plain values: cheap to copy, comparable.
//
// Ordering contract (identical for every book implementation):
//
// submit, invalid:    OrderRejected
// submit, valid:      OrderAccepted, then
//                     FOK that cannot fill completely: OrderCanceled(Unfilled), stop.
//                     Otherwise, walking resting orders in priority order:
//                       - different owner: Trade, LevelUpdate(resting side)
//                       - same owner (self-trade), by policy:
//                           CancelNewest: OrderCanceled(aggressor); stop
//                           CancelOldest: OrderCanceled(resting), LevelUpdate
//                           CancelBoth:   OrderCanceled(resting), LevelUpdate,
//                                         OrderCanceled(aggressor); stop
//                           Decrement:    OrderCanceled(resting, partial), LevelUpdate,
//                                         OrderCanceled(aggressor, partial)
//                     then for any remainder: LevelUpdate(own side) if it rests
//                     (limit + GoodTillCancel), OrderCanceled(BookFull) if it would rest
//                     but the book already holds max_orders, else OrderCanceled(Unfilled).
// cancel, found:      OrderCanceled(UserRequested), LevelUpdate
// cancel, not found:  CancelRejected
// modify, invalid:    ModifyRejected
// modify, keeps priority (same price, qty not increased):
//                     OrderModified(kept_priority), LevelUpdate
// modify, loses priority (price changed or qty increased):
//                     OrderModified, LevelUpdate(old level), then the order is
//                     re-entered exactly like a GoodTillCancel limit order from the
//                     "walking resting orders" step above.
// ---------------------------------------------------------------------------

struct OrderAccepted {
    OrderId order_id;
    OwnerId owner;
    Side side;
    OrderType type;
    TimeInForce tif;
    bool post_only;
    Price price;  // 0 for market orders
    Qty qty;
    bool operator==(const OrderAccepted&) const = default;
};

struct OrderRejected {
    OrderId order_id;
    RejectReason reason;
    bool operator==(const OrderRejected&) const = default;
};

// One execution between an incoming (aggressor) order and a resting order.
// Invariant: price is the resting order's price; qty >= 1; the two owners differ
// (or at least one is kNoOwner).
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

// Quantity removed from an order without trading.
// remaining_qty == 0 means the order is gone; > 0 only for SelfTradePolicy::Decrement.
struct OrderCanceled {
    OrderId order_id;
    Qty canceled_qty;
    Qty remaining_qty;
    CancelReason reason;
    bool operator==(const OrderCanceled&) const = default;
};

struct CancelRejected {
    OrderId order_id;
    RejectReason reason;
    bool operator==(const CancelRejected&) const = default;
};

// A resting order's price and/or remaining quantity changed via modify().
struct OrderModified {
    OrderId order_id;
    Side side;
    Price price;  // new price
    Qty qty;      // new remaining quantity
    bool kept_priority;
    bool operator==(const OrderModified&) const = default;
};

struct ModifyRejected {
    OrderId order_id;
    RejectReason reason;
    bool operator==(const ModifyRejected&) const = default;
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
std::ostream& operator<<(std::ostream& os, CancelReason reason);
std::ostream& operator<<(std::ostream& os, const OrderAccepted& e);
std::ostream& operator<<(std::ostream& os, const OrderRejected& e);
std::ostream& operator<<(std::ostream& os, const Trade& e);
std::ostream& operator<<(std::ostream& os, const OrderCanceled& e);
std::ostream& operator<<(std::ostream& os, const CancelRejected& e);
std::ostream& operator<<(std::ostream& os, const OrderModified& e);
std::ostream& operator<<(std::ostream& os, const ModifyRejected& e);
std::ostream& operator<<(std::ostream& os, const LevelUpdate& e);

}  // namespace litbook
