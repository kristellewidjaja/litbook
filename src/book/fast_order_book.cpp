#include "litbook/book/fast_order_book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace litbook {

namespace {

constexpr bool is_self_trade(OwnerId a, OwnerId b) noexcept {
    return a != kNoOwner && a == b;
}

// Validates everything FastOrderBook needs and returns the ladder size, before any
// member allocates. Differences are computed in unsigned arithmetic, which cannot
// overflow even across the full int64 range.
std::size_t checked_levels(const BookConfig& config) {
    validate_config(config);
    if (config.min_price % config.tick_size != 0 || config.max_price % config.tick_size != 0) {
        throw std::invalid_argument("FastOrderBook: min_price and max_price must be on tick");
    }
    const std::uint64_t span =
        static_cast<std::uint64_t>(config.max_price) - static_cast<std::uint64_t>(config.min_price);
    const std::uint64_t ticks = span / static_cast<std::uint64_t>(config.tick_size);
    if (ticks >= FastOrderBook::kMaxLevels) {
        throw std::invalid_argument("FastOrderBook: price band is too wide (or unbounded)");
    }
    if (config.max_orders > FastOrderBook::kMaxOrders) {
        throw std::invalid_argument("FastOrderBook: max_orders is too large");
    }
    return static_cast<std::size_t>(ticks) + 1;
}

}  // namespace

FastOrderBook::FastOrderBook(OrderBookListener& listener, BookConfig config)
    : listener_(&listener),
      config_(config),
      levels_(checked_levels(config)),
      bids_(levels_),
      asks_(levels_),
      pool_(config.max_orders),
      ids_(config.max_orders) {}

// Converting between prices and ladder indexes.

bool FastOrderBook::on_tick(Price price) const noexcept {
    return price % config_.tick_size == 0;
}

bool FastOrderBook::in_band(Price price) const noexcept {
    return price >= config_.min_price && price <= config_.max_price;
}

std::size_t FastOrderBook::to_index(Price price) const noexcept {
    const std::uint64_t offset =
        static_cast<std::uint64_t>(price) - static_cast<std::uint64_t>(config_.min_price);
    return static_cast<std::size_t>(offset / static_cast<std::uint64_t>(config_.tick_size));
}

Price FastOrderBook::to_price(std::size_t index) const noexcept {
    // Unsigned wraparound is well defined, and the true result is in [min_price, max_price].
    return static_cast<Price>(static_cast<std::uint64_t>(config_.min_price) +
                              index * static_cast<std::uint64_t>(config_.tick_size));
}

std::size_t FastOrderBook::next_worse(const Ladder& ladder, Side side, std::size_t index) noexcept {
    if (side == Side::Buy) {
        return index == 0 ? kNoLevel : ladder.occupied.find_prev(index - 1);
    }
    return ladder.occupied.find_next(index + 1);
}

bool FastOrderBook::crosses(const Aggressor& aggressor, std::size_t index) noexcept {
    return aggressor.side == Side::Buy ? index <= aggressor.limit : index >= aggressor.limit;
}

// Validation. These are the same checks, in the same order, as in MapOrderBook.

std::optional<RejectReason> FastOrderBook::validate(const OrderRequest& request) const noexcept {
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

bool FastOrderBook::would_cross(Side side, Price price) const noexcept {
    const Ladder& opposite_side = ladder(opposite(side));
    if (opposite_side.best == kNoLevel) {
        return false;
    }
    const Price best = to_price(opposite_side.best);
    return side == Side::Buy ? best <= price : best >= price;
}

// The FOK rule is the same as in MapOrderBook. Enough quantity must be reachable, in
// priority order, before any same-owner order.
bool FastOrderBook::can_fill_completely(const Aggressor& aggressor) const noexcept {
    const Side resting_side = opposite(aggressor.side);
    const Ladder& resting = ladder(resting_side);
    AggregateQty reachable = 0;
    for (std::size_t i = resting.best; i != kNoLevel && crosses(aggressor, i);
         i = next_worse(resting, resting_side, i)) {
        for (Index n = resting.levels[i].head; n != kNone; n = pool_[n].next) {
            const Node& order = pool_[n];
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

void FastOrderBook::submit(const OrderRequest& request) {
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

    const std::size_t market_limit = request.side == Side::Buy ? levels_ - 1 : 0;
    Aggressor aggressor{.id = request.id,
                        .owner = request.owner,
                        .side = request.side,
                        .limit = is_market ? market_limit : to_index(request.price),
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
    if (may_rest && !pool_.full()) {
        rest(aggressor, request.post_only);
    } else {
        listener_->on_canceled(
            {.order_id = aggressor.id,
             .canceled_qty = aggressor.remaining,
             .remaining_qty = 0,
             .reason = may_rest ? CancelReason::BookFull : CancelReason::Unfilled});
    }
}

void FastOrderBook::cancel(OrderId id) {
    const Index n = ids_.find(id);
    if (n == FlatIdMap::kNotFound) {
        listener_->on_cancel_rejected({.order_id = id, .reason = RejectReason::UnknownOrderId});
        return;
    }
    const Node order = pool_[n];  // A copy, because the node is freed below.
    remove(n);
    listener_->on_canceled({.order_id = id,
                            .canceled_qty = order.remaining,
                            .remaining_qty = 0,
                            .reason = CancelReason::UserRequested});
    emit_level_update(order.side, order.level);
}

void FastOrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
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
    const Node order = pool_[n];  // A copy, because the node may be freed below.
    if (order.post_only && would_cross(order.side, new_price)) {
        return reject(RejectReason::PostOnlyWouldCross);
    }

    const std::size_t new_index = to_index(new_price);
    const bool keeps_priority = new_index == order.level && new_qty <= order.remaining;
    if (keeps_priority) {
        ladder(order.side).levels[order.level].total_qty -= order.remaining - new_qty;
        pool_[n].remaining = new_qty;
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
                        .limit = new_index,
                        .remaining = new_qty};
    match(aggressor);
    if (aggressor.remaining > 0) {
        rest(aggressor, order.post_only);
    }
}

// Matching.

void FastOrderBook::match(Aggressor& aggressor) {
    const Side resting_side = opposite(aggressor.side);
    Ladder& resting = ladder(resting_side);

    while (aggressor.remaining > 0 && resting.best != kNoLevel &&
           crosses(aggressor, resting.best)) {
        const std::size_t index = resting.best;
        const Price price = to_price(index);
        Level& level = resting.levels[index];

        // When the level empties, unlink() advances resting.best and count drops to 0.
        while (aggressor.remaining > 0 && level.count > 0) {
            const Index n = level.head;
            Node& order = pool_[n];
            if (is_self_trade(aggressor.owner, order.owner)) {
                prevent_self_trade(resting, resting_side, index, aggressor);
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

// Called when the front order at `index` has the aggressor's owner.
void FastOrderBook::prevent_self_trade(Ladder& resting, Side resting_side, std::size_t index,
                                       Aggressor& aggressor) {
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
            cancel_front(resting, resting_side, index, CancelReason::SelfTradePrevention);
            return;
        case SelfTradePolicy::CancelBoth:
            cancel_front(resting, resting_side, index, CancelReason::SelfTradePrevention);
            cancel_aggressor(aggressor.remaining);
            return;
        case SelfTradePolicy::Decrement: {
            Level& level = resting.levels[index];
            const Index n = level.head;
            Node& order = pool_[n];
            const Qty reduce = std::min(aggressor.remaining, order.remaining);
            order.remaining -= reduce;
            level.total_qty -= reduce;
            const OrderId resting_id = order.id;
            const Qty resting_left = order.remaining;
            if (resting_left == 0) {
                remove(n);
            }
            listener_->on_canceled({.order_id = resting_id,
                                    .canceled_qty = reduce,
                                    .remaining_qty = resting_left,
                                    .reason = CancelReason::SelfTradePrevention});
            emit_level_update(resting_side, index);
            cancel_aggressor(reduce);
            return;
        }
    }
}

void FastOrderBook::cancel_front(Ladder& resting, Side side, std::size_t index,
                                 CancelReason reason) {
    const Index n = resting.levels[index].head;
    const Node order = pool_[n];
    remove(n);
    listener_->on_canceled({.order_id = order.id,
                            .canceled_qty = order.remaining,
                            .remaining_qty = 0,
                            .reason = reason});
    emit_level_update(side, index);
}

// Resting orders and the intrusive lists.

void FastOrderBook::rest(const Aggressor& aggressor, bool post_only) {
    const Index n = pool_.acquire();  // Never kNone, because callers guarantee a free node.
    pool_[n] = Node{.id = aggressor.id,
                    .owner = aggressor.owner,
                    .remaining = aggressor.remaining,
                    .prev = kNone,
                    .next = kNone,
                    .level = static_cast<std::uint32_t>(aggressor.limit),
                    .side = aggressor.side,
                    .post_only = post_only};
    ids_.insert(aggressor.id, n);  // Cannot fail, since the ID was checked and capacity matches.
    link_back(ladder(aggressor.side), aggressor.limit, n);
    emit_level_update(aggressor.side, aggressor.limit);
}

void FastOrderBook::link_back(Ladder& lad, std::size_t index, Index n) noexcept {
    Level& level = lad.levels[index];
    Node& node = pool_[n];
    node.prev = level.tail;
    node.next = kNone;
    if (level.tail == kNone) {
        level.head = n;
    } else {
        pool_[level.tail].next = n;
    }
    level.tail = n;
    level.total_qty += node.remaining;
    if (++level.count == 1) {
        lad.occupied.set(index);
        ++lad.level_count;
        // For bids the best is the highest index, for asks the lowest (kNoLevel is the largest).
        if (node.side == Side::Buy ? (lad.best == kNoLevel || index > lad.best)
                                   : index < lad.best) {
            lad.best = index;
        }
    }
}

void FastOrderBook::unlink(Ladder& lad, Index n) noexcept {
    const Node& node = pool_[n];
    const std::size_t index = node.level;
    Level& level = lad.levels[index];
    (node.prev == kNone ? level.head : pool_[node.prev].next) = node.next;
    (node.next == kNone ? level.tail : pool_[node.next].prev) = node.prev;
    level.total_qty -= node.remaining;
    if (--level.count == 0) {
        lad.occupied.reset(index);
        --lad.level_count;
        if (lad.best == index) {
            lad.best = next_worse(lad, node.side, index);
        }
    }
}

void FastOrderBook::remove(Index n) noexcept {
    const Node& node = pool_[n];
    unlink(ladder(node.side), n);
    ids_.erase(node.id);
    pool_.release(n);
}

void FastOrderBook::emit_level_update(Side side, std::size_t index) {
    const Level& level = ladder(side).levels[index];
    listener_->on_level_update({.side = side,
                                .price = to_price(index),
                                .total_qty = level.total_qty,
                                .order_count = level.count});
}

// Queries.

std::optional<Price> FastOrderBook::best_bid() const noexcept {
    return bids_.best == kNoLevel ? std::nullopt : std::optional<Price>(to_price(bids_.best));
}

std::optional<Price> FastOrderBook::best_ask() const noexcept {
    return asks_.best == kNoLevel ? std::nullopt : std::optional<Price>(to_price(asks_.best));
}

AggregateQty FastOrderBook::volume_at(Side side, Price price) const noexcept {
    if (!in_band(price) || !on_tick(price)) {
        return 0;
    }
    return ladder(side).levels[to_index(price)].total_qty;
}

std::vector<LevelSummary> FastOrderBook::depth(Side side, std::size_t max_levels) const {
    std::vector<LevelSummary> out;
    const Ladder& lad = ladder(side);
    for (std::size_t i = lad.best; i != kNoLevel && out.size() < max_levels;
         i = next_worse(lad, side, i)) {
        out.push_back({.price = to_price(i),
                       .total_qty = lad.levels[i].total_qty,
                       .order_count = lad.levels[i].count});
    }
    return out;
}

std::size_t FastOrderBook::level_count(Side side) const noexcept {
    return ladder(side).level_count;
}

// Invariant checks.

bool FastOrderBook::check_side(Side side, std::size_t& orders_seen) const {
    const Ladder& lad = ladder(side);
    std::size_t occupied_levels = 0;
    std::size_t best = kNoLevel;
    for (std::size_t i = 0; i < levels_; ++i) {
        const Level& level = lad.levels[i];
        if ((level.count > 0) != lad.occupied.test(i)) {
            return false;  // Breaks invariant 2.
        }
        if (level.count == 0) {
            if (level.head != kNone || level.tail != kNone || level.total_qty != 0) {
                return false;  // Breaks invariant 3.
            }
            continue;
        }
        ++occupied_levels;
        if (side == Side::Buy || best == kNoLevel) {
            best = i;  // Scanning up, bids keep the last (highest), asks the first.
        }

        AggregateQty sum = 0;
        std::uint32_t length = 0;
        Index prev = kNone;
        for (Index n = level.head; n != kNone; n = pool_[n].next) {
            if (n >= pool_.capacity() || length > pool_.capacity()) {
                return false;  // A bad index or a cycle.
            }
            const Node& node = pool_[n];
            if (node.prev != prev || node.remaining == 0 ||
                node.remaining % config_.lot_size != 0 || node.side != side || node.level != i ||
                ids_.find(node.id) != n) {
                return false;  // Breaks invariant 3, 4 or 5.
            }
            sum += node.remaining;
            ++length;
            prev = n;
        }
        if (prev != level.tail || length != level.count || sum != level.total_qty) {
            return false;  // Breaks invariant 3.
        }
        orders_seen += length;
    }
    return occupied_levels == lad.level_count && best == lad.best;  // Invariant 2.
}

bool FastOrderBook::check_invariants() const {
    if (bids_.best != kNoLevel && asks_.best != kNoLevel && bids_.best >= asks_.best) {
        return false;  // Breaks invariant 1.
    }
    std::size_t orders_seen = 0;
    if (!check_side(Side::Buy, orders_seen) || !check_side(Side::Sell, orders_seen)) {
        return false;
    }
    return orders_seen == pool_.in_use() && orders_seen == ids_.size() &&
           orders_seen <= config_.max_orders;  // Invariant 5.
}

}  // namespace litbook
