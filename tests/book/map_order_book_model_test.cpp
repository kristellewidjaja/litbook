// Model-based test: random operations go to MapOrderBook and to an independent
// naive oracle (naive_book.hpp). Both must emit identical event streams and agree on
// best prices; MapOrderBook's invariants must hold after every operation.

#include "litbook/book/book_config.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <random>
#include <unordered_map>
#include <variant>
#include <vector>

#include "book/naive_book.hpp"

namespace litbook {
namespace {

struct WorkloadParams {
    const char* name;
    std::uint64_t seed;
    int operations;
    Price min_price;
    Price max_price;
    std::uint32_t max_lots;  // quantities are mostly 0..max_lots lots
    int cancel_percent;
    int modify_percent;  // remaining operations are new orders
    BookConfig config;
    OwnerId owners;  // owners drawn from 0..owners (0 = exempt from self-trade prevention)
};

// Generates a random but reproducible stream of operations.
class Generator {
public:
    explicit Generator(const WorkloadParams& p) : p_(p), rng_(p.seed) {}

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

private:
    template <class T>
    T uniform(T lo, T hi) {
        return std::uniform_int_distribution<T>(lo, hi)(rng_);
    }

    WorkloadParams p_;
    std::mt19937_64 rng_;
};

// Counts what a workload actually exercised, so a generator change that silently stops
// producing (say) self-trades makes the test fail instead of passing vacuously.
struct Coverage {
    int trades = 0;
    int resting_adds = 0;
    int user_cancels = 0;
    int unfilled_cancels = 0;  // IOC / FOK / market remainders
    int stp_cancels = 0;
    int stp_partial = 0;  // Decrement policy leaving quantity behind
    int kept_priority = 0;
    int lost_priority = 0;
    std::array<int, 8> rejects{};  // indexed by RejectReason

    void count(const BookEvent& event) {
        if (const auto* t = std::get_if<Trade>(&event)) {
            (void)t;
            ++trades;
        } else if (const auto* c = std::get_if<OrderCanceled>(&event)) {
            if (c->reason == CancelReason::UserRequested) {
                ++user_cancels;
            } else if (c->reason == CancelReason::Unfilled) {
                ++unfilled_cancels;
            } else {
                ++stp_cancels;
                stp_partial += c->remaining_qty > 0 ? 1 : 0;
            }
        } else if (const auto* m = std::get_if<OrderModified>(&event)) {
            ++(m->kept_priority ? kept_priority : lost_priority);
        } else if (const auto* r = std::get_if<OrderRejected>(&event)) {
            ++rejects[static_cast<std::size_t>(r->reason)];
        } else if (const auto* mr = std::get_if<ModifyRejected>(&event)) {
            ++rejects[static_cast<std::size_t>(mr->reason)];
        } else if (const auto* cr = std::get_if<CancelRejected>(&event)) {
            ++rejects[static_cast<std::size_t>(cr->reason)];
        } else if (std::holds_alternative<LevelUpdate>(event)) {
            const auto& l = std::get<LevelUpdate>(event);
            resting_adds += l.order_count > 0 ? 1 : 0;
        }
    }
    int rejected(RejectReason reason) const { return rejects[static_cast<std::size_t>(reason)]; }
};

// Lets GoogleTest print a workload by name instead of as a byte dump.
void PrintTo(const WorkloadParams& p, std::ostream* os) {
    *os << p.name;
}

class MapOrderBookModelTest : public ::testing::TestWithParam<WorkloadParams> {};

TEST_P(MapOrderBookModelTest, MatchesNaiveOracle) {
    const WorkloadParams& p = GetParam();
    Generator gen(p);

    RecordingListener map_rec;
    RecordingListener naive_rec;
    MapOrderBook map_book(map_rec, p.config);
    testing::NaiveBook naive_book(naive_rec, p.config);

    OrderId next_id = 1;
    std::vector<OrderId> issued;  // every ID used, so cancels/modifies hit live and dead orders
    std::unordered_map<OrderId, Price> last_price;  // so modifies can target the same price
    Coverage cov;

    for (int op = 0; op < p.operations; ++op) {
        const int roll = gen.percent();
        if (!issued.empty() && roll < p.cancel_percent) {
            const OrderId id = gen.percent() < 5 ? next_id + 1'000'000 : gen.pick(issued);
            map_book.cancel(id);
            naive_book.cancel(id);
        } else if (!issued.empty() && roll < p.cancel_percent + p.modify_percent) {
            const OrderId id = gen.percent() < 5 ? next_id + 1'000'000 : gen.pick(issued);
            // Half the time: same price, smaller size (the priority-keeping path).
            const bool same_price = gen.percent() < 50 && last_price.contains(id);
            const Price price = same_price ? last_price[id] : gen.price();
            const Qty qty = same_price ? gen.small_qty() : gen.qty();
            last_price[id] = price;
            map_book.modify(id, price, qty);
            naive_book.modify(id, price, qty);
        } else {
            OrderId id = next_id++;
            if (!issued.empty() && gen.percent() < 2) {
                id = gen.pick(issued);  // deliberately reuse an ID
            } else {
                issued.push_back(id);
            }
            const OrderRequest request = gen.request(id);
            last_price[id] = request.price;
            map_book.submit(request);
            naive_book.submit(request);
        }

        ASSERT_EQ(map_rec.events(), naive_rec.events()) << "diverged at operation " << op;
        ASSERT_TRUE(map_book.check_invariants()) << "invariant broken at operation " << op;
        ASSERT_EQ(map_book.best_bid(), naive_book.best(Side::Buy)) << "operation " << op;
        ASSERT_EQ(map_book.best_ask(), naive_book.best(Side::Sell)) << "operation " << op;
        ASSERT_EQ(map_book.order_count(), naive_book.order_count()) << "operation " << op;
        for (const BookEvent& e : map_rec.events()) {
            cov.count(e);
        }
        map_rec.clear();
        naive_rec.clear();
    }

    // Every workload must exercise every core path at least once...
    EXPECT_GT(cov.trades, 100);
    EXPECT_GT(cov.resting_adds, 100);
    EXPECT_GT(cov.user_cancels, 0);
    EXPECT_GT(cov.unfilled_cancels, 0);
    EXPECT_GT(cov.kept_priority, 0);
    EXPECT_GT(cov.lost_priority, 0);
    for (const RejectReason r :
         {RejectReason::ZeroQuantity, RejectReason::UnknownOrderId, RejectReason::DuplicateOrderId,
          RejectReason::InvalidOrderType, RejectReason::PostOnlyWouldCross}) {
        EXPECT_GT(cov.rejected(r), 0) << "never produced reject " << r;
    }
    // ...and the paths specific to its configuration.
    if (p.config.tick_size > 1) {
        EXPECT_GT(cov.rejected(RejectReason::InvalidPrice), 0);
    }
    if (p.config.lot_size > 1) {
        EXPECT_GT(cov.rejected(RejectReason::InvalidLotSize), 0);
    }
    if (p.owners > 0) {
        EXPECT_GT(cov.stp_cancels, 10);
        if (p.config.self_trade_policy == SelfTradePolicy::Decrement) {
            EXPECT_GT(cov.stp_partial, 0);
        }
    } else {
        EXPECT_EQ(cov.stp_cancels, 0);
    }
    RecordProperty("trades", cov.trades);
    RecordProperty("stp_cancels", cov.stp_cancels);
}

constexpr BookConfig kDefault{};
constexpr BookConfig kTicked{.tick_size = 5, .lot_size = 10};

constexpr BookConfig with_policy(BookConfig c, SelfTradePolicy policy) {
    c.self_trade_policy = policy;
    return c;
}

INSTANTIATE_TEST_SUITE_P(
    Workloads, MapOrderBookModelTest,
    ::testing::Values(
        // No owners: pure price-time matching across all order types.
        WorkloadParams{"Aggressive", 1, 20'000, 98, 102, 50, 15, 10, kDefault, 0},
        WorkloadParams{"DeepBook", 2, 20'000, 1, 1'000, 100, 15, 10, kDefault, 0},
        WorkloadParams{"CancelHeavy", 3, 20'000, 95, 105, 20, 60, 20, kDefault, 0},
        WorkloadParams{"ModifyHeavy", 4, 20'000, 95, 105, 20, 10, 50, kDefault, 0},
        WorkloadParams{"SmallQty", 5, 20'000, 99, 101, 3, 25, 25, kDefault, 0},
        WorkloadParams{"NegativePrices", 6, 10'000, -10, 10, 30, 25, 10, kDefault, 0},
        // Tick 5, lot 10: off-tick prices and odd lots get rejected.
        WorkloadParams{"TickAndLot", 7, 20'000, 90, 110, 10, 20, 15, kTicked, 0},
        WorkloadParams{"TickNegative", 8, 10'000, -50, 50, 10, 20, 15, kTicked, 0},
        // Few owners: many self-trades, one workload per policy.
        WorkloadParams{"StpCancelNewest", 9, 20'000, 97, 103, 20, 15, 15,
                       with_policy(kDefault, SelfTradePolicy::CancelNewest), 3},
        WorkloadParams{"StpCancelOldest", 10, 20'000, 97, 103, 20, 15, 15,
                       with_policy(kDefault, SelfTradePolicy::CancelOldest), 3},
        WorkloadParams{"StpCancelBoth", 11, 20'000, 97, 103, 20, 15, 15,
                       with_policy(kDefault, SelfTradePolicy::CancelBoth), 3},
        WorkloadParams{"StpDecrement", 12, 20'000, 97, 103, 20, 15, 15,
                       with_policy(kDefault, SelfTradePolicy::Decrement), 3},
        WorkloadParams{"StpDecrementTicked", 13, 20'000, 90, 110, 10, 15, 15,
                       with_policy(kTicked, SelfTradePolicy::Decrement), 2}),
    [](const ::testing::TestParamInfo<WorkloadParams>& param_info) {
        return param_info.param.name;
    });

}  // namespace
}  // namespace litbook
