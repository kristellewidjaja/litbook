#include "litbook/book/map_order_book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace litbook {

namespace {

// Does an aggressor on `aggressor_side` with limit `limit` trade against a resting
// level at `level_price`?
constexpr bool crosses(Side aggressor_side, Price limit, Price level_price) noexcept {
    return aggressor_side == Side::Buy ? level_price <= limit : level_price >= limit;
}

// A limit that crosses every possible price. This is how a market order matches.
constexpr Price market_limit(Side side) noexcept {
    return side == Side::Buy ? std::numeric_limits<Price>::max()
                             : std::numeric_limits<Price>::min();
}

constexpr bool is_self_trade(OwnerId a, OwnerId b) noexcept {
    return a != kNoOwner && a == b;
}

}  // namespace

MapOrderBook::MapOrderBook(OrderBookListener& listener, BookConfig config)
    : listener_(&listener), config_(config) {
    validate_config(config_);
}

template <class F>
decltype(auto) MapOrderBook::with_levels(Side side, F&& f) {
    return side == Side::Buy ? std::forward<F>(f)(bids_) : std::forward<F>(f)(asks_);
}

template <class F>
decltype(auto) MapOrderBook::with_levels(Side side, F&& f) const {
    return side == Side::Buy ? std::forward<F>(f)(bids_) : std::forward<F>(f)(asks_);
}

// Validation.

bool MapOrderBook::valid_price(Price price) const noexcept {
    return price % config_.tick_size == 0;  // This is also correct for negative prices.
}

bool MapOrderBook::in_band(Price price) const noexcept {
    return price >= config_.min_price && price <= config_.max_price;
}

bool MapOrderBook::valid_qty(Qty qty) const noexcept {
    return qty % config_.lot_size == 0;
}

bool MapOrderBook::would_cross(Side side, Price price) const noexcept {
    if (side == Side::Buy) {
        return !asks_.empty() && asks_.begin()->first <= price;
    }
    return !bids_.empty() && bids_.begin()->first >= price;
}

// Checks run in a fixed order so every book reports the same reason for the same request.
std::optional<RejectReason> MapOrderBook::validate(const OrderRequest& request) const {
    if (request.qty == 0) {
        return RejectReason::ZeroQuantity;
    }
    if (request.post_only &&
        (request.type == OrderType::Market || request.tif != TimeInForce::GoodTillCancel)) {
        return RejectReason::InvalidOrderType;
    }
    if (request.type == OrderType::Limit && !valid_price(request.price)) {
        return RejectReason::InvalidPrice;
    }
    if (request.type == OrderType::Limit && !in_band(request.price)) {
        return RejectReason::PriceOutOfRange;
    }
    if (!valid_qty(request.qty)) {
        return RejectReason::InvalidLotSize;
    }
    if (index_.contains(request.id)) {
        return RejectReason::DuplicateOrderId;
    }
    if (request.post_only && would_cross(request.side, request.price)) {
        return RejectReason::PostOnlyWouldCross;
    }
    return std::nullopt;
}

// To apply the FOK rule, walk the resting orders this aggressor would reach, in priority
// order. It can fill only if enough quantity is reached *before* any same-owner order, so
// a FOK order never partially executes and never triggers self-trade prevention.
bool MapOrderBook::can_fill_completely(const Aggressor& aggressor) const {
    return with_levels(opposite(aggressor.side), [&](const auto& levels) {
        AggregateQty reachable = 0;
        for (const auto& [price, level] : levels) {
            if (!crosses(aggressor.side, aggressor.limit, price)) {
                return false;
            }
            for (const QueuedOrder& order : level.orders) {
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
    });
}

// Public operations.

void MapOrderBook::submit(const OrderRequest& request) {
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
                        .limit = is_market ? market_limit(request.side) : request.price,
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
    if (may_rest && index_.size() < config_.max_orders) {
        rest(aggressor, request.post_only);
    } else {
        listener_->on_canceled(
            {.order_id = aggressor.id,
             .canceled_qty = aggressor.remaining,
             .remaining_qty = 0,
             .reason = may_rest ? CancelReason::BookFull : CancelReason::Unfilled});
    }
}

void MapOrderBook::cancel(OrderId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) {
        listener_->on_cancel_rejected({.order_id = id, .reason = RejectReason::UnknownOrderId});
        return;
    }
    const Location location = found->second;
    index_.erase(found);

    with_levels(location.side, [&](auto& levels) {
        const auto level_it = levels.find(location.price);
        Level& level = level_it->second;
        const Qty canceled = location.order->remaining;
        level.total_qty -= canceled;
        level.orders.erase(location.order);

        listener_->on_canceled({.order_id = id,
                                .canceled_qty = canceled,
                                .remaining_qty = 0,
                                .reason = CancelReason::UserRequested});
        emit_level_update(location.side, location.price, level);
        if (level.orders.empty()) {
            levels.erase(level_it);
        }
    });
}

void MapOrderBook::modify(OrderId id, Price new_price, Qty new_qty) {
    const auto found = index_.find(id);
    const auto reject = [&](RejectReason reason) {
        listener_->on_modify_rejected({.order_id = id, .reason = reason});
    };
    if (found == index_.end()) {
        return reject(RejectReason::UnknownOrderId);
    }
    if (new_qty == 0) {
        return reject(RejectReason::ZeroQuantity);
    }
    if (!valid_price(new_price)) {
        return reject(RejectReason::InvalidPrice);
    }
    if (!in_band(new_price)) {
        return reject(RejectReason::PriceOutOfRange);
    }
    if (!valid_qty(new_qty)) {
        return reject(RejectReason::InvalidLotSize);
    }
    const Location location = found->second;
    const QueuedOrder order = *location.order;  // A copy, since the node may be erased below.
    if (order.post_only && would_cross(location.side, new_price)) {
        return reject(RejectReason::PostOnlyWouldCross);
    }

    const bool keeps_priority = new_price == location.price && new_qty <= order.remaining;

    with_levels(location.side, [&](auto& levels) {
        const auto level_it = levels.find(location.price);
        Level& level = level_it->second;

        if (keeps_priority) {
            // Same price and not larger, so shrink it in place and keep its queue position.
            level.total_qty -= order.remaining - new_qty;
            location.order->remaining = new_qty;
        } else {
            // Take it out, so it loses its place. It is re-entered below at the new terms.
            level.total_qty -= order.remaining;
            level.orders.erase(location.order);
            index_.erase(found);
        }
        listener_->on_modified({.order_id = id,
                                .side = location.side,
                                .price = new_price,
                                .qty = new_qty,
                                .kept_priority = keeps_priority});
        emit_level_update(location.side, location.price, level);
        if (level.orders.empty()) {
            levels.erase(level_it);
        }
    });

    if (keeps_priority) {
        return;
    }
    // The order left the book above, so its slot is free. Re-resting cannot exceed
    // max_orders and needs no capacity check.
    Aggressor aggressor{.id = id,
                        .owner = order.owner,
                        .side = location.side,
                        .limit = new_price,
                        .remaining = new_qty};
    match(aggressor);
    if (aggressor.remaining > 0) {
        rest(aggressor, order.post_only);
    }
}

// Matching.

void MapOrderBook::match(Aggressor& aggressor) {
    if (aggressor.side == Side::Buy) {
        match_against(asks_, aggressor);
    } else {
        match_against(bids_, aggressor);
    }
}

template <class Levels>
void MapOrderBook::match_against(Levels& levels, Aggressor& aggressor) {
    const Side resting_side = opposite(aggressor.side);

    while (aggressor.remaining > 0 && !levels.empty()) {
        const auto level_it = levels.begin();  // The best opposite price.
        const Price level_price = level_it->first;
        if (!crosses(aggressor.side, aggressor.limit, level_price)) {
            break;
        }

        Level& level = level_it->second;
        while (aggressor.remaining > 0 && !level.orders.empty()) {
            QueuedOrder& resting = level.orders.front();  // Highest priority at this price.
            if (is_self_trade(aggressor.owner, resting.owner)) {
                prevent_self_trade(level, resting_side, level_price, aggressor);
                continue;
            }

            const Qty fill = std::min(aggressor.remaining, resting.remaining);
            aggressor.remaining -= fill;
            resting.remaining -= fill;
            level.total_qty -= fill;

            listener_->on_trade({.match_id = next_match_id_++,
                                 .aggressor_id = aggressor.id,
                                 .resting_id = resting.id,
                                 .aggressor_side = aggressor.side,
                                 .price = level_price,
                                 .qty = fill,
                                 .aggressor_remaining = aggressor.remaining,
                                 .resting_remaining = resting.remaining});

            if (resting.remaining == 0) {
                index_.erase(resting.id);
                level.orders.pop_front();
            }
            emit_level_update(resting_side, level_price, level);
        }

        if (level.orders.empty()) {
            levels.erase(level_it);
        }
    }
}

// Called when the front order of `level` has the aggressor's owner.
void MapOrderBook::prevent_self_trade(Level& level, Side resting_side, Price price,
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
            cancel_front(level, resting_side, price, CancelReason::SelfTradePrevention);
            return;
        case SelfTradePolicy::CancelBoth:
            cancel_front(level, resting_side, price, CancelReason::SelfTradePrevention);
            cancel_aggressor(aggressor.remaining);
            return;
        case SelfTradePolicy::Decrement: {
            QueuedOrder& resting = level.orders.front();
            const Qty reduce = std::min(aggressor.remaining, resting.remaining);
            resting.remaining -= reduce;
            level.total_qty -= reduce;
            const OrderId resting_id = resting.id;
            const Qty resting_left = resting.remaining;
            if (resting_left == 0) {
                index_.erase(resting_id);
                level.orders.pop_front();
            }
            listener_->on_canceled({.order_id = resting_id,
                                    .canceled_qty = reduce,
                                    .remaining_qty = resting_left,
                                    .reason = CancelReason::SelfTradePrevention});
            emit_level_update(resting_side, price, level);
            cancel_aggressor(reduce);
            return;
        }
    }
}

void MapOrderBook::cancel_front(Level& level, Side side, Price price, CancelReason reason) {
    const QueuedOrder front = level.orders.front();
    level.total_qty -= front.remaining;
    index_.erase(front.id);
    level.orders.pop_front();
    listener_->on_canceled({.order_id = front.id,
                            .canceled_qty = front.remaining,
                            .remaining_qty = 0,
                            .reason = reason});
    emit_level_update(side, price, level);
}

void MapOrderBook::rest(const Aggressor& aggressor, bool post_only) {
    const Level& level = place({.id = aggressor.id,
                                .owner = aggressor.owner,
                                .side = aggressor.side,
                                .price = aggressor.limit,
                                .remaining = aggressor.remaining,
                                .post_only = post_only});
    emit_level_update(aggressor.side, aggressor.limit, level);
}

const MapOrderBook::Level& MapOrderBook::place(const RestingOrder& order) {
    return with_levels(order.side, [&](auto& levels) -> const Level& {
        Level& level = levels[order.price];  // creates an empty level if the price is new
        level.orders.push_back({.id = order.id,
                                .owner = order.owner,
                                .remaining = order.remaining,
                                .post_only = order.post_only});
        level.total_qty += order.remaining;
        index_.emplace(order.id, Location{.side = order.side,
                                          .price = order.price,
                                          .order = std::prev(level.orders.end())});
        return level;
    });
}

// The checks run in the same order in every book, so they all refuse a bad order for
// the same reason.
RestoreStatus MapOrderBook::restore_resting(const RestingOrder& order) {
    if (!valid_price(order.price) || !in_band(order.price)) {
        return RestoreStatus::BadPrice;
    }
    if (order.remaining == 0 || !valid_qty(order.remaining)) {
        return RestoreStatus::BadQuantity;
    }
    if (index_.contains(order.id)) {
        return RestoreStatus::DuplicateId;
    }
    if (would_cross(order.side, order.price)) {
        return RestoreStatus::WouldCross;
    }
    if (index_.size() >= config_.max_orders) {
        return RestoreStatus::BookFull;
    }
    place(order);
    return RestoreStatus::Ok;
}

void MapOrderBook::emit_level_update(Side side, Price price, const Level& level) {
    listener_->on_level_update({.side = side,
                                .price = price,
                                .total_qty = level.total_qty,
                                .order_count = static_cast<std::uint32_t>(level.orders.size())});
}

// Queries.

std::optional<Price> MapOrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> MapOrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

AggregateQty MapOrderBook::volume_at(Side side, Price price) const {
    return with_levels(side, [price](const auto& levels) -> AggregateQty {
        const auto it = levels.find(price);
        return it == levels.end() ? 0 : it->second.total_qty;
    });
}

std::vector<LevelSummary> MapOrderBook::depth(Side side, std::size_t max_levels) const {
    std::vector<LevelSummary> out;
    with_levels(side, [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            if (out.size() == max_levels) {
                break;
            }
            out.push_back({.price = price,
                           .total_qty = level.total_qty,
                           .order_count = static_cast<std::uint32_t>(level.orders.size())});
        }
    });
    return out;
}

bool MapOrderBook::contains(OrderId id) const {
    return index_.contains(id);
}

std::size_t MapOrderBook::level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

// Invariant checks.

template <class Levels>
bool MapOrderBook::check_side(const Levels& levels, Side side, std::size_t& orders_seen) const {
    for (const auto& [price, level] : levels) {
        if (level.orders.empty() || !valid_price(price) || !in_band(price)) {
            return false;  // Breaks invariant 2 or 7.
        }
        AggregateQty sum = 0;
        for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
            if (it->remaining == 0 || !valid_qty(it->remaining)) {
                return false;  // Breaks invariant 3 or 7.
            }
            sum += it->remaining;
            const auto found = index_.find(it->id);
            if (found == index_.end() || found->second.side != side ||
                found->second.price != price || found->second.order != it) {
                return false;  // Breaks invariant 6.
            }
            ++orders_seen;
        }
        if (sum != level.total_qty) {
            return false;  // Breaks invariant 4.
        }
    }
    return true;
}

bool MapOrderBook::check_invariants() const {
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        return false;  // Breaks invariant 1.
    }
    std::size_t orders_seen = 0;
    if (!check_side(bids_, Side::Buy, orders_seen) || !check_side(asks_, Side::Sell, orders_seen)) {
        return false;
    }
    // This checks invariant 6 in the other direction, so no index entry exists for an order
    // that is not in the book. Invariant 5 (priority order) holds by construction, because
    // orders are only added with push_back, and it is tested behaviorally.
    return orders_seen == index_.size() && index_.size() <= config_.max_orders;  // Invariants 6, 8.
}

}  // namespace litbook
