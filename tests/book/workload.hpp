#pragma once

// Random but reproducible streams of book operations, shared by the model test
// (books vs the naive oracle), the long differential test (fast vs map) and the
// no-allocation test. Operations are generated up front, independent of any book, so
// the same stream can be replayed into several books.

#include "litbook/book/book_config.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <random>
#include <unordered_map>
#include <vector>

#include "book/book_test_support.hpp"

namespace litbook::testing {

struct WorkloadParams {
    const char* name;
    std::uint64_t seed;
    int operations;
    Price min_price;  // generated prices; the book's band (config) may be narrower
    Price max_price;
    std::uint32_t max_lots;  // quantities are mostly 0..max_lots lots
    int cancel_percent;
    int modify_percent;  // remaining operations are new orders
    BookConfig config;   // band / capacity left unbounded: see book_config() below
    OwnerId owners;      // owners drawn from 0..owners (0 = exempt from self-trade prevention)
};

// Lets GoogleTest print a workload by name instead of as a byte dump.
inline void PrintTo(const WorkloadParams& p, std::ostream* os) {
    *os << p.name;
}

struct Operation {
    enum class Kind : std::uint8_t { Submit, Cancel, Modify };
    Kind kind;
    OrderRequest request;  // Submit
    OrderId id;            // Cancel, Modify
    Price price;           // Modify
    Qty qty;               // Modify
};

template <class Book>
void apply(Book& book, const Operation& op) {
    switch (op.kind) {
        case Operation::Kind::Submit:
            book.submit(op.request);
            return;
        case Operation::Kind::Cancel:
            book.cancel(op.id);
            return;
        case Operation::Kind::Modify:
            book.modify(op.id, op.price, op.qty);
            return;
    }
}

class Generator {
public:
    explicit Generator(const WorkloadParams& p) : p_(p), rng_(p.seed) {}

    std::vector<Operation> generate() {
        std::vector<Operation> ops;
        ops.reserve(static_cast<std::size_t>(p_.operations));
        for (int i = 0; i < p_.operations; ++i) {
            ops.push_back(next());
        }
        return ops;
    }

    // The next operation of the stream (for streaming very long runs).
    Operation next() {
        const int roll = percent();
        if (!issued_.empty() && roll < p_.cancel_percent) {
            return {.kind = Operation::Kind::Cancel,
                    .request = {},
                    .id = target(),
                    .price = 0,
                    .qty = 0};
        }
        if (!issued_.empty() && roll < p_.cancel_percent + p_.modify_percent) {
            const OrderId id = target();
            // Half the time: same price, smaller size (the priority-keeping path).
            const bool same_price = percent() < 50 && last_price_.contains(id);
            const Price new_price = same_price ? last_price_[id] : price();
            const Qty new_qty = same_price ? small_qty() : qty();
            last_price_[id] = new_price;
            return {.kind = Operation::Kind::Modify,
                    .request = {},
                    .id = id,
                    .price = new_price,
                    .qty = new_qty};
        }
        OrderId id = next_id_++;
        if (!issued_.empty() && percent() < 2) {
            // Deliberately reuse an ID. Half the time a recent one, which is likely still
            // resting (a duplicate); otherwise any, usually long gone (a legal reuse).
            id = percent() < 50 ? recent() : pick(issued_);
        } else {
            issued_.push_back(id);
        }
        const OrderRequest r = request(id);
        last_price_[id] = r.price;
        return {.kind = Operation::Kind::Submit, .request = r, .id = 0, .price = 0, .qty = 0};
    }

private:
    // An ID for cancel/modify: usually one already used (live or dead), sometimes unknown.
    OrderId target() { return percent() < 5 ? next_id_ + 1'000'000 : pick(issued_); }

    OrderId recent() {
        const std::size_t window = std::min<std::size_t>(issued_.size(), 8);
        return issued_[issued_.size() - 1 - uniform<std::size_t>(0, window - 1)];
    }

    int percent() { return uniform(0, 99); }

    Price price() {
        if (percent() < 10) {
            return uniform(p_.min_price, p_.max_price);  // may be off-tick: exercises rejects
        }
        const Price tick = p_.config.tick_size;
        return tick * uniform(p_.min_price / tick, p_.max_price / tick);
    }

    // A smaller quantity (1 lot up to half the usual maximum): for priority-keeping modifies.
    Qty small_qty() {
        return p_.config.lot_size * static_cast<Qty>(uniform<std::uint64_t>(
                                        1, std::max<std::uint64_t>(1, p_.max_lots / 2)));
    }

    Qty qty() {
        const Qty lot = p_.config.lot_size;
        if (percent() < 10) {
            return static_cast<Qty>(uniform<std::uint64_t>(0, std::uint64_t{lot} * p_.max_lots));
        }
        return lot * static_cast<Qty>(uniform<std::uint64_t>(0, p_.max_lots));
    }

    OwnerId owner() { return static_cast<OwnerId>(uniform<std::uint64_t>(0, p_.owners)); }

    OrderRequest request(OrderId id) {
        OrderRequest r{.id = id, .owner = owner(), .side = percent() < 50 ? Side::Buy : Side::Sell};
        r.type = percent() < 8 ? OrderType::Market : OrderType::Limit;
        const int tif = percent();
        r.tif = tif < 70   ? TimeInForce::GoodTillCancel
                : tif < 85 ? TimeInForce::ImmediateOrCancel
                           : TimeInForce::FillOrKill;
        r.post_only = percent() < 12;  // sometimes in invalid combinations, on purpose
        r.price = price();
        r.qty = qty();
        return r;
    }

    template <class T>
    T pick(const std::vector<T>& v) {
        return v[static_cast<std::size_t>(uniform<std::uint64_t>(0, v.size() - 1))];
    }

    template <class T>
    T uniform(T lo, T hi) {
        return std::uniform_int_distribution<T>(lo, hi)(rng_);
    }

    WorkloadParams p_;
    std::mt19937_64 rng_;
    OrderId next_id_ = 1;
    std::vector<OrderId> issued_;  // every ID used, so cancels/modifies hit live and dead orders
    std::unordered_map<OrderId, Price> last_price_;  // so modifies can target the same price
};

// The book config a workload runs with. An unbounded band becomes the generated price
// range plus a 10-tick margin (so no PriceOutOfRange, and a small ladder that is cheap
// to check after every operation); an unbounded capacity becomes kTestMaxOrders.
inline BookConfig book_config(const WorkloadParams& p) {
    BookConfig c = p.config;
    const Price tick = c.tick_size;
    if (c.min_price == std::numeric_limits<Price>::min()) {
        c.min_price = (p.min_price / tick - 10) * tick;
    }
    if (c.max_price == std::numeric_limits<Price>::max()) {
        c.max_price = (p.max_price / tick + 10) * tick;
    }
    return with_test_limits(c);
}

inline std::vector<Operation> generate(const WorkloadParams& p) {
    return Generator(p).generate();
}

constexpr BookConfig with_policy(BookConfig c, SelfTradePolicy policy) {
    c.self_trade_policy = policy;
    return c;
}

// The workloads every randomized test runs. Each targets different paths; the model
// test asserts that each one actually exercised them.
inline std::vector<WorkloadParams> standard_workloads() {
    constexpr BookConfig kDefault{};
    constexpr BookConfig kTicked{.tick_size = 5, .lot_size = 10};
    constexpr BookConfig kBanded{.min_price = 90, .max_price = 110, .max_orders = 40};
    return {
        // No owners: pure price-time matching across all order types.
        {"Aggressive", 1, 20'000, 98, 102, 50, 15, 10, kDefault, 0},
        {"DeepBook", 2, 20'000, 1, 1'000, 100, 15, 10, kDefault, 0},
        {"CancelHeavy", 3, 20'000, 95, 105, 20, 60, 20, kDefault, 0},
        {"ModifyHeavy", 4, 20'000, 95, 105, 20, 10, 50, kDefault, 0},
        {"SmallQty", 5, 20'000, 99, 101, 3, 25, 25, kDefault, 0},
        {"NegativePrices", 6, 10'000, -10, 10, 30, 25, 10, kDefault, 0},
        // Tick 5, lot 10: off-tick prices and odd lots get rejected.
        {"TickAndLot", 7, 20'000, 90, 110, 10, 20, 15, kTicked, 0},
        {"TickNegative", 8, 10'000, -50, 50, 10, 20, 15, kTicked, 0},
        // Few owners: many self-trades, one workload per policy.
        {"StpCancelNewest", 9, 20'000, 97, 103, 20, 15, 15,
         with_policy(kDefault, SelfTradePolicy::CancelNewest), 3},
        {"StpCancelOldest", 10, 20'000, 97, 103, 20, 15, 15,
         with_policy(kDefault, SelfTradePolicy::CancelOldest), 3},
        {"StpCancelBoth", 11, 20'000, 97, 103, 20, 15, 15,
         with_policy(kDefault, SelfTradePolicy::CancelBoth), 3},
        {"StpDecrement", 12, 20'000, 97, 103, 20, 15, 15,
         with_policy(kDefault, SelfTradePolicy::Decrement), 3},
        {"StpDecrementTicked", 13, 20'000, 90, 110, 10, 15, 15,
         with_policy(kTicked, SelfTradePolicy::Decrement), 2},
        // Band narrower than the generated prices, and a book that is often full.
        {"BandAndCapacity", 14, 20'000, 80, 120, 20, 10, 10, kBanded, 2},
        {"TinyCapacity", 15, 20'000, 95, 105, 10, 10, 35,
         BookConfig{.min_price = 0, .max_price = 200, .max_orders = 6}, 0},
    };
}

}  // namespace litbook::testing
