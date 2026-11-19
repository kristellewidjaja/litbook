// Phase 1 scenarios: limit orders, price-time priority, cancels. Typed tests: every
// scenario runs against every book implementation (BookTypes).

#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <variant>
#include <vector>

#include "book/book_test_support.hpp"

namespace litbook {
namespace {

using namespace testing;  // NOLINT(google-build-using-namespace): event builders

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;

// Fixture: a book wired to a recorder. Every test ends by checking the invariants.
template <class Book>
class OrderBookTest : public ::testing::Test {
protected:
    void TearDown() override { EXPECT_TRUE(book_.check_invariants()); }

    // Runs an operation and returns exactly the events it produced.
    std::vector<BookEvent> add(OrderId id, Side side, Price price, Qty qty) {
        rec_.clear();
        book_.submit({.id = id, .side = side, .price = price, .qty = qty});
        EXPECT_TRUE(book_.check_invariants());
        return rec_.take();
    }
    std::vector<BookEvent> cancel(OrderId id) {
        rec_.clear();
        book_.cancel(id);
        EXPECT_TRUE(book_.check_invariants());
        return rec_.take();
    }

    RecordingListener rec_;
    Book book_{rec_, with_test_limits({})};
};

TYPED_TEST_SUITE(OrderBookTest, BookTypes, BookTypeNames);

using Events = std::vector<BookEvent>;

// ---- Empty book -------------------------------------------------------------

TYPED_TEST(OrderBookTest, EmptyBookHasNoBestPrices) {
    EXPECT_EQ(this->book_.best_bid(), std::nullopt);
    EXPECT_EQ(this->book_.best_ask(), std::nullopt);
    EXPECT_EQ(this->book_.order_count(), 0U);
    EXPECT_EQ(this->book_.level_count(kBuy), 0U);
    EXPECT_EQ(this->book_.level_count(kSell), 0U);
    EXPECT_EQ(this->book_.volume_at(kBuy, 100), 0U);
    EXPECT_TRUE(this->book_.depth(kBuy, 10).empty());
}

// ---- Resting without trading ------------------------------------------------

TYPED_TEST(OrderBookTest, BuyRestsWhenBookIsEmpty) {
    EXPECT_EQ(this->add(1, kBuy, 100, 10),
              (Events{accepted(1, kBuy, 100, 10), level(kBuy, 100, 10, 1)}));
    EXPECT_EQ(this->book_.best_bid(), 100);
    EXPECT_EQ(this->book_.best_ask(), std::nullopt);
    EXPECT_TRUE(this->book_.contains(1));
}

TYPED_TEST(OrderBookTest, SellRestsWhenBookIsEmpty) {
    EXPECT_EQ(this->add(1, kSell, 100, 10),
              (Events{accepted(1, kSell, 100, 10), level(kSell, 100, 10, 1)}));
    EXPECT_EQ(this->book_.best_ask(), 100);
}

TYPED_TEST(OrderBookTest, NonCrossingOrdersBothRest) {
    this->add(1, kBuy, 99, 10);
    EXPECT_EQ(this->add(2, kSell, 101, 5),
              (Events{accepted(2, kSell, 101, 5), level(kSell, 101, 5, 1)}));
    EXPECT_EQ(this->book_.best_bid(), 99);
    EXPECT_EQ(this->book_.best_ask(), 101);
    EXPECT_EQ(this->book_.order_count(), 2U);
}

TYPED_TEST(OrderBookTest, BestBidIsHighestAndBestAskIsLowest) {
    this->add(1, kBuy, 98, 1);
    this->add(2, kBuy, 100, 1);
    this->add(3, kBuy, 99, 1);
    this->add(4, kSell, 105, 1);
    this->add(5, kSell, 103, 1);
    this->add(6, kSell, 104, 1);
    EXPECT_EQ(this->book_.best_bid(), 100);
    EXPECT_EQ(this->book_.best_ask(), 103);
}

TYPED_TEST(OrderBookTest, SamePriceOrdersAggregateIntoOneLevel) {
    this->add(1, kBuy, 100, 10);
    EXPECT_EQ(this->add(2, kBuy, 100, 5),
              (Events{accepted(2, kBuy, 100, 5), level(kBuy, 100, 15, 2)}));
    EXPECT_EQ(this->book_.level_count(kBuy), 1U);
    EXPECT_EQ(this->book_.volume_at(kBuy, 100), 15U);
}

TYPED_TEST(OrderBookTest, DepthIsBestFirstAndRespectsMaxLevels) {
    this->add(1, kBuy, 98, 3);
    this->add(2, kBuy, 100, 1);
    this->add(3, kBuy, 100, 2);
    this->add(4, kBuy, 99, 7);
    EXPECT_EQ(this->book_.depth(kBuy, 10),
              (std::vector<LevelSummary>{{.price = 100, .total_qty = 3, .order_count = 2},
                                         {.price = 99, .total_qty = 7, .order_count = 1},
                                         {.price = 98, .total_qty = 3, .order_count = 1}}));
    EXPECT_EQ(this->book_.depth(kBuy, 1).size(), 1U);
    EXPECT_TRUE(this->book_.depth(kBuy, 0).empty());

    this->add(5, kSell, 103, 4);
    this->add(6, kSell, 101, 6);
    EXPECT_EQ(this->book_.depth(kSell, 10),
              (std::vector<LevelSummary>{{.price = 101, .total_qty = 6, .order_count = 1},
                                         {.price = 103, .total_qty = 4, .order_count = 1}}));
}

// ---- Matching ---------------------------------------------------------------

TYPED_TEST(OrderBookTest, ExactCrossFillsBothCompletely) {
    this->add(1, kSell, 100, 10);
    EXPECT_EQ(this->add(2, kBuy, 100, 10),
              (Events{accepted(2, kBuy, 100, 10), trade(1, 2, 1, kBuy, 100, 10, 0, 0),
                      level(kSell, 100, 0, 0)}));
    EXPECT_EQ(this->book_.order_count(), 0U);
    EXPECT_FALSE(this->book_.contains(1));
    EXPECT_FALSE(this->book_.contains(2));
    EXPECT_EQ(this->book_.best_ask(), std::nullopt);
}

TYPED_TEST(OrderBookTest, AggressorPartiallyFillsAndRestsRemainder) {
    this->add(1, kSell, 100, 4);
    EXPECT_EQ(this->add(2, kBuy, 100, 10),
              (Events{accepted(2, kBuy, 100, 10), trade(1, 2, 1, kBuy, 100, 4, 6, 0),
                      level(kSell, 100, 0, 0), level(kBuy, 100, 6, 1)}));
    EXPECT_EQ(this->book_.best_bid(), 100);
    EXPECT_EQ(this->book_.best_ask(), std::nullopt);
    EXPECT_EQ(this->book_.volume_at(kBuy, 100), 6U);
}

TYPED_TEST(OrderBookTest, RestingOrderPartiallyFilledKeepsItsPlace) {
    this->add(1, kSell, 100, 10);
    this->add(2, kSell, 100, 5);
    EXPECT_EQ(this->add(3, kBuy, 100, 4),
              (Events{accepted(3, kBuy, 100, 4), trade(1, 3, 1, kBuy, 100, 4, 0, 6),
                      level(kSell, 100, 11, 2)}));
    // Order 1 still has 6 left and is still first in line.
    EXPECT_EQ(this->add(4, kBuy, 100, 6),
              (Events{accepted(4, kBuy, 100, 6), trade(2, 4, 1, kBuy, 100, 6, 0, 0),
                      level(kSell, 100, 5, 1)}));
}

TYPED_TEST(OrderBookTest, SellAggressorTradesAgainstBids) {
    this->add(1, kBuy, 100, 5);
    EXPECT_EQ(this->add(2, kSell, 100, 3),
              (Events{accepted(2, kSell, 100, 3), trade(1, 2, 1, kSell, 100, 3, 0, 2),
                      level(kBuy, 100, 2, 1)}));
}

TYPED_TEST(OrderBookTest, TradesHappenAtTheRestingPrice) {
    this->add(1, kSell, 100, 5);
    // A buyer willing to pay 105 gets the resting seller's 100 (price improvement).
    const Events events = this->add(2, kBuy, 105, 5);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(std::get<Trade>(events[1]).price, 100);

    this->add(3, kBuy, 90, 5);
    const Events sell_events = this->add(4, kSell, 80, 5);
    EXPECT_EQ(std::get<Trade>(sell_events[1]).price, 90);
}

TYPED_TEST(OrderBookTest, AggressorSweepsMultipleLevelsBestPriceFirst) {
    this->add(1, kSell, 102, 5);
    this->add(2, kSell, 100, 5);
    this->add(3, kSell, 101, 5);
    EXPECT_EQ(this->add(4, kBuy, 102, 12),
              (Events{accepted(4, kBuy, 102, 12), trade(1, 4, 2, kBuy, 100, 5, 7, 0),
                      level(kSell, 100, 0, 0), trade(2, 4, 3, kBuy, 101, 5, 2, 0),
                      level(kSell, 101, 0, 0), trade(3, 4, 1, kBuy, 102, 2, 0, 3),
                      level(kSell, 102, 3, 1)}));
    EXPECT_EQ(this->book_.best_ask(), 102);
    EXPECT_EQ(this->book_.best_bid(), std::nullopt);
}

TYPED_TEST(OrderBookTest, AggressorStopsAtItsLimitPrice) {
    this->add(1, kSell, 100, 5);
    this->add(2, kSell, 101, 5);
    this->add(3, kSell, 103, 5);
    EXPECT_EQ(this->add(4, kBuy, 101, 20),
              (Events{accepted(4, kBuy, 101, 20), trade(1, 4, 1, kBuy, 100, 5, 15, 0),
                      level(kSell, 100, 0, 0), trade(2, 4, 2, kBuy, 101, 5, 10, 0),
                      level(kSell, 101, 0, 0), level(kBuy, 101, 10, 1)}));
    EXPECT_EQ(this->book_.best_bid(), 101);
    EXPECT_EQ(this->book_.best_ask(), 103);  // never traded through its limit
}

TYPED_TEST(OrderBookTest, SellSweepsBidsHighestFirst) {
    this->add(1, kBuy, 98, 1);
    this->add(2, kBuy, 100, 1);
    this->add(3, kBuy, 99, 1);
    const Events events = this->add(4, kSell, 98, 3);
    ASSERT_EQ(events.size(), 7U);
    EXPECT_EQ(std::get<Trade>(events[1]).price, 100);
    EXPECT_EQ(std::get<Trade>(events[3]).price, 99);
    EXPECT_EQ(std::get<Trade>(events[5]).price, 98);
}

// ---- Time priority ----------------------------------------------------------

TYPED_TEST(OrderBookTest, OrdersAtOnePriceFillInArrivalOrder) {
    this->add(10, kSell, 100, 1);
    this->add(11, kSell, 100, 1);
    this->add(12, kSell, 100, 1);
    const Events events = this->add(20, kBuy, 100, 3);
    ASSERT_EQ(events.size(), 7U);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 10U);
    EXPECT_EQ(std::get<Trade>(events[3]).resting_id, 11U);
    EXPECT_EQ(std::get<Trade>(events[5]).resting_id, 12U);
}

TYPED_TEST(OrderBookTest, BetterPriceBeatsEarlierArrival) {
    this->add(1, kSell, 101, 5);  // earlier, worse price
    this->add(2, kSell, 100, 5);  // later, better price
    const Events events = this->add(3, kBuy, 101, 5);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 2U);
}

TYPED_TEST(OrderBookTest, CancelInMiddleOfQueuePreservesOthersOrder) {
    this->add(1, kSell, 100, 1);
    this->add(2, kSell, 100, 1);
    this->add(3, kSell, 100, 1);
    this->cancel(2);
    const Events events = this->add(4, kBuy, 100, 2);
    ASSERT_EQ(events.size(), 5U);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 1U);
    EXPECT_EQ(std::get<Trade>(events[3]).resting_id, 3U);
}

TYPED_TEST(OrderBookTest, MatchIdsIncreaseAcrossCalls) {
    this->add(1, kSell, 100, 2);
    this->add(2, kSell, 101, 2);
    const Events a = this->add(3, kBuy, 101, 3);
    const Events b = this->add(4, kBuy, 101, 1);
    EXPECT_EQ(std::get<Trade>(a[1]).match_id, 1U);
    EXPECT_EQ(std::get<Trade>(a[3]).match_id, 2U);
    EXPECT_EQ(std::get<Trade>(b[1]).match_id, 3U);
}

// ---- Cancels ----------------------------------------------------------------

TYPED_TEST(OrderBookTest, CancelRemovesRestingOrder) {
    this->add(1, kBuy, 100, 10);
    EXPECT_EQ(this->cancel(1), (Events{OrderCanceled{.order_id = 1,
                                                     .canceled_qty = 10,
                                                     .remaining_qty = 0,
                                                     .reason = CancelReason::UserRequested},
                                       level(kBuy, 100, 0, 0)}));
    EXPECT_FALSE(this->book_.contains(1));
    EXPECT_EQ(this->book_.best_bid(), std::nullopt);
}

TYPED_TEST(OrderBookTest, CancelLeavesOtherOrdersAtTheLevel) {
    this->add(1, kSell, 100, 10);
    this->add(2, kSell, 100, 3);
    EXPECT_EQ(this->cancel(1), (Events{OrderCanceled{.order_id = 1,
                                                     .canceled_qty = 10,
                                                     .remaining_qty = 0,
                                                     .reason = CancelReason::UserRequested},
                                       level(kSell, 100, 3, 1)}));
    EXPECT_EQ(this->book_.best_ask(), 100);
}

TYPED_TEST(OrderBookTest, CancelOfPartiallyFilledOrderCancelsOnlyTheRemainder) {
    this->add(1, kSell, 100, 10);
    this->add(2, kBuy, 100, 4);
    EXPECT_EQ(this->cancel(1), (Events{OrderCanceled{.order_id = 1,
                                                     .canceled_qty = 6,
                                                     .remaining_qty = 0,
                                                     .reason = CancelReason::UserRequested},
                                       level(kSell, 100, 0, 0)}));
}

TYPED_TEST(OrderBookTest, CancelOfUnknownIdIsRejected) {
    EXPECT_EQ(this->cancel(42),
              (Events{CancelRejected{.order_id = 42, .reason = RejectReason::UnknownOrderId}}));
}

TYPED_TEST(OrderBookTest, CancelTwiceRejectsTheSecond) {
    this->add(1, kBuy, 100, 1);
    this->cancel(1);
    EXPECT_EQ(this->cancel(1),
              (Events{CancelRejected{.order_id = 1, .reason = RejectReason::UnknownOrderId}}));
}

TYPED_TEST(OrderBookTest, CancelOfFullyFilledOrderIsRejected) {
    this->add(1, kSell, 100, 5);
    this->add(2, kBuy, 100, 5);
    EXPECT_EQ(this->cancel(1),
              (Events{CancelRejected{.order_id = 1, .reason = RejectReason::UnknownOrderId}}));
    EXPECT_EQ(this->cancel(2),
              (Events{CancelRejected{.order_id = 2, .reason = RejectReason::UnknownOrderId}}));
}

TYPED_TEST(OrderBookTest, CancelBestLevelExposesNextLevel) {
    this->add(1, kBuy, 100, 1);
    this->add(2, kBuy, 99, 1);
    this->cancel(1);
    EXPECT_EQ(this->book_.best_bid(), 99);
}

// ---- Rejects ----------------------------------------------------------------

TYPED_TEST(OrderBookTest, ZeroQuantityIsRejected) {
    EXPECT_EQ(this->add(1, kBuy, 100, 0),
              (Events{OrderRejected{.order_id = 1, .reason = RejectReason::ZeroQuantity}}));
    EXPECT_FALSE(this->book_.contains(1));
}

TYPED_TEST(OrderBookTest, DuplicateRestingIdIsRejectedAndBookUnchanged) {
    this->add(1, kBuy, 100, 10);
    EXPECT_EQ(this->add(1, kSell, 100, 5),
              (Events{OrderRejected{.order_id = 1, .reason = RejectReason::DuplicateOrderId}}));
    EXPECT_EQ(this->book_.volume_at(kBuy, 100), 10U);
    EXPECT_EQ(this->book_.order_count(), 1U);
}

TYPED_TEST(OrderBookTest, IdOfFilledOrderMayBeReused) {
    // Documented behavior: the book only guarantees uniqueness among *resting* orders.
    // Session-level uniqueness is the gateway's job (Phase 6).
    this->add(1, kSell, 100, 5);
    this->add(2, kBuy, 100, 5);
    EXPECT_EQ(this->add(1, kBuy, 99, 1), (Events{accepted(1, kBuy, 99, 1), level(kBuy, 99, 1, 1)}));
}

// ---- Numeric edges ----------------------------------------------------------

TYPED_TEST(OrderBookTest, NegativePricesWork) {
    this->add(1, kBuy, -5, 1);
    this->add(2, kBuy, -3, 1);
    this->add(3, kSell, -1, 1);
    EXPECT_EQ(this->book_.best_bid(), -3);
    EXPECT_EQ(this->book_.best_ask(), -1);
    // A sell limited at -4 trades with the -3 bid but not the -5 bid (-5 < -4).
    const Events limited = this->add(4, kSell, -4, 2);
    ASSERT_EQ(limited.size(), 4U);
    EXPECT_EQ(std::get<Trade>(limited[1]).price, -3);
    EXPECT_EQ(limited[3], level(kSell, -4, 1, 1));
    // A sell limited at -5 reaches the -5 bid.
    const Events deeper = this->add(5, kSell, -5, 1);
    EXPECT_EQ(std::get<Trade>(deeper[1]).price, -5);
}

TYPED_TEST(OrderBookTest, LevelTotalDoesNotOverflowWithManyMaxSizeOrders) {
    constexpr Qty kMaxQty = std::numeric_limits<Qty>::max();
    for (OrderId id = 1; id <= 4; ++id) {
        this->add(id, kBuy, 100, kMaxQty);
    }
    EXPECT_EQ(this->book_.volume_at(kBuy, 100), AggregateQty{kMaxQty} * 4);
}

}  // namespace
}  // namespace litbook
