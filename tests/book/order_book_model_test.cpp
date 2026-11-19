// Model-based test: random operations go to the naive oracle (naive_book.hpp),
// MapOrderBook and FastOrderBook. All three must emit identical event streams and
// agree on every query; both real books' invariants must hold after every operation.

#include "litbook/book/book_config.hpp"
#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <variant>
#include <vector>

#include "book/book_test_support.hpp"
#include "book/naive_book.hpp"
#include "book/workload.hpp"

namespace litbook {
namespace {

using testing::WorkloadParams;

// Counts what a workload actually exercised, so a generator change that silently stops
// producing (say) self-trades makes the test fail instead of passing vacuously.
struct Coverage {
    int trades = 0;
    int resting_adds = 0;
    int user_cancels = 0;
    int unfilled_cancels = 0;  // IOC / FOK / market remainders
    int stp_cancels = 0;
    int stp_partial = 0;  // Decrement policy leaving quantity behind
    int book_full = 0;
    int kept_priority = 0;
    int lost_priority = 0;
    std::array<int, kRejectReasonCount> rejects{};

    void count(const BookEvent& event) {
        if (std::holds_alternative<Trade>(event)) {
            ++trades;
        } else if (const auto* c = std::get_if<OrderCanceled>(&event)) {
            switch (c->reason) {
                case CancelReason::UserRequested:
                    ++user_cancels;
                    break;
                case CancelReason::Unfilled:
                    ++unfilled_cancels;
                    break;
                case CancelReason::SelfTradePrevention:
                    ++stp_cancels;
                    stp_partial += c->remaining_qty > 0 ? 1 : 0;
                    break;
                case CancelReason::BookFull:
                    ++book_full;
                    break;
            }
        } else if (const auto* m = std::get_if<OrderModified>(&event)) {
            ++(m->kept_priority ? kept_priority : lost_priority);
        } else if (const auto* r = std::get_if<OrderRejected>(&event)) {
            ++rejects[static_cast<std::size_t>(r->reason)];
        } else if (const auto* mr = std::get_if<ModifyRejected>(&event)) {
            ++rejects[static_cast<std::size_t>(mr->reason)];
        } else if (const auto* cr = std::get_if<CancelRejected>(&event)) {
            ++rejects[static_cast<std::size_t>(cr->reason)];
        } else if (const auto* l = std::get_if<LevelUpdate>(&event)) {
            resting_adds += l->order_count > 0 ? 1 : 0;
        }
    }
    [[nodiscard]] int rejected(RejectReason reason) const {
        return rejects[static_cast<std::size_t>(reason)];
    }
};

class OrderBookModelTest : public ::testing::TestWithParam<WorkloadParams> {};

TEST_P(OrderBookModelTest, AllBooksMatchTheNaiveOracle) {
    const WorkloadParams& p = GetParam();
    const BookConfig config = testing::book_config(p);
    const std::vector<testing::Operation> ops = testing::generate(p);

    RecordingListener naive_rec;
    RecordingListener map_rec;
    RecordingListener fast_rec;
    testing::NaiveBook naive(naive_rec, config);
    MapOrderBook map_book(map_rec, config);
    FastOrderBook fast_book(fast_rec, config);
    Coverage cov;

    for (std::size_t op = 0; op < ops.size(); ++op) {
        testing::apply(naive, ops[op]);
        testing::apply(map_book, ops[op]);
        testing::apply(fast_book, ops[op]);

        ASSERT_EQ(map_rec.events(), naive_rec.events()) << "map diverged at operation " << op;
        ASSERT_EQ(fast_rec.events(), naive_rec.events()) << "fast diverged at operation " << op;
        ASSERT_TRUE(map_book.check_invariants()) << "map invariant broken at operation " << op;
        ASSERT_TRUE(fast_book.check_invariants()) << "fast invariant broken at operation " << op;
        ASSERT_EQ(map_book.best_bid(), naive.best(Side::Buy)) << "operation " << op;
        ASSERT_EQ(map_book.best_ask(), naive.best(Side::Sell)) << "operation " << op;
        ASSERT_EQ(fast_book.best_bid(), map_book.best_bid()) << "operation " << op;
        ASSERT_EQ(fast_book.best_ask(), map_book.best_ask()) << "operation " << op;
        for (const Side side : {Side::Buy, Side::Sell}) {
            ASSERT_EQ(fast_book.depth(side, 3), map_book.depth(side, 3)) << "operation " << op;
            ASSERT_EQ(fast_book.level_count(side), map_book.level_count(side))
                << "operation " << op;
        }
        ASSERT_EQ(map_book.order_count(), naive.order_count()) << "operation " << op;
        ASSERT_EQ(fast_book.order_count(), naive.order_count()) << "operation " << op;
        for (const BookEvent& e : naive_rec.events()) {
            cov.count(e);
        }
        naive_rec.clear();
        map_rec.clear();
        fast_rec.clear();
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
    if (config.min_price > p.min_price || config.max_price < p.max_price) {
        EXPECT_GT(cov.rejected(RejectReason::PriceOutOfRange), 0);
    } else {
        EXPECT_EQ(cov.rejected(RejectReason::PriceOutOfRange), 0);
    }
    if (p.config.max_orders <= 50) {
        EXPECT_GT(cov.book_full, 10);
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
    RecordProperty("book_full", cov.book_full);
}

INSTANTIATE_TEST_SUITE_P(Workloads, OrderBookModelTest,
                         ::testing::ValuesIn(testing::standard_workloads()),
                         [](const ::testing::TestParamInfo<WorkloadParams>& param_info) {
                             return param_info.param.name;
                         });

}  // namespace
}  // namespace litbook
