#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/container/flat_id_map.hpp"
#include "litbook/container/index_pool.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace litbook {

// Single-symbol limit order book with the same rules and the same event stream as
// MapOrderBook and FastOrderBook. Like FastOrderBook it never allocates after the
// constructor, but it does not need a price band, so it can hold real market data.
//
// Why a third book. FastOrderBook keeps one slot per possible price, which needs a narrow
// price band. A real Nasdaq day breaks that. Market makers post "stub quotes" at $0.0001
// and $199,999.99, and most stocks have resting orders that far apart, which would take
// two billion slots per stock. Real orders do cluster near the best price, though. This
// book stores only the price levels that have orders, in a sorted array per side with the
// best price at the end, so the busy end of the array is where the work happens.
//
// Layout.
// Each side has a sorted array of entries {price, level slot}, ordered from the worst
// price at the front to the best at the back (ascending for bids, descending for asks).
// Finding a price checks the last few entries one by one, because most activity is at or
// near the best price, and falls back to binary search. Adding or removing a level moves
// only the entries behind it, which is usually none or a few.
// The levels live in a pool and are addressed by index. Orders point at their level's
// pool slot, not at its position in the array, so moving array entries never breaks an
// order's link to its level.
// Orders live in a pool of 32-byte nodes linked into one FIFO list per level, and a
// FlatIdMap maps OrderId to node, exactly as in FastOrderBook.
// Both arrays reserve room for max_orders entries, the level pool holds max_orders
// levels, and a side can never have more levels than orders, so nothing ever grows.
//
// The constructor calls validate_config and throws std::invalid_argument if max_orders
// is above kMaxOrders (so the default, unlimited max_orders must be replaced by a real
// limit). The price band is optional. If the config sets one, it is enforced like in
// every other book.
//
// These invariants are true whenever no call is in progress, and check_invariants()
// verifies them.
//   1. The book is never crossed. If both sides are non-empty, best_bid() < best_ask().
//   2. Each side's array is strictly sorted from worst to best, every entry's level has at
//      least one order and the entry's price, and the level pool holds exactly one level
//      per entry.
//   3. Each level's list is well formed (head, tail, prev and next agree). Its count is
//      the list length, and its total_qty is the sum of the remaining quantities.
//   4. Every resting order has remaining >= 1, a multiple of lot_size, and records the
//      side and level it is linked into.
//   5. The ID index has exactly one entry per resting order, pointing at its node, and
//      the order pool's in_use() == order_count() <= max_orders.
//   6. Within a level, list order is priority order, because orders are appended only at
//      the tail.
//
// The book is not thread-safe. Events go synchronously to the listener, which must
// outlive the book.
class SortedOrderBook {
public:
    static constexpr std::uint32_t kMaxOrders = std::uint32_t{1} << 24;

    SortedOrderBook(OrderBookListener& listener, BookConfig config);

    void submit(const OrderRequest& request);
    void cancel(OrderId id);
    void modify(OrderId id, Price new_price, Qty new_qty);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] AggregateQty volume_at(Side side, Price price) const noexcept;
    [[nodiscard]] std::vector<LevelSummary> depth(Side side, std::size_t max_levels) const;
    [[nodiscard]] bool contains(OrderId id) const noexcept { return ids_.contains(id); }
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.in_use(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept { return sorted(side).size(); }
    [[nodiscard]] const BookConfig& config() const noexcept { return config_; }

    // Checks every invariant listed above in O(levels + orders) time. Meant for tests.
    [[nodiscard]] bool check_invariants() const;

    // Snapshot calls (see the OrderBook concept). for_each_resting visits one side's
    // orders, best price first and in priority order within a price, and never allocates.
    template <class Visit>
    void for_each_resting(Side side, Visit&& visit) const;

    // Puts one order at the back of its price's queue, with no event and no matching.
    // Returns why it refused, and then changes nothing. Never allocates.
    RestoreStatus restore_resting(const RestingOrder& order) noexcept;

    [[nodiscard]] MatchId next_match_id() const noexcept { return next_match_id_; }
    void restore_next_match_id(MatchId next) noexcept { next_match_id_ = next; }

private:
    using Index = IndexPool<int>::Index;
    static constexpr Index kNone = IndexPool<int>::kNone;

    struct Node {  // 32 bytes, so two orders fit in one cache line.
        OrderId id = 0;
        OwnerId owner = kNoOwner;
        Qty remaining = 0;
        Index prev = kNone;
        Index next = kNone;
        Index level = kNone;  // The level's slot in the level pool.
        Side side = Side::Buy;
        bool post_only = false;
    };

    struct Level {
        Price price = 0;
        AggregateQty total_qty = 0;
        Index head = kNone;  // Highest priority, so this order fills next.
        Index tail = kNone;
        std::uint32_t count = 0;
    };

    struct Entry {  // One element of a side's sorted array.
        Price price;
        Index level;
    };

    struct Aggressor {
        OrderId id;
        OwnerId owner;
        Side side;
        bool market;  // A market order crosses every price.
        Price limit;
        Qty remaining;
    };

    [[nodiscard]] std::vector<Entry>& sorted(Side side) noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }
    [[nodiscard]] const std::vector<Entry>& sorted(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    // Where `price` is or belongs in a side's array. Returns the position and whether the
    // entry at that position has exactly this price. If not, the position is where a new
    // entry would be inserted.
    [[nodiscard]] std::pair<std::size_t, bool> find(Side side, Price price) const noexcept;
    [[nodiscard]] static bool better(Side side, Price a, Price b) noexcept {
        return side == Side::Buy ? a > b : a < b;
    }
    [[nodiscard]] static bool crosses(const Aggressor& aggressor, Price price) noexcept;

    [[nodiscard]] bool on_tick(Price price) const noexcept;
    [[nodiscard]] bool in_band(Price price) const noexcept;
    [[nodiscard]] std::optional<RejectReason> validate(const OrderRequest& request) const noexcept;
    [[nodiscard]] bool would_cross(Side side, Price price) const noexcept;
    [[nodiscard]] bool can_fill_completely(const Aggressor& aggressor) const noexcept;

    void match(Aggressor& aggressor);
    void prevent_self_trade(Side resting_side, Index level, Aggressor& aggressor);
    void cancel_front(Side side, Index level, CancelReason reason);

    void rest(const Aggressor& aggressor, bool post_only);  // Requires a free node in the pool.
    // Finds or creates the order's level, takes a node, indexes the order and links it at
    // the back of the level, without any event. Returns the level's slot. Both rest() and
    // restore_resting() use it, so a restored order is stored exactly like one that came
    // to rest after matching. Requires a free node.
    Index place(const RestingOrder& order) noexcept;
    void link_back(Index level, Index node) noexcept;
    void unlink(Index node) noexcept;
    void remove(Index node) noexcept;  // Unlinks the node, forgets its ID and frees it.
    // Reports the level's new state, then retires the level if it is now empty. Every
    // change to a level ends with this call, so an empty level never outlives an operation.
    void emit_level_update(Side side, Index level);

    [[nodiscard]] bool check_side(Side side, std::size_t& orders_seen) const;

    OrderBookListener* listener_;  // Never null.
    BookConfig config_;
    std::vector<Entry> bids_;  // Worst (lowest) price first, best (highest) last.
    std::vector<Entry> asks_;  // Worst (highest) price first, best (lowest) last.
    IndexPool<Level> levels_;
    IndexPool<Node> orders_;
    FlatIdMap ids_;
    MatchId next_match_id_ = 1;
};

template <class Visit>
void SortedOrderBook::for_each_resting(Side side, Visit&& visit) const {
    // The best price is at the back of the array, so this walks it backwards.
    const std::vector<Entry>& entries = sorted(side);
    for (std::size_t i = entries.size(); i > 0; --i) {
        const Level& level = levels_[entries[i - 1].level];
        for (Index n = level.head; n != kNone; n = orders_[n].next) {
            const Node& node = orders_[n];
            visit(RestingOrder{.id = node.id,
                               .owner = node.owner,
                               .side = side,
                               .price = level.price,
                               .remaining = node.remaining,
                               .post_only = node.post_only});
        }
    }
}

static_assert(OrderBook<SortedOrderBook>);

}  // namespace litbook
