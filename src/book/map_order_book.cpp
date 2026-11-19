#include "litbook/book/map_order_book.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

namespace litbook {

namespace {

// Does an aggressor on `aggressor_side` with limit `limit` trade against a resting
// level at `level_price`?
constexpr bool crosses(Side aggressor_side, Price limit, Price level_price) noexcept {
    return aggressor_side == Side::Buy ? level_price <= limit : level_price >= limit;
}

}  // namespace

MapOrderBook::MapOrderBook(OrderBookListener& listener) noexcept : listener_(&listener) {}

void MapOrderBook::add_limit(OrderId id, Side side, Price price, Qty qty) {
    if (qty == 0) {
        listener_->on_rejected({.order_id = id, .reason = RejectReason::ZeroQuantity});
        return;
    }
    if (index_.contains(id)) {
        listener_->on_rejected({.order_id = id, .reason = RejectReason::DuplicateOrderId});
        return;
    }

    listener_->on_accepted({.order_id = id, .side = side, .price = price, .qty = qty});

    if (side == Side::Buy) {
        const Qty remaining = match(asks_, id, side, price, qty);
        if (remaining > 0) {
            rest(bids_, id, side, price, remaining);
        }
    } else {
        const Qty remaining = match(bids_, id, side, price, qty);
        if (remaining > 0) {
            rest(asks_, id, side, price, remaining);
        }
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

    if (location.side == Side::Buy) {
        remove(bids_, id, location);
    } else {
        remove(asks_, id, location);
    }
}

template <class Levels>
Qty MapOrderBook::match(Levels& opposite_levels, OrderId aggressor_id, Side aggressor_side,
                        Price limit, Qty remaining) {
    const Side resting_side = opposite(aggressor_side);

    while (remaining > 0 && !opposite_levels.empty()) {
        const auto level_it = opposite_levels.begin();  // best opposite price
        const Price level_price = level_it->first;
        if (!crosses(aggressor_side, limit, level_price)) {
            break;
        }

        Level& level = level_it->second;
        while (remaining > 0 && !level.orders.empty()) {
            RestingOrder& resting = level.orders.front();  // earliest arrival at this price
            const Qty fill = std::min(remaining, resting.remaining);
            remaining -= fill;
            resting.remaining -= fill;
            level.total_qty -= fill;

            listener_->on_trade({.match_id = next_match_id_++,
                                 .aggressor_id = aggressor_id,
                                 .resting_id = resting.id,
                                 .aggressor_side = aggressor_side,
                                 .price = level_price,
                                 .qty = fill,
                                 .aggressor_remaining = remaining,
                                 .resting_remaining = resting.remaining});

            if (resting.remaining == 0) {
                index_.erase(resting.id);
                level.orders.pop_front();
            }
            emit_level_update(resting_side, level_price, level);
        }

        if (level.orders.empty()) {
            opposite_levels.erase(level_it);
        }
    }
    return remaining;
}

template <class Levels>
void MapOrderBook::rest(Levels& levels, OrderId id, Side side, Price price, Qty qty) {
    Level& level = levels[price];  // creates an empty level if this price is new
    level.orders.push_back({.id = id, .remaining = qty});
    level.total_qty += qty;
    index_.emplace(id,
                   Location{.side = side, .price = price, .order = std::prev(level.orders.end())});
    emit_level_update(side, price, level);
}

template <class Levels>
void MapOrderBook::remove(Levels& levels, OrderId id, const Location& location) {
    const auto level_it = levels.find(location.price);
    Level& level = level_it->second;
    const Qty canceled = location.order->remaining;

    level.total_qty -= canceled;
    level.orders.erase(location.order);

    listener_->on_canceled({.order_id = id, .canceled_qty = canceled});
    emit_level_update(location.side, location.price, level);

    if (level.orders.empty()) {
        levels.erase(level_it);
    }
}

void MapOrderBook::emit_level_update(Side side, Price price, const Level& level) {
    listener_->on_level_update({.side = side,
                                .price = price,
                                .total_qty = level.total_qty,
                                .order_count = static_cast<std::uint32_t>(level.orders.size())});
}

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
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_qty;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_qty;
}

std::vector<LevelSummary> MapOrderBook::depth(Side side, std::size_t max_levels) const {
    std::vector<LevelSummary> out;
    const auto collect = [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            if (out.size() == max_levels) {
                break;
            }
            out.push_back({.price = price,
                           .total_qty = level.total_qty,
                           .order_count = static_cast<std::uint32_t>(level.orders.size())});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return out;
}

bool MapOrderBook::contains(OrderId id) const {
    return index_.contains(id);
}

std::size_t MapOrderBook::level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

template <class Levels>
bool MapOrderBook::check_side(const Levels& levels, Side side, std::size_t& orders_seen) const {
    for (const auto& [price, level] : levels) {
        if (level.orders.empty()) {
            return false;  // invariant 2
        }
        AggregateQty sum = 0;
        for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
            if (it->remaining == 0) {
                return false;  // invariant 3
            }
            sum += it->remaining;
            const auto found = index_.find(it->id);
            if (found == index_.end() || found->second.side != side ||
                found->second.price != price || found->second.order != it) {
                return false;  // invariant 6
            }
            ++orders_seen;
        }
        if (sum != level.total_qty) {
            return false;  // invariant 4
        }
    }
    return true;
}

bool MapOrderBook::check_invariants() const {
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first) {
        return false;  // invariant 1
    }
    std::size_t orders_seen = 0;
    if (!check_side(bids_, Side::Buy, orders_seen) || !check_side(asks_, Side::Sell, orders_seen)) {
        return false;
    }
    // Invariant 6, other direction: no index entries for orders that are not in the book.
    // (Invariant 5, time order, is enforced structurally by push_back-only insertion and
    // is tested behaviorally.)
    return orders_seen == index_.size();
}

}  // namespace litbook
