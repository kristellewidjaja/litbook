#pragma once

#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace litbook {

// Why a new order, a cancel or a modify was refused.
enum class RejectReason : std::uint8_t {
    DuplicateOrderId,    // A resting order already has this ID.
    ZeroQuantity,        // The quantity must be at least 1.
    UnknownOrderId,      // A cancel or modify named an ID that is not resting.
    InvalidPrice,        // The limit price is not a multiple of the tick size.
    InvalidLotSize,      // The quantity is not a multiple of the lot size.
    InvalidOrderType,    // The fields contradict each other, e.g. a post-only market order.
    PostOnlyWouldCross,  // A post-only order (or a modify of one) would trade on arrival.
    PriceOutOfRange,     // The limit price is outside the book's [min_price, max_price] band.
};

// Number of RejectReason values, for tables indexed by reason.
inline constexpr std::size_t kRejectReasonCount = 8;

// Why quantity left the book without trading.
enum class CancelReason : std::uint8_t {
    UserRequested,        // cancel() was called for this order.
    Unfilled,             // An IOC, FOK or market remainder that could not trade immediately.
    SelfTradePrevention,  // The self-trade prevention policy removed it.
    BookFull,             // It would rest, but the book already holds max_orders resting orders.
};

// Events emitted by an order book. They are plain values, so they are cheap to copy and
// can be compared with ==.
//
// Ordering contract. Every book implementation emits events in exactly this order.
//
// 1. A submit() with an invalid request emits OrderRejected and stops.
// 2. A submit() with a valid request emits OrderAccepted, and then:
//    a. If it is a FOK order that cannot fill completely, the book emits
//       OrderCanceled(Unfilled) and stops.
//    b. Otherwise the book walks the resting orders in priority order. For each one:
//       i.  If the owners differ, it emits Trade, then LevelUpdate(resting side).
//       ii. If the owners are the same (a self-trade), it follows the policy.
//           CancelNewest emits OrderCanceled(aggressor) and stops.
//           CancelOldest emits OrderCanceled(resting), then LevelUpdate.
//           CancelBoth emits OrderCanceled(resting), LevelUpdate, then
//             OrderCanceled(aggressor), and stops.
//           Decrement emits OrderCanceled(resting, partial), LevelUpdate, then
//             OrderCanceled(aggressor, partial).
//    c. For any remainder, the book emits LevelUpdate(own side) if the remainder rests
//       (a GoodTillCancel limit order). If it would rest but the book already holds
//       max_orders, the book emits OrderCanceled(BookFull). Otherwise it emits
//       OrderCanceled(Unfilled).
// 3. A cancel() of a resting order emits OrderCanceled(UserRequested), then LevelUpdate.
// 4. A cancel() of an order that is not resting emits CancelRejected.
// 5. A modify() with an invalid request emits ModifyRejected.
// 6. A modify() that keeps priority (same price, quantity not increased) emits
//    OrderModified(kept_priority), then LevelUpdate.
// 7. A modify() that loses priority (price changed or quantity increased) emits
//    OrderModified, then LevelUpdate(old level). The order is then re-entered exactly like
//    a GoodTillCancel limit order, starting from the walk in step 2b.

struct OrderAccepted {
    OrderId order_id;
    OwnerId owner;
    Side side;
    OrderType type;
    TimeInForce tif;
    bool post_only;
    Price price;  // 0 for market orders.
    Qty qty;
    bool operator==(const OrderAccepted&) const = default;
};

struct OrderRejected {
    OrderId order_id;
    RejectReason reason;
    bool operator==(const OrderRejected&) const = default;
};

// One execution between an incoming (aggressor) order and a resting order. These
// invariants hold. The price is the resting order's price, qty >= 1, and the two owners
// differ (or at least one of them is kNoOwner).
struct Trade {
    MatchId match_id;
    OrderId aggressor_id;
    OrderId resting_id;
    Side aggressor_side;
    Price price;
    Qty qty;
    Qty aggressor_remaining;  // Aggressor quantity still unfilled after this trade.
    Qty resting_remaining;    // Resting quantity left after this trade (0 means fully filled).
    bool operator==(const Trade&) const = default;
};

// Quantity removed from an order without trading. remaining_qty == 0 means the order is
// gone. remaining_qty > 0 happens only with SelfTradePolicy::Decrement.
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

// A resting order's price, remaining quantity, or both changed through modify().
struct OrderModified {
    OrderId order_id;
    Side side;
    Price price;  // The new price.
    Qty qty;      // The new remaining quantity.
    bool kept_priority;
    bool operator==(const OrderModified&) const = default;
};

struct ModifyRejected {
    OrderId order_id;
    RejectReason reason;
    bool operator==(const ModifyRejected&) const = default;
};

// The new state of one price level. If total_qty == 0 and order_count == 0, the level
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
