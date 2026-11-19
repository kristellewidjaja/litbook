#pragma once

// This is the test oracle, an order book that is naive on purpose. It shares no code or
// data structures with MapOrderBook. It keeps one flat vector of orders and uses linear
// scans for everything. It is slow (O(n) per step) but small enough to verify by reading,
// rule by rule, against the contract in events.hpp.
//
// Its invariant is that it produces the same event stream as any correct book.

#include "litbook/book/book_config.hpp"
#include "litbook/book/events.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace litbook::testing {

class NaiveBook {
public:
    NaiveBook(OrderBookListener& listener, BookConfig config)
        : listener_(&listener), config_(config) {}

    void submit(const OrderRequest& r) {
        // Validate the request, checking in the documented order.
        std::optional<RejectReason> reject;
        if (r.qty == 0) {
            reject = RejectReason::ZeroQuantity;
        } else if (r.post_only &&
                   (r.type == OrderType::Market || r.tif != TimeInForce::GoodTillCancel)) {
            reject = RejectReason::InvalidOrderType;
        } else if (r.type == OrderType::Limit && r.price % config_.tick_size != 0) {
            reject = RejectReason::InvalidPrice;
        } else if (r.type == OrderType::Limit && !in_band(r.price)) {
            reject = RejectReason::PriceOutOfRange;
        } else if (r.qty % config_.lot_size != 0) {
            reject = RejectReason::InvalidLotSize;
        } else if (find(r.id) != nullptr) {
            reject = RejectReason::DuplicateOrderId;
        } else if (r.post_only && first_crossing(r.side, r.price, false) != nullptr) {
            reject = RejectReason::PostOnlyWouldCross;
        }
        if (reject) {
            listener_->on_rejected({.order_id = r.id, .reason = *reject});
            return;
        }

        const bool market = r.type == OrderType::Market;
        listener_->on_accepted({.order_id = r.id,
                                .owner = r.owner,
                                .side = r.side,
                                .type = r.type,
                                .tif = r.tif,
                                .post_only = r.post_only,
                                .price = market ? 0 : r.price,
                                .qty = r.qty});

        Incoming in{.id = r.id,
                    .owner = r.owner,
                    .side = r.side,
                    .price = r.price,
                    .market = market,
                    .left = r.qty};

        if (r.tif == TimeInForce::FillOrKill && !fok_fillable(in)) {
            canceled(in.id, in.left, 0, CancelReason::Unfilled);
            return;
        }
        run_matching(in);
        if (in.left == 0) {
            return;
        }
        if (!market && r.tif == TimeInForce::GoodTillCancel) {
            if (orders_.size() >= config_.max_orders) {
                canceled(in.id, in.left, 0, CancelReason::BookFull);
            } else {
                add_resting(in, r.post_only);
            }
        } else {
            canceled(in.id, in.left, 0, CancelReason::Unfilled);
        }
    }

    void cancel(OrderId id) {
        const Order* o = find(id);
        if (o == nullptr) {
            listener_->on_cancel_rejected({.order_id = id, .reason = RejectReason::UnknownOrderId});
            return;
        }
        const Order copy = *o;
        erase(id);
        canceled(id, copy.left, 0, CancelReason::UserRequested);
        level(copy.side, copy.price);
    }

    void modify(OrderId id, Price price, Qty qty) {
        Order* o = find(id);
        std::optional<RejectReason> reject;
        if (o == nullptr) {
            reject = RejectReason::UnknownOrderId;
        } else if (qty == 0) {
            reject = RejectReason::ZeroQuantity;
        } else if (price % config_.tick_size != 0) {
            reject = RejectReason::InvalidPrice;
        } else if (!in_band(price)) {
            reject = RejectReason::PriceOutOfRange;
        } else if (qty % config_.lot_size != 0) {
            reject = RejectReason::InvalidLotSize;
        } else if (o->post_only && first_crossing(o->side, price, false) != nullptr) {
            reject = RejectReason::PostOnlyWouldCross;
        }
        if (reject) {
            listener_->on_modify_rejected({.order_id = id, .reason = *reject});
            return;
        }

        if (price == o->price && qty <= o->left) {
            o->left = qty;
            listener_->on_modified({.order_id = id,
                                    .side = o->side,
                                    .price = price,
                                    .qty = qty,
                                    .kept_priority = true});
            level(o->side, o->price);
            return;
        }

        const Order old = *o;
        erase(id);
        listener_->on_modified(
            {.order_id = id, .side = old.side, .price = price, .qty = qty, .kept_priority = false});
        level(old.side, old.price);

        Incoming in{.id = id,
                    .owner = old.owner,
                    .side = old.side,
                    .price = price,
                    .market = false,
                    .left = qty};
        run_matching(in);
        if (in.left > 0) {
            add_resting(in, old.post_only);
        }
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
        OwnerId owner;
        Side side;
        Price price;
        Qty left;
        bool post_only;
        std::uint64_t seq;  // arrival order, where smaller means earlier
    };

    struct Incoming {
        OrderId id;
        OwnerId owner;
        Side side;
        Price price;
        bool market;
        Qty left;
    };

    [[nodiscard]] bool in_band(Price p) const {
        return config_.min_price <= p && p <= config_.max_price;
    }

    static bool same_owner(OwnerId a, OwnerId b) { return a != kNoOwner && a == b; }

    Order* find(OrderId id) {
        for (Order& o : orders_) {
            if (o.id == id) {
                return &o;
            }
        }
        return nullptr;
    }

    void erase(OrderId id) {
        std::erase_if(orders_, [id](const Order& o) { return o.id == id; });
    }

    // Compares two resting orders on the same side by price priority, then by time
    // priority.
    static bool better(const Order& a, const Order& b) {
        if (a.price != b.price) {
            return a.side == Side::Sell ? a.price < b.price : a.price > b.price;
        }
        return a.seq < b.seq;
    }

    static bool reaches(const Order& resting, Side side, Price price, bool market) {
        return market || (side == Side::Buy ? resting.price <= price : resting.price >= price);
    }

    // Returns the highest-priority resting order that an incoming order on `side` at `price`
    // would reach (at any price if `market` is set), or nullptr if there is none.
    Order* first_crossing(Side side, Price price, bool market) {
        // NOLINTNEXTLINE(misc-const-correctness): returned as a mutable pointer below
        Order* best_order = nullptr;
        for (Order& o : orders_) {
            if (o.side != side && reaches(o, side, price, market) &&
                (best_order == nullptr || better(o, *best_order))) {
                best_order = &o;
            }
        }
        return best_order;
    }

    bool fok_fillable(const Incoming& in) {
        std::vector<Order> reachable;
        for (const Order& o : orders_) {
            if (o.side != in.side && reaches(o, in.side, in.price, in.market)) {
                reachable.push_back(o);
            }
        }
        std::ranges::sort(reachable, better);
        AggregateQty total = 0;
        for (const Order& o : reachable) {
            if (same_owner(o.owner, in.owner)) {
                return false;
            }
            total += o.left;
            if (total >= in.left) {
                return true;
            }
        }
        return false;
    }

    void run_matching(Incoming& in) {
        while (in.left > 0) {
            Order* o = first_crossing(in.side, in.price, in.market);
            if (o == nullptr) {
                return;
            }
            const Side rest_side = o->side;
            const Price rest_price = o->price;
            const OrderId rest_id = o->id;

            if (same_owner(o->owner, in.owner)) {
                self_trade(in, *o);
                continue;
            }

            const Qty fill = std::min(in.left, o->left);
            in.left -= fill;
            o->left -= fill;
            const Qty rest_left = o->left;
            listener_->on_trade({.match_id = next_match_++,
                                 .aggressor_id = in.id,
                                 .resting_id = rest_id,
                                 .aggressor_side = in.side,
                                 .price = rest_price,
                                 .qty = fill,
                                 .aggressor_remaining = in.left,
                                 .resting_remaining = rest_left});
            if (rest_left == 0) {
                erase(rest_id);
            }
            level(rest_side, rest_price);
        }
    }

    void self_trade(Incoming& in, Order& o) {
        const Side rest_side = o.side;
        const Price rest_price = o.price;
        const OrderId rest_id = o.id;
        const Qty rest_qty = o.left;

        switch (config_.self_trade_policy) {
            case SelfTradePolicy::CancelNewest: {
                const Qty mine = in.left;
                in.left = 0;
                canceled(in.id, mine, 0, CancelReason::SelfTradePrevention);
                return;
            }
            case SelfTradePolicy::CancelOldest:
                erase(rest_id);
                canceled(rest_id, rest_qty, 0, CancelReason::SelfTradePrevention);
                level(rest_side, rest_price);
                return;
            case SelfTradePolicy::CancelBoth: {
                erase(rest_id);
                canceled(rest_id, rest_qty, 0, CancelReason::SelfTradePrevention);
                level(rest_side, rest_price);
                const Qty mine = in.left;
                in.left = 0;
                canceled(in.id, mine, 0, CancelReason::SelfTradePrevention);
                return;
            }
            case SelfTradePolicy::Decrement: {
                const Qty d = std::min(in.left, rest_qty);
                o.left -= d;
                const Qty rest_left = o.left;
                if (rest_left == 0) {
                    erase(rest_id);
                }
                canceled(rest_id, d, rest_left, CancelReason::SelfTradePrevention);
                level(rest_side, rest_price);
                in.left -= d;
                canceled(in.id, d, in.left, CancelReason::SelfTradePrevention);
                return;
            }
        }
    }

    void add_resting(const Incoming& in, bool post_only) {
        orders_.push_back({.id = in.id,
                           .owner = in.owner,
                           .side = in.side,
                           .price = in.price,
                           .left = in.left,
                           .post_only = post_only,
                           .seq = next_seq_++});
        level(in.side, in.price);
    }

    void canceled(OrderId id, Qty qty, Qty left, CancelReason reason) {
        listener_->on_canceled(
            {.order_id = id, .canceled_qty = qty, .remaining_qty = left, .reason = reason});
    }

    void level(Side side, Price price) {
        AggregateQty total = 0;
        std::uint32_t count = 0;
        for (const Order& o : orders_) {
            if (o.side == side && o.price == price) {
                total += o.left;
                ++count;
            }
        }
        listener_->on_level_update(
            {.side = side, .price = price, .total_qty = total, .order_count = count});
    }

    OrderBookListener* listener_;
    BookConfig config_;
    std::vector<Order> orders_;
    std::uint64_t next_seq_ = 0;
    MatchId next_match_ = 1;
};

}  // namespace litbook::testing
