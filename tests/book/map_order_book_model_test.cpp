// Model-based test: random operations go to MapOrderBook and to an independent
// naive oracle (naive_book.hpp). Both must emit identical event streams and agree on
// best prices; MapOrderBook's invariants must hold after every operation.

#include "litbook/book/map_order_book.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
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
    Qty max_qty;
    int cancel_percent;  // remaining operations are new orders
};

class MapOrderBookModelTest : public ::testing::TestWithParam<WorkloadParams> {};

TEST_P(MapOrderBookModelTest, MatchesNaiveOracle) {
    const WorkloadParams p = GetParam();
    std::mt19937_64 rng(p.seed);
    std::uniform_int_distribution<Price> price_dist(p.min_price, p.max_price);
    std::uniform_int_distribution<Qty> qty_dist(0, p.max_qty);  // 0 exercises the reject path
    std::uniform_int_distribution<int> percent(0, 99);

    RecordingListener map_rec;
    RecordingListener naive_rec;
    MapOrderBook map_book(map_rec);
    testing::NaiveBook naive_book(naive_rec);

    OrderId next_id = 1;
    std::vector<OrderId>
        issued;  // every ID ever used, so cancels hit live, filled and canceled orders

    for (int op = 0; op < p.operations; ++op) {
        const bool do_cancel = !issued.empty() && percent(rng) < p.cancel_percent;
        if (do_cancel) {
            std::uniform_int_distribution<std::size_t> pick(0, issued.size() - 1);
            // 1 in 20 cancels targets an ID that was never issued.
            const OrderId id = percent(rng) < 5 ? next_id + 1'000'000 : issued[pick(rng)];
            map_book.cancel(id);
            naive_book.cancel(id);
        } else {
            // 1 in 50 new orders deliberately reuses an existing ID.
            OrderId id = next_id++;
            if (!issued.empty() && percent(rng) < 2) {
                std::uniform_int_distribution<std::size_t> pick(0, issued.size() - 1);
                id = issued[pick(rng)];
            } else {
                issued.push_back(id);
            }
            const Side side = percent(rng) < 50 ? Side::Buy : Side::Sell;
            const Price price = price_dist(rng);
            const Qty qty = qty_dist(rng);
            map_book.add_limit(id, side, price, qty);
            naive_book.add_limit(id, side, price, qty);
        }

        ASSERT_EQ(map_rec.events(), naive_rec.events()) << "diverged at operation " << op;
        ASSERT_TRUE(map_book.check_invariants()) << "invariant broken at operation " << op;
        ASSERT_EQ(map_book.best_bid(), naive_book.best(Side::Buy)) << "operation " << op;
        ASSERT_EQ(map_book.best_ask(), naive_book.best(Side::Sell)) << "operation " << op;
        ASSERT_EQ(map_book.order_count(), naive_book.order_count()) << "operation " << op;
        map_rec.clear();
        naive_rec.clear();
    }
}

INSTANTIATE_TEST_SUITE_P(
    Workloads, MapOrderBookModelTest,
    ::testing::Values(
        // Narrow price band: almost every order crosses, lots of partial fills.
        WorkloadParams{"Aggressive", 1, 20'000, 98, 102, 50, 20},
        // Wide band: a deep book with many levels, few trades.
        WorkloadParams{"DeepBook", 2, 20'000, 1, 1'000, 100, 20},
        // Cancel-heavy, like real markets where most orders are canceled.
        WorkloadParams{"CancelHeavy", 3, 20'000, 95, 105, 20, 70},
        // Tiny quantities: many exact fills and empty levels.
        WorkloadParams{"SmallQty", 4, 20'000, 99, 101, 3, 30},
        // Negative prices.
        WorkloadParams{"NegativePrices", 5, 10'000, -10, 10, 30, 30}),
    [](const ::testing::TestParamInfo<WorkloadParams>& param_info) {
        return param_info.param.name;
    });

}  // namespace
}  // namespace litbook
