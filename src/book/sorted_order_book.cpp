#include "litbook/book/sorted_order_book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace litbook {

namespace {

constexpr bool is_self_trade(OwnerId a, OwnerId b) noexcept {
    return a != kNoOwner && a == b;
}

// How many entries at the best end of the array find() checks one by one before it
// switches to binary search. Most new orders land within a few levels of the best price.
constexpr std::size_t kLinearSteps = 8;

// Validates the config before any member allocates and returns max_orders.
std::uint32_t checked_capacity(const BookConfig& config) {
    validate_config(config);
    if (config.max_orders > SortedOrderBook::kMaxOrders) {
        throw std::invalid_argument("SortedOrderBook: max_orders is too large (or unlimited)");
    }
    return config.max_orders;
}

template <class T>
std::vector<T> reserved(std::size_t capacity) {
    std::vector<T> v;
    v.reserve(capacity);
    return v;
}

}  // namespace

SortedOrderBook::SortedOrderBook(OrderBookListener& listener, BookConfig config)
    : listener_(&listener),
      config_(config),
      bids_(reserved<Entry>(checked_capacity(config))),
      asks_(reserved<Entry>(config.max_orders)),
      levels_(config.max_orders),
      orders_(config.max_orders),
      ids_(config.max_orders) {}

// Finding prices in the sorted arrays.

std::pair<std::size_t, bool> SortedOrderBook::find(Side side, Price price) const noexcept {
    const std::vector<Entry>& v = sorted(side);
    // Entries are ordered worst to best, so the entries better than `price` form a block
    // at the back. Walk into that block from the back for a few steps first.
    std::size_t i = v.size();
    const std::size_t stop = v.size() > kLinearSteps ? v.size() - kLinearSteps : 0;
    while (i > stop && better(side, v[i - 1].price, price)) {
        --i;
    }
    if (i == stop && stop > 0 && better(side, v[i - 1].price, price)) {
        // Still inside the block after kLinearSteps, so binary search the rest. The first
        // entry better than `price` is where the block starts.
        const auto first_better =
            std::partition_point(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i),
                                 [&](const Entry& e) { return !better(side, e.price, price); });
        i = static_cast<std::size_t>(first_better - v.begin());
    }
    // Now v[i - 1] (if any) is the best entry that is not better than `price`.
    if (i > 0 && v[i - 1].price == price) {
        return {i - 1, true};
    }
    return {i, false};
}

bool SortedOrderBook::crosses(const Aggressor& aggressor, Price price) noexcept {
    if (aggressor.market) {
        return true;
    }
    return aggressor.side == Side::Buy ? price <= aggressor.limit : price >= aggressor.limit;
}

// Validation. These are the same checks, in the same order, as in MapOrderBook.

bool SortedOrderBook::on_tick(Price price) const noexcept {
    return price % config_.tick_size == 0;
}

bool SortedOrderBook::in_band(Price price) const noexcept {
    return price >= config_.min_price && price <= config_.max_price;
}

std::optional<RejectReason> SortedOrderBook::validate(const OrderRequest& request) const noexcept {
    const bool limit = request.type == OrderType::Limit;
    if (request.qty == 0) {
        return RejectReason::ZeroQuantity;
    }
    if (request.post_only && (!limit || request.tif != TimeInForce::GoodTillCancel)) {
        return RejectReason::InvalidOrderType;
    }
    if (limit && !on_tick(request.price)) {
        return RejectReason::InvalidPrice;
    }
    if (limit && !in_band(request.price)) {
        return RejectReason::PriceOutOfRange;
    }
    if (request.qty % config_.lot_size != 0) {
        return RejectReason::InvalidLotSize;
    }
    if (ids_.contains(request.id)) {
        return RejectReason::DuplicateOrderId;
    }
    if (request.post_only && would_cross(request.side, request.price)) {
        return RejectReason::PostOnlyWouldCross;
    }
    return std::nullopt;
}

bool SortedOrderBook::would_cross(Side side, Price price) const noexcept {
    const std::vector<Entry>& opposite_side = sorted(opposite(side));
    if (opposite_side.empty()) {
        return false;
    }
    const Price best = opposite_side.back().price;
    return side == Side::Buy ? best <= price : best >= price;
}

// The FOK rule is the same as in MapOrderBook. Enough quantity must be reachable, in
// priority order, before any same-owner order.
bool SortedOrderBook::can_fill_completely(const Aggressor& aggressor) const noexcept {
    const std::vector<Entry>& resting = sorted(opposite(aggressor.side));
    AggregateQty reachable = 0;
    for (std::size_t i = resting.size(); i > 0 && crosses(aggressor, resting[i - 1].price); --i) {
        for (Index n = levels_[resting[i - 1].level].head; n != kNone; n = orders_[n].next) {
            const Node& order = orders_[n];
            if (is_self_trade(aggressor.owner, order.owner)) {
                return false;
            }
            reachable += order.remaining;
            if (reachable >= aggressor.remaining) {
                return true;
            }
        }
    }
    return false;
}

// Public operations.

void SortedOrderBook::submit(const OrderRequest& request) {
    if (const auto reason = validate(request)) {
        listener_->on_rejected({.order_id = request.id, .reason = *reason});
        return;
    }

    const bool is_market = request.type == OrderType::Market;
    listener_->on_accepted({.order_id = request.id,
                            .owner = request.owner,
                            .side = request.side,
                            .type = request.type,
                            .tif = request.tif,
                            .post_only = request.post_only,
                            .price = is_market ? 0 : request.price,
                            .qty = request.qty});

    Aggressor aggressor{.id = request.id,
                        .owner = request.owner,
                        .side = request.side,
                        .market = is_market,
                        .limit = request.price,
                        .remaining = request.qty};

    if (request.tif == TimeInForce::FillOrKill && !can_fill_completely(aggressor)) {
        listener_->on_canceled({.order_id = aggressor.id,
                                .canceled_qty = aggressor.remaining,
                                .remaining_qty = 0,
                                .reason = CancelReason::Unfilled});
        return;
    }

    match(aggressor);

    if (aggressor.remaining == 0) {
        return;
    }
    const bool may_rest = !is_market && request.tif == TimeInForce::GoodTillCancel;
    if (may_rest && !orders_.full()) {
        rest(aggressor, request.post_only);
    } else {
        listener_->on_canceled(
            {.order_id = aggressor.id,
             .canceled_qty = aggressor.remaining,
             .remaining_qty = 0,
             .reason = may_rest ? CancelReason::BookFull : CancelReason::Unfilled});
    }
}

void SortedOrderBook::cancel(OrderId id) {
    const Index n = ids_.find(id);
    if (n == FlatIdMap::kNotFound) {
        listener_->on_cancel_rejected({.order_id = id, .reason = RejectReason::UnknownOrderId});
        return;
    }
    const Node order = orders_[n];  // A copy, because the node is freed below.
    remove(n);
    listener_->on_canceled({.order_id = id,
                            .canceled_qty = order.remaining,
                            .remaining_qty = 0,
                            .reason = CancelReason::UserRequested});
    emit_level_update(order.side, order.level);
}

void SortedOrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
    const Index n = ids_.find(id);
    const auto reject = [&](RejectReason reason) {
        listener_->on_modify_rejected({.order_id = id, .reason = reason});
    };
    if (n == FlatIdMap::kNotFound) {
        return reject(RejectReason::UnknownOrderId);
    }
    if (new_qty == 0) {
        return reject(RejectReason::ZeroQuantity);
    }
    if (!on_tick(new_price)) {
        return reject(RejectReason::InvalidPrice);
    }
    if (!in_band(new_price)) {
        return reject(RejectReason::PriceOutOfRange);
    }
    if (new_qty % config_.lot_size != 0) {
        return reject(RejectReason::InvalidLotSize);
    }
    const Node order = orders_[n];  // A copy, because the node may be freed below.
    if (order.post_only && would_cross(order.side, new_price)) {
        return reject(RejectReason::PostOnlyWouldCross);
    }

    const bool keeps_priority =
        new_price == levels_[order.level].price && new_qty <= order.remaining;
    if (keeps_priority) {
        levels_[order.level].total_qty -= order.remaining - new_qty;
        orders_[n].remaining = new_qty;
    } else {
        remove(n);  // It loses its place and is re-entered below at the new terms.
    }
    listener_->on_modified({.order_id = id,
                            .side = order.side,
                            .price = new_price,
                            .qty = new_qty,
                            .kept_priority = keeps_priority});
    emit_level_update(order.side, order.level);
    if (keeps_priority) {
        return;
    }

    // The node was freed above, so rest() below always finds a free one.
    Aggressor aggressor{.id = id,
                        .owner = order.owner,
                        .side = order.side,
                        .market = false,
                        .limit = new_price,
                        .remaining = new_qty};
    match(aggressor);
    if (aggressor.remaining > 0) {
        rest(aggressor, order.post_only);
    }
}

// Matching.

void SortedOrderBook::match(Aggressor& aggressor) {
    const Side resting_side = opposite(aggressor.side);
    const std::vector<Entry>& resting = sorted(resting_side);

    while (aggressor.remaining > 0 && !resting.empty() &&
           crosses(aggressor, resting.back().price)) {
        const Index index = resting.back().level;
        Level& level = levels_[index];
        const Price price = level.price;

        // When the level empties, emit_level_update() retires it and the next best level
        // becomes resting.back(). count is still readable then, and is 0.
        while (aggressor.remaining > 0 && level.count > 0) {
            const Index n = level.head;
            Node& order = orders_[n];
            if (is_self_trade(aggressor.owner, order.owner)) {
                prevent_self_trade(resting_side, index, aggressor);
                continue;
            }

            const Qty fill = std::min(aggressor.remaining, order.remaining);
            aggressor.remaining -= fill;
            order.remaining -= fill;
            level.total_qty -= fill;

            listener_->on_trade({.match_id = next_match_id_++,
                                 .aggressor_id = aggressor.id,
                                 .resting_id = order.id,
                                 .aggressor_side = aggressor.side,
                                 .price = price,
                                 .qty = fill,
                                 .aggressor_remaining = aggressor.remaining,
                                 .resting_remaining = order.remaining});
            if (order.remaining == 0) {
                remove(n);
            }
            emit_level_update(resting_side, index);
        }
    }
}

// Called when the front order of `level` has the aggressor's owner.
void SortedOrderBook::prevent_self_trade(Side resting_side, Index level, Aggressor& aggressor) {
    const auto cancel_aggressor = [&](Qty qty) {
        aggressor.remaining -= qty;
        listener_->on_canceled({.order_id = aggressor.id,
                                .canceled_qty = qty,
                                .remaining_qty = aggressor.remaining,
                                .reason = CancelReason::SelfTradePrevention});
    };

    switch (config_.self_trade_policy) {
        case SelfTradePolicy::CancelNewest:
            cancel_aggressor(aggressor.remaining);
            return;
        case SelfTradePolicy::CancelOldest:
            cancel_front(resting_side, level, CancelReason::SelfTradePrevention);
            return;
        case SelfTradePolicy::CancelBoth:
            cancel_front(resting_side, level, CancelReason::SelfTradePrevention);
            cancel_aggressor(aggressor.remaining);
            return;
        case SelfTradePolicy::Decrement: {
            Level& l = levels_[level];
            const Index n = l.head;
            Node& order = orders_[n];
            const Qty reduce = std::min(aggressor.remaining, order.remaining);
            order.remaining -= reduce;
            l.total_qty -= reduce;
            const OrderId resting_id = order.id;
            const Qty resting_left = order.remaining;
            if (resting_left == 0) {
                remove(n);
            }
            listener_->on_canceled({.order_id = resting_id,
                                    .canceled_qty = reduce,
                                    .remaining_qty = resting_left,
                                    .reason = CancelReason::SelfTradePrevention});
            emit_level_update(resting_side, level);
            cancel_aggressor(reduce);
            return;
        }
    }
}

void SortedOrderBook::cancel_front(Side side, Index level, CancelReason reason) {
    const Index n = levels_[level].head;
    const Node order = orders_[n];
    remove(n);
    listener_->on_canceled({.order_id = order.id,
                            .canceled_qty = order.remaining,
                            .remaining_qty = 0,
                            .reason = reason});
    emit_level_update(side, level);
}

// Resting orders, levels and the intrusive lists.

void SortedOrderBook::rest(const Aggressor& aggressor, bool post_only) {
    const Index level = place({.id = aggressor.id,
                               .owner = aggressor.owner,
                               .side = aggressor.side,
                               .price = aggressor.limit,
                               .remaining = aggressor.remaining,
                               .post_only = post_only});
    emit_level_update(aggressor.side, level);
}

SortedOrderBook::Index SortedOrderBook::place(const RestingOrder& order) noexcept {
    std::vector<Entry>& side = sorted(order.side);
    const auto [pos, found] = find(order.side, order.price);
    Index level = kNone;
    if (found) {
        level = side[pos].level;
    } else {
        // Never kNone. Every level holds at least one order, so with a free order node
        // there is also a free level slot.
        level = levels_.acquire();
        levels_[level] = Level{.price = order.price};
        // Never reallocates, because capacity is max_orders and size < max_orders here.
        side.insert(side.begin() + static_cast<std::ptrdiff_t>(pos),
                    Entry{.price = order.price, .level = level});
    }

    const Index n = orders_.acquire();  // Never kNone, because callers guarantee a free node.
    orders_[n] = Node{.id = order.id,
                      .owner = order.owner,
                      .remaining = order.remaining,
                      .prev = kNone,
                      .next = kNone,
                      .level = level,
                      .side = order.side,
                      .post_only = order.post_only};
    ids_.insert(order.id, n);  // Cannot fail, since the ID was checked and capacity matches.
    link_back(level, n);
    return level;
}

// The checks run in the same order in every book, so they all refuse a bad order for
// the same reason.
RestoreStatus SortedOrderBook::restore_resting(const RestingOrder& order) noexcept {
    if (!on_tick(order.price) || !in_band(order.price)) {
        return RestoreStatus::BadPrice;
    }
    if (order.remaining == 0 || order.remaining % config_.lot_size != 0) {
        return RestoreStatus::BadQuantity;
    }
    if (ids_.contains(order.id)) {
        return RestoreStatus::DuplicateId;
    }
    if (would_cross(order.side, order.price)) {
        return RestoreStatus::WouldCross;
    }
    if (orders_.full()) {
        return RestoreStatus::BookFull;
    }
    place(order);
    return RestoreStatus::Ok;
}

void SortedOrderBook::link_back(Index level, Index n) noexcept {
    Level& l = levels_[level];
    Node& node = orders_[n];
    node.prev = l.tail;
    node.next = kNone;
    if (l.tail == kNone) {
        l.head = n;
    } else {
        orders_[l.tail].next = n;
    }
    l.tail = n;
    l.total_qty += node.remaining;
    ++l.count;
}

void SortedOrderBook::unlink(Index n) noexcept {
    const Node& node = orders_[n];
    Level& l = levels_[node.level];
    (node.prev == kNone ? l.head : orders_[node.prev].next) = node.next;
    (node.next == kNone ? l.tail : orders_[node.next].prev) = node.prev;
    l.total_qty -= node.remaining;
    --l.count;
}

void SortedOrderBook::remove(Index n) noexcept {
    unlink(n);
    ids_.erase(orders_[n].id);
    orders_.release(n);
}

void SortedOrderBook::emit_level_update(Side side, Index level) {
    const Level& l = levels_[level];
    listener_->on_level_update(
        {.side = side, .price = l.price, .total_qty = l.total_qty, .order_count = l.count});
    if (l.count > 0) {
        return;
    }
    // The level is empty, so take it out of the array and give its slot back. It is
    // usually the best level, which is the last entry, so nothing has to move.
    std::vector<Entry>& v = sorted(side);
    const auto [pos, found] = find(side, l.price);
    if (found) {
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(pos));
        levels_.release(level);
    }
}

// Queries.

std::optional<Price> SortedOrderBook::best_bid() const noexcept {
    return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.back().price);
}

std::optional<Price> SortedOrderBook::best_ask() const noexcept {
    return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.back().price);
}

AggregateQty SortedOrderBook::volume_at(Side side, Price price) const noexcept {
    const auto [pos, found] = find(side, price);
    return found ? levels_[sorted(side)[pos].level].total_qty : 0;
}

std::vector<LevelSummary> SortedOrderBook::depth(Side side, std::size_t max_levels) const {
    std::vector<LevelSummary> out;
    const std::vector<Entry>& v = sorted(side);
    for (std::size_t i = v.size(); i > 0 && out.size() < max_levels; --i) {
        const Level& l = levels_[v[i - 1].level];
        out.push_back({.price = l.price, .total_qty = l.total_qty, .order_count = l.count});
    }
    return out;
}

// Invariant checks.

bool SortedOrderBook::check_side(Side side, std::size_t& orders_seen) const {
    const std::vector<Entry>& v = sorted(side);
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0 && !better(side, v[i].price, v[i - 1].price)) {
            return false;  // Breaks invariant 2, the array is not strictly sorted.
        }
        if (v[i].level >= levels_.capacity()) {
            return false;  // A bad level index.
        }
        const Level& l = levels_[v[i].level];
        if (l.count == 0 || l.price != v[i].price) {
            return false;  // Breaks invariant 2.
        }
        AggregateQty sum = 0;
        std::uint32_t length = 0;
        Index prev = kNone;
        for (Index n = l.head; n != kNone; n = orders_[n].next) {
            if (n >= orders_.capacity() || length > orders_.capacity()) {
                return false;  // A bad index or a cycle.
            }
            const Node& node = orders_[n];
            if (node.prev != prev || node.remaining == 0 ||
                node.remaining % config_.lot_size != 0 || node.side != side ||
                node.level != v[i].level || ids_.find(node.id) != n) {
                return false;  // Breaks invariant 3, 4 or 5.
            }
            sum += node.remaining;
            ++length;
            prev = n;
        }
        if (prev != l.tail || length != l.count || sum != l.total_qty) {
            return false;  // Breaks invariant 3.
        }
        orders_seen += length;
    }
    return true;
}

bool SortedOrderBook::check_invariants() const {
    if (!bids_.empty() && !asks_.empty() && bids_.back().price >= asks_.back().price) {
        return false;  // Breaks invariant 1.
    }
    if (levels_.in_use() != bids_.size() + asks_.size()) {
        return false;  // Breaks invariant 2, a level slot is leaked or shared.
    }
    std::size_t orders_seen = 0;
    if (!check_side(Side::Buy, orders_seen) || !check_side(Side::Sell, orders_seen)) {
        return false;
    }
    return orders_seen == orders_.in_use() && orders_seen == ids_.size() &&
           orders_seen <= config_.max_orders;  // Invariant 5.
}

}  // namespace litbook
