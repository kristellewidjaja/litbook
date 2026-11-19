#pragma once

// A generator of order flow that looks more like a real market than the test
// generator in tests/book/workload.hpp. The test generator sends invalid requests on
// purpose to reach every rule. This one only sends valid orders, and it copies four
// well-known features of real order flow.
//
// 1. Most orders are canceled before they trade. In a full day of Nasdaq ITCH data
//    there are about as many deletes as adds, and only a few percent of orders execute.
// 2. A book holds a roughly steady number of orders, in the thousands for one stock.
//    Here the chance that the next operation is a cancel grows with the number of
//    outstanding orders, so the book settles around target_orders instead of growing
//    for the whole run.
// 3. Most new orders are placed at or near the best price, with fewer and fewer orders
//    further away. Here the distance in ticks follows a geometric distribution.
// 4. The price drifts. The middle price takes a random one-tick step now and then.
//
// The stream is generated up front and does not depend on any book, so the exact same
// operations can be replayed into every book implementation. Because of that, the
// generator does not know which orders have traded. It cancels orders it has not
// canceled yet, and a few of those have already traded, so their cancels are rejected.
// That happens in real markets too.

#include "litbook/book/order_request.hpp"
#include "litbook/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "book/workload.hpp"

namespace litbook::bench {

struct MarketParams {
    const char* name;
    std::uint64_t seed;
    int operations;
    Price start_mid;
    std::size_t target_orders;  // the book size the cancel rate steers toward
    int modify_percent;         // share of operations that resize an outstanding order
    int aggressive_percent;     // share of new orders priced to trade immediately
    double distance_p;          // geometric parameter, where larger keeps orders closer to the best
    int drift_percent;          // chance per operation that the middle price moves one tick
};

class MarketGenerator {
public:
    explicit MarketGenerator(const MarketParams& p) : p_(p), rng_(p.seed), mid_(p.start_mid) {}

    std::vector<testing::Operation> generate() {
        std::vector<testing::Operation> ops;
        ops.reserve(static_cast<std::size_t>(p_.operations));
        for (int i = 0; i < p_.operations; ++i) {
            ops.push_back(next());
        }
        return ops;
    }

private:
    struct Sent {
        OrderId id;
        Price price;
    };

    testing::Operation next() {
        if (percent() < p_.drift_percent) {
            mid_ += coin() ? 1 : -1;
        }
        if (!outstanding_.empty() && percent() < p_.modify_percent) {
            // A new size at the same price. A smaller size keeps the order's place in
            // line and a larger one loses it.
            const Sent& s = outstanding_[pick()];
            return {.kind = testing::Operation::Kind::Modify,
                    .request = {},
                    .id = s.id,
                    .price = s.price,
                    .qty = 1 + static_cast<Qty>(uniform(0, 99))};
        }
        // With n orders outstanding, cancel with probability n / (2 * target). At the
        // target, cancels and adds are equally likely, so the book stops growing there.
        const double cancel_chance =
            std::min(0.95, static_cast<double>(outstanding_.size()) /
                               (2.0 * static_cast<double>(p_.target_orders)));
        if (!outstanding_.empty() && std::bernoulli_distribution(cancel_chance)(rng_)) {
            const std::size_t i = pick();
            const OrderId id = outstanding_[i].id;
            outstanding_[i] = outstanding_.back();  // remove it by moving the last one in
            outstanding_.pop_back();
            return {.kind = testing::Operation::Kind::Cancel,
                    .request = {},
                    .id = id,
                    .price = 0,
                    .qty = 0};
        }
        return new_order();
    }

    testing::Operation new_order() {
        const Side side = coin() ? Side::Buy : Side::Sell;
        const Price toward = side == Side::Buy ? 1 : -1;  // the direction that crosses the market
        Price price = 0;
        if (percent() < p_.aggressive_percent) {
            // Priced up to 3 ticks through the middle, so it trades with the other side.
            price = mid_ + toward * (1 + uniform(0, 2));
        } else {
            // A passive order at or behind the best price on its own side.
            std::geometric_distribution<Price> distance(p_.distance_p);
            price = mid_ - toward * (1 + distance(rng_));
        }
        const OrderId id = next_id_++;
        outstanding_.push_back({id, price});
        const OrderRequest r{
            .id = id, .side = side, .price = price, .qty = 1 + static_cast<Qty>(uniform(0, 99))};
        return {
            .kind = testing::Operation::Kind::Submit, .request = r, .id = 0, .price = 0, .qty = 0};
    }

    std::size_t pick() {
        return static_cast<std::size_t>(uniform(0, static_cast<Price>(outstanding_.size()) - 1));
    }
    bool coin() { return (rng_() & 1U) != 0; }
    int percent() { return static_cast<int>(uniform(0, 99)); }
    Price uniform(Price lo, Price hi) { return std::uniform_int_distribution<Price>(lo, hi)(rng_); }

    MarketParams p_;
    std::mt19937_64 rng_;
    Price mid_;
    OrderId next_id_ = 1;
    std::vector<Sent> outstanding_;  // orders sent and not yet canceled by this generator
};

}  // namespace litbook::bench
