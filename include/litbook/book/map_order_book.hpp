#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace litbook {

// Single-symbol limit order book with price-then-time priority, built from standard
// containers. This is the *correctness reference*. It is simple enough to trust, and every
// faster book must produce exactly the same event stream (the contract is in events.hpp).
//
// It supports limit and market orders, GoodTillCancel, IOC and FOK, post-only orders,
// cancel, and modify (a quantity decrease at the same price keeps queue priority, and
// anything else loses it). It also supports self-trade prevention, tick-size and lot-size
// validation, a price band, and a maximum number of resting orders (all set in BookConfig).
//
// These invariants are true whenever no call is in progress, and check_invariants()
// verifies them.
//   1. The book is never crossed. If both sides are non-empty, best_bid() < best_ask().
//   2. There are no empty levels. Every price level holds at least one order.
//   3. Every resting order has remaining quantity >= 1.
//   4. A level's total_qty equals the sum of its orders' remaining quantities.
//   5. Within a level, orders are in priority order (earliest first).
//   6. The ID index has exactly one entry per resting order, pointing at that order.
//   7. Every resting price is a multiple of tick_size and inside [min_price, max_price],
//      and every remaining quantity is a multiple of lot_size.
//   8. order_count() <= max_orders.
//
// The book is not thread-safe. Events go synchronously to the listener, which must
// outlive the book.
class MapOrderBook {
public:
    // Throws std::invalid_argument if validate_config rejects `config`. Throwing is fine
    // here because construction happens at startup, not on the hot path.
    explicit MapOrderBook(OrderBookListener& listener, BookConfig config = {});

    // Enters a new order. Invalid requests are rejected with OrderRejected. See OrderRequest
    // for the valid combinations and events.hpp for the event sequence.
    void submit(const OrderRequest& request);

    // Cancels the resting quantity of order `id`, or emits CancelRejected if it is not resting.
    void cancel(OrderId id);

    // Changes a resting order's price and remaining quantity. Keeps queue priority only
    // when the price is unchanged and the quantity does not increase. Otherwise the
    // order goes to the back of the queue at the new price and may trade immediately.
    void modify(OrderId id, Price new_price, Qty new_qty);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;

    // Total resting quantity at one price, or 0 if there is no level there.
    [[nodiscard]] AggregateQty volume_at(Side side, Price price) const;

    // Returns up to `max_levels` levels of one side, best price first.
    [[nodiscard]] std::vector<LevelSummary> depth(Side side, std::size_t max_levels) const;

    [[nodiscard]] bool contains(OrderId id) const;
    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept;
    [[nodiscard]] const BookConfig& config() const noexcept { return config_; }

    // Checks every invariant listed above in O(number of orders) time. Meant for tests.
    [[nodiscard]] bool check_invariants() const;

    // Snapshot calls (see the OrderBook concept). for_each_resting visits one side's
    // orders, best price first and in priority order within a price.
    template <class Visit>
    void for_each_resting(Side side, Visit&& visit) const;

    // Puts one order at the back of its price's queue, with no event and no matching.
    // Returns why it refused, and then changes nothing. This is a startup call that
    // allocates like any insert into a std::map.
    RestoreStatus restore_resting(const RestingOrder& order);

    [[nodiscard]] MatchId next_match_id() const noexcept { return next_match_id_; }
    void restore_next_match_id(MatchId next) noexcept { next_match_id_ = next; }

private:
    struct QueuedOrder {
        OrderId id;
        OwnerId owner;
        Qty remaining;
        bool post_only;  // Remembered so that a later modify cannot make it cross.
    };

    struct Level {
        std::list<QueuedOrder> orders;  // The front order has the highest priority and fills next.
        AggregateQty total_qty = 0;
    };

    // Bids are sorted high to low and asks low to high, so begin() is always the best price.
    using BidLevels = std::map<Price, Level, std::greater<>>;
    using AskLevels = std::map<Price, Level, std::less<>>;

    struct Location {
        Side side;
        Price price;
        std::list<QueuedOrder>::iterator order;
    };

    // An incoming order (a new order, or a modify that lost priority) while it matches.
    struct Aggressor {
        OrderId id;
        OwnerId owner;
        Side side;
        Price limit;  // Market orders use an extreme value, so every price crosses.
        Qty remaining;
    };

    // Calls f(bids_) or f(asks_), so code that works on either side is written once.
    template <class F>
    decltype(auto) with_levels(Side side, F&& f);
    template <class F>
    decltype(auto) with_levels(Side side, F&& f) const;

    [[nodiscard]] std::optional<RejectReason> validate(const OrderRequest& request) const;
    [[nodiscard]] bool valid_price(Price price) const noexcept;
    [[nodiscard]] bool in_band(Price price) const noexcept;
    [[nodiscard]] bool valid_qty(Qty qty) const noexcept;
    [[nodiscard]] bool would_cross(Side side, Price price) const noexcept;
    [[nodiscard]] bool can_fill_completely(const Aggressor& aggressor) const;

    void match(Aggressor& aggressor);
    template <class Levels>
    void match_against(Levels& levels, Aggressor& aggressor);
    void prevent_self_trade(Level& level, Side resting_side, Price price, Aggressor& aggressor);
    void cancel_front(Level& level, Side side, Price price, CancelReason reason);

    void rest(const Aggressor& aggressor, bool post_only);
    // Links an order at the back of its price's queue and indexes it, without any event.
    // Both rest() and restore_resting() use it, so a restored order is stored exactly
    // like one that came to rest after matching.
    const Level& place(const RestingOrder& order);
    void emit_level_update(Side side, Price price, const Level& level);

    template <class Levels>
    [[nodiscard]] bool check_side(const Levels& levels, Side side, std::size_t& orders_seen) const;

    OrderBookListener* listener_;  // Never null.
    BookConfig config_;
    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, Location> index_;
    MatchId next_match_id_ = 1;
};

template <class Visit>
void MapOrderBook::for_each_resting(Side side, Visit&& visit) const {
    // begin() is the best price on both sides, see BidLevels and AskLevels.
    auto visit_side = [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            for (const QueuedOrder& order : level.orders) {
                visit(RestingOrder{.id = order.id,
                                   .owner = order.owner,
                                   .side = side,
                                   .price = price,
                                   .remaining = order.remaining,
                                   .post_only = order.post_only});
            }
        }
    };
    if (side == Side::Buy) {
        visit_side(bids_);
    } else {
        visit_side(asks_);
    }
}

static_assert(OrderBook<MapOrderBook>);

}  // namespace litbook
