#pragma once

#include "litbook/book/listener.hpp"
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

// Aggregated view of one price level.
struct LevelSummary {
    Price price;
    AggregateQty total_qty;
    std::uint32_t order_count;
    bool operator==(const LevelSummary&) const = default;
};

// Single-symbol limit order book with price-then-time priority, built from standard
// containers. This is the *correctness reference*: simple enough to trust, and every
// faster book must produce exactly the same event stream.
//
// Invariants (verified by check_invariants(), true whenever no call is in progress):
//   1. Never crossed: if both sides are non-empty, best_bid() < best_ask().
//   2. No empty levels: every price level holds at least one order.
//   3. Every resting order has remaining quantity >= 1.
//   4. A level's total_qty equals the sum of its orders' remaining quantities.
//   5. Within a level, orders are in arrival order (earliest first).
//   6. The ID index has exactly one entry per resting order, pointing at that order.
//
// Not thread-safe. Events go synchronously to the listener, which must outlive the book.
class MapOrderBook {
public:
    explicit MapOrderBook(OrderBookListener& listener) noexcept;

    // Submits a limit order. It trades against the opposite side while prices cross;
    // any remaining quantity rests at `price`. Rejected if qty == 0 or if `id` is
    // already resting. IDs of filled or canceled orders may be reused.
    void add_limit(OrderId id, Side side, Price price, Qty qty);

    // Cancels the resting quantity of order `id`, or emits CancelRejected if it is not resting.
    void cancel(OrderId id);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;

    // Total resting quantity at one price (0 if there is no level there).
    [[nodiscard]] AggregateQty volume_at(Side side, Price price) const;

    // Up to `max_levels` levels of one side, best price first.
    [[nodiscard]] std::vector<LevelSummary> depth(Side side, std::size_t max_levels) const;

    [[nodiscard]] bool contains(OrderId id) const;
    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept;

    // Checks every invariant listed above. O(number of orders); meant for tests.
    [[nodiscard]] bool check_invariants() const;

private:
    struct RestingOrder {
        OrderId id;
        Qty remaining;
    };

    struct Level {
        std::list<RestingOrder> orders;  // front = oldest = next to fill
        AggregateQty total_qty = 0;
    };

    // Bids sorted high-to-low and asks low-to-high, so begin() is always the best price.
    using BidLevels = std::map<Price, Level, std::greater<>>;
    using AskLevels = std::map<Price, Level, std::less<>>;

    struct Location {
        Side side;
        Price price;
        std::list<RestingOrder>::iterator order;
    };

    template <class Levels>
    Qty match(Levels& opposite_levels, OrderId aggressor_id, Side aggressor_side, Price limit,
              Qty remaining);

    template <class Levels>
    void rest(Levels& levels, OrderId id, Side side, Price price, Qty qty);

    template <class Levels>
    void remove(Levels& levels, OrderId id, const Location& location);

    template <class Levels>
    [[nodiscard]] bool check_side(const Levels& levels, Side side, std::size_t& orders_seen) const;

    void emit_level_update(Side side, Price price, const Level& level);

    OrderBookListener* listener_;  // never null
    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, Location> index_;
    MatchId next_match_id_ = 1;
};

}  // namespace litbook
