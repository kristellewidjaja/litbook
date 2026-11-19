#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/container/flat_id_map.hpp"
#include "litbook/container/index_pool.hpp"
#include "litbook/container/occupancy_bitmap.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace litbook {

// Single-symbol limit order book with the same rules and the same event stream as
// MapOrderBook, built for speed: after the constructor returns, no operation allocates.
//
// Layout (all sized once, from BookConfig):
//   - Price ladder: one Level per tick in [min_price, max_price], per side, in a flat
//     array. Price -> level is arithmetic ((price - min_price) / tick_size), not a tree
//     lookup.
//   - Occupancy bitmap per side: one bit per level, so the best price moves past empty
//     levels 64 at a time.
//   - Order pool: max_orders nodes, addressed by 32-bit index. Each level's queue is an
//     intrusive doubly linked list threaded through the nodes (prev/next indexes), so
//     joining or leaving a queue is O(1) and touches no allocator.
//   - ID index: FlatIdMap (open addressing) from OrderId to pool index.
//
// Requirements beyond validate_config (constructor throws std::invalid_argument):
//   - a finite band: min_price and max_price are multiples of tick_size and the band
//     has at most kMaxLevels ticks;
//   - max_orders <= kMaxOrders.
// Memory: 2 sides * levels * 24 B, plus per order 36 B (node + free-list slot) and
// 32-64 B of ID index (the table is a power of two at least twice max_orders).
//
// Invariants (verified by check_invariants(), true whenever no call is in progress):
//   1. Never crossed: if both sides are non-empty, best_bid() < best_ask().
//   2. A level is non-empty iff its occupancy bit is set; level_count is the number of
//      set bits; the best index is the highest set bid / lowest set ask (none if empty).
//   3. Each level's list is well formed (head/tail/prev/next agree), its count is the
//      list length and its total_qty the sum of remaining quantities.
//   4. Every resting order has remaining >= 1, a multiple of lot_size, and records the
//      side and level it is linked into.
//   5. The ID index has exactly one entry per resting order, pointing at its node;
//      pool.in_use() == order_count() <= max_orders.
//   6. Within a level, list order is priority order (appends only at the tail).
//
// Not thread-safe. Events go synchronously to the listener, which must outlive the book.
class FastOrderBook {
public:
    static constexpr std::size_t kMaxLevels = std::size_t{1} << 22;  // per side
    static constexpr std::uint32_t kMaxOrders = std::uint32_t{1} << 24;

    FastOrderBook(OrderBookListener& listener, BookConfig config);

    void submit(const OrderRequest& request);
    void cancel(OrderId id);
    void modify(OrderId id, Price new_price, Qty new_qty);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] AggregateQty volume_at(Side side, Price price) const noexcept;
    [[nodiscard]] std::vector<LevelSummary> depth(Side side, std::size_t max_levels) const;
    [[nodiscard]] bool contains(OrderId id) const noexcept { return ids_.contains(id); }
    [[nodiscard]] std::size_t order_count() const noexcept { return pool_.in_use(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept;
    [[nodiscard]] const BookConfig& config() const noexcept { return config_; }

    // Checks every invariant listed above. O(levels + orders); meant for tests.
    [[nodiscard]] bool check_invariants() const;

private:
    using Index = IndexPool<int>::Index;
    static constexpr Index kNone = IndexPool<int>::kNone;
    static constexpr std::size_t kNoLevel = OccupancyBitmap::kNotFound;

    struct Node {  // 32 bytes: two orders per cache line
        OrderId id = 0;
        OwnerId owner = kNoOwner;
        Qty remaining = 0;
        Index prev = kNone;
        Index next = kNone;
        std::uint32_t level = 0;  // ladder index of the level this order rests at
        Side side = Side::Buy;
        bool post_only = false;
    };

    struct Level {  // 24 bytes
        AggregateQty total_qty = 0;
        Index head = kNone;  // highest priority = next to fill
        Index tail = kNone;
        std::uint32_t count = 0;
    };

    struct Ladder {
        explicit Ladder(std::size_t size) : levels(size), occupied(size) {}
        std::vector<Level> levels;
        OccupancyBitmap occupied;
        std::size_t best = kNoLevel;  // bids: highest occupied; asks: lowest occupied
        std::size_t level_count = 0;
    };

    struct Aggressor {
        OrderId id;
        OwnerId owner;
        Side side;
        std::size_t limit;  // ladder index; the far end of the ladder for market orders
        Qty remaining;
    };

    [[nodiscard]] Ladder& ladder(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }
    [[nodiscard]] const Ladder& ladder(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    [[nodiscard]] bool on_tick(Price price) const noexcept;
    [[nodiscard]] bool in_band(Price price) const noexcept;
    [[nodiscard]] std::size_t to_index(Price price) const noexcept;  // pre: on tick, in band
    [[nodiscard]] Price to_price(std::size_t index) const noexcept;
    // The next level after `index` moving away from the best price, or kNoLevel.
    [[nodiscard]] static std::size_t next_worse(const Ladder& ladder, Side side,
                                                std::size_t index) noexcept;
    [[nodiscard]] static bool crosses(const Aggressor& aggressor, std::size_t index) noexcept;

    [[nodiscard]] std::optional<RejectReason> validate(const OrderRequest& request) const noexcept;
    [[nodiscard]] bool would_cross(Side side, Price price) const noexcept;
    [[nodiscard]] bool can_fill_completely(const Aggressor& aggressor) const noexcept;

    void match(Aggressor& aggressor);
    void prevent_self_trade(Ladder& resting, Side resting_side, std::size_t index,
                            Aggressor& aggressor);
    void cancel_front(Ladder& resting, Side side, std::size_t index, CancelReason reason);

    void rest(const Aggressor& aggressor, bool post_only);  // pre: the pool is not full
    void link_back(Ladder& ladder, std::size_t index, Index node) noexcept;
    void unlink(Ladder& ladder, Index node) noexcept;
    void remove(Index node) noexcept;  // unlink + forget the ID + free the node
    void emit_level_update(Side side, std::size_t index);

    [[nodiscard]] bool check_side(Side side, std::size_t& orders_seen) const;

    OrderBookListener* listener_;  // never null
    BookConfig config_;
    std::size_t levels_;  // ticks in the band (same for both sides)
    Ladder bids_;
    Ladder asks_;
    IndexPool<Node> pool_;
    FlatIdMap ids_;
    MatchId next_match_id_ = 1;
};

static_assert(OrderBook<FastOrderBook>);

}  // namespace litbook
