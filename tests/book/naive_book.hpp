#pragma once

// Test oracle: an intentionally naive order book that shares no code or data
// structures with MapOrderBook. One flat vector of orders, linear scans for
// everything. Slow (O(n) per operation) but small enough to verify by reading.
//
// Invariant: produces the same event stream as any correct book (see events.hpp).

#include "litbook/book/events.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace litbook::testing {

class NaiveBook {
public:
    explicit NaiveBook(OrderBookListener& listener) : listener_(&listener) {}

    void add_limit(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0) {
            listener_->on_rejected({.order_id = id, .reason = RejectReason::ZeroQuantity});
            return;
        }
        if (find(id) != orders_.end()) {
            listener_->on_rejected({.order_id = id, .reason = RejectReason::DuplicateOrderId});
            return;
        }
        listener_->on_accepted({.order_id = id, .side = side, .price = price, .qty = qty});

        Qty remaining = qty;
        while (remaining > 0) {
            const auto best = best_opposite(side, price);
            if (best == orders_.end()) {
                break;
            }
            const Qty fill = std::min(remaining, best->remaining);
            remaining -= fill;
            best->remaining -= fill;
            const Order resting = *best;  // copy: erase below invalidates `best`
            listener_->on_trade({.match_id = next_match_id_++,
                                 .aggressor_id = id,
                                 .resting_id = resting.id,
                                 .aggressor_side = side,
                                 .price = resting.price,
                                 .qty = fill,
                                 .aggressor_remaining = remaining,
                                 .resting_remaining = resting.remaining});
            if (resting.remaining == 0) {
                orders_.erase(best);
            }
            emit_level(resting.side, resting.price);
        }

        if (remaining > 0) {
            orders_.push_back({.id = id,
                               .side = side,
                               .price = price,
                               .remaining = remaining,
                               .seq = next_seq_++});
            emit_level(side, price);
        }
    }

    void cancel(OrderId id) {
        const auto it = find(id);
        if (it == orders_.end()) {
            listener_->on_cancel_rejected({.order_id = id, .reason = RejectReason::UnknownOrderId});
            return;
        }
        const Order order = *it;
        orders_.erase(it);
        listener_->on_canceled({.order_id = id, .canceled_qty = order.remaining});
        emit_level(order.side, order.price);
    }

    [[nodiscard]] std::optional<Price> best(Side side) const {
        std::optional<Price> out;
        for (const Order& o : orders_) {
            if (o.side == side && (!out || (side == Side::Buy ? o.price > *out : o.price < *out))) {
                out = o.price;
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t order_count() const { return orders_.size(); }

private:
    struct Order {
        OrderId id;
        Side side;
        Price price;
        Qty remaining;
        std::uint64_t seq;  // arrival order
    };
    using Iter = std::vector<Order>::iterator;

    Iter find(OrderId id) {
        return std::ranges::find_if(orders_, [id](const Order& o) { return o.id == id; });
    }

    // Best resting order the aggressor can trade with: best price, then earliest arrival.
    Iter best_opposite(Side aggressor_side, Price limit) {
        auto best = orders_.end();
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->side == aggressor_side) {
                continue;
            }
            const bool crosses =
                aggressor_side == Side::Buy ? it->price <= limit : it->price >= limit;
            if (!crosses) {
                continue;
            }
            if (best == orders_.end()) {
                best = it;
                continue;
            }
            const bool better_price =
                aggressor_side == Side::Buy ? it->price < best->price : it->price > best->price;
            if (better_price || (it->price == best->price && it->seq < best->seq)) {
                best = it;
            }
        }
        return best;
    }

    void emit_level(Side side, Price price) {
        AggregateQty total = 0;
        std::uint32_t count = 0;
        for (const Order& o : orders_) {
            if (o.side == side && o.price == price) {
                total += o.remaining;
                ++count;
            }
        }
        listener_->on_level_update(
            {.side = side, .price = price, .total_qty = total, .order_count = count});
    }

    OrderBookListener* listener_;
    std::vector<Order> orders_;
    std::uint64_t next_seq_ = 0;
    MatchId next_match_id_ = 1;
};

}  // namespace litbook::testing
