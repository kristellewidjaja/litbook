#include "litbook/book/map_order_book.hpp"

#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace litbook {
namespace {

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;

// Fixture: a book wired to a recorder. Every test ends by checking the invariants.
class MapOrderBookTest : public ::testing::Test {
protected:
    void TearDown() override { EXPECT_TRUE(book_.check_invariants()); }

    // Runs an operation and returns exactly the events it produced.
    std::vector<BookEvent> add(OrderId id, Side side, Price price, Qty qty) {
        rec_.clear();
        book_.add_limit(id, side, price, qty);
        EXPECT_TRUE(book_.check_invariants());
        return rec_.take();
    }
    std::vector<BookEvent> cancel(OrderId id) {
        rec_.clear();
        book_.cancel(id);
        EXPECT_TRUE(book_.check_invariants());
        return rec_.take();
    }

    static BookEvent accepted(OrderId id, Side side, Price price, Qty qty) {
        return OrderAccepted{.order_id = id, .side = side, .price = price, .qty = qty};
    }
    static BookEvent level(Side side, Price price, AggregateQty total, std::uint32_t count) {
        return LevelUpdate{.side = side, .price = price, .total_qty = total, .order_count = count};
    }
    static BookEvent trade(MatchId match, OrderId aggr, OrderId rest, Side aggr_side, Price price,
                           Qty qty, Qty aggr_left, Qty rest_left) {
        return Trade{.match_id = match,
                     .aggressor_id = aggr,
                     .resting_id = rest,
                     .aggressor_side = aggr_side,
                     .price = price,
                     .qty = qty,
                     .aggressor_remaining = aggr_left,
                     .resting_remaining = rest_left};
    }

    RecordingListener rec_;
    MapOrderBook book_{rec_};
};

using Events = std::vector<BookEvent>;

// ---- Empty book -------------------------------------------------------------

TEST_F(MapOrderBookTest, EmptyBookHasNoBestPrices) {
    EXPECT_EQ(book_.best_bid(), std::nullopt);
    EXPECT_EQ(book_.best_ask(), std::nullopt);
    EXPECT_EQ(book_.order_count(), 0U);
    EXPECT_EQ(book_.level_count(kBuy), 0U);
    EXPECT_EQ(book_.level_count(kSell), 0U);
    EXPECT_EQ(book_.volume_at(kBuy, 100), 0U);
    EXPECT_TRUE(book_.depth(kBuy, 10).empty());
}

// ---- Resting without trading ------------------------------------------------

TEST_F(MapOrderBookTest, BuyRestsWhenBookIsEmpty) {
    EXPECT_EQ(add(1, kBuy, 100, 10), (Events{accepted(1, kBuy, 100, 10), level(kBuy, 100, 10, 1)}));
    EXPECT_EQ(book_.best_bid(), 100);
    EXPECT_EQ(book_.best_ask(), std::nullopt);
    EXPECT_TRUE(book_.contains(1));
}

TEST_F(MapOrderBookTest, SellRestsWhenBookIsEmpty) {
    EXPECT_EQ(add(1, kSell, 100, 10),
              (Events{accepted(1, kSell, 100, 10), level(kSell, 100, 10, 1)}));
    EXPECT_EQ(book_.best_ask(), 100);
}

TEST_F(MapOrderBookTest, NonCrossingOrdersBothRest) {
    add(1, kBuy, 99, 10);
    EXPECT_EQ(add(2, kSell, 101, 5), (Events{accepted(2, kSell, 101, 5), level(kSell, 101, 5, 1)}));
    EXPECT_EQ(book_.best_bid(), 99);
    EXPECT_EQ(book_.best_ask(), 101);
    EXPECT_EQ(book_.order_count(), 2U);
}

TEST_F(MapOrderBookTest, BestBidIsHighestAndBestAskIsLowest) {
    add(1, kBuy, 98, 1);
    add(2, kBuy, 100, 1);
    add(3, kBuy, 99, 1);
    add(4, kSell, 105, 1);
    add(5, kSell, 103, 1);
    add(6, kSell, 104, 1);
    EXPECT_EQ(book_.best_bid(), 100);
    EXPECT_EQ(book_.best_ask(), 103);
}

TEST_F(MapOrderBookTest, SamePriceOrdersAggregateIntoOneLevel) {
    add(1, kBuy, 100, 10);
    EXPECT_EQ(add(2, kBuy, 100, 5), (Events{accepted(2, kBuy, 100, 5), level(kBuy, 100, 15, 2)}));
    EXPECT_EQ(book_.level_count(kBuy), 1U);
    EXPECT_EQ(book_.volume_at(kBuy, 100), 15U);
}

TEST_F(MapOrderBookTest, DepthIsBestFirstAndRespectsMaxLevels) {
    add(1, kBuy, 98, 3);
    add(2, kBuy, 100, 1);
    add(3, kBuy, 100, 2);
    add(4, kBuy, 99, 7);
    EXPECT_EQ(book_.depth(kBuy, 10),
              (std::vector<LevelSummary>{{.price = 100, .total_qty = 3, .order_count = 2},
                                         {.price = 99, .total_qty = 7, .order_count = 1},
                                         {.price = 98, .total_qty = 3, .order_count = 1}}));
    EXPECT_EQ(book_.depth(kBuy, 1).size(), 1U);
    EXPECT_TRUE(book_.depth(kBuy, 0).empty());

    add(5, kSell, 103, 4);
    add(6, kSell, 101, 6);
    EXPECT_EQ(book_.depth(kSell, 10),
              (std::vector<LevelSummary>{{.price = 101, .total_qty = 6, .order_count = 1},
                                         {.price = 103, .total_qty = 4, .order_count = 1}}));
}

// ---- Matching ---------------------------------------------------------------

TEST_F(MapOrderBookTest, ExactCrossFillsBothCompletely) {
    add(1, kSell, 100, 10);
    EXPECT_EQ(add(2, kBuy, 100, 10),
              (Events{accepted(2, kBuy, 100, 10), trade(1, 2, 1, kBuy, 100, 10, 0, 0),
                      level(kSell, 100, 0, 0)}));
    EXPECT_EQ(book_.order_count(), 0U);
    EXPECT_FALSE(book_.contains(1));
    EXPECT_FALSE(book_.contains(2));
    EXPECT_EQ(book_.best_ask(), std::nullopt);
}

TEST_F(MapOrderBookTest, AggressorPartiallyFillsAndRestsRemainder) {
    add(1, kSell, 100, 4);
    EXPECT_EQ(add(2, kBuy, 100, 10),
              (Events{accepted(2, kBuy, 100, 10), trade(1, 2, 1, kBuy, 100, 4, 6, 0),
                      level(kSell, 100, 0, 0), level(kBuy, 100, 6, 1)}));
    EXPECT_EQ(book_.best_bid(), 100);
    EXPECT_EQ(book_.best_ask(), std::nullopt);
    EXPECT_EQ(book_.volume_at(kBuy, 100), 6U);
}

TEST_F(MapOrderBookTest, RestingOrderPartiallyFilledKeepsItsPlace) {
    add(1, kSell, 100, 10);
    add(2, kSell, 100, 5);
    EXPECT_EQ(add(3, kBuy, 100, 4),
              (Events{accepted(3, kBuy, 100, 4), trade(1, 3, 1, kBuy, 100, 4, 0, 6),
                      level(kSell, 100, 11, 2)}));
    // Order 1 still has 6 left and is still first in line.
    EXPECT_EQ(add(4, kBuy, 100, 6),
              (Events{accepted(4, kBuy, 100, 6), trade(2, 4, 1, kBuy, 100, 6, 0, 0),
                      level(kSell, 100, 5, 1)}));
}

TEST_F(MapOrderBookTest, SellAggressorTradesAgainstBids) {
    add(1, kBuy, 100, 5);
    EXPECT_EQ(add(2, kSell, 100, 3),
              (Events{accepted(2, kSell, 100, 3), trade(1, 2, 1, kSell, 100, 3, 0, 2),
                      level(kBuy, 100, 2, 1)}));
}

TEST_F(MapOrderBookTest, TradesHappenAtTheRestingPrice) {
    add(1, kSell, 100, 5);
    // A buyer willing to pay 105 gets the resting seller's 100 (price improvement).
    const Events events = add(2, kBuy, 105, 5);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(std::get<Trade>(events[1]).price, 100);

    add(3, kBuy, 90, 5);
    const Events sell_events = add(4, kSell, 80, 5);
    EXPECT_EQ(std::get<Trade>(sell_events[1]).price, 90);
}

TEST_F(MapOrderBookTest, AggressorSweepsMultipleLevelsBestPriceFirst) {
    add(1, kSell, 102, 5);
    add(2, kSell, 100, 5);
    add(3, kSell, 101, 5);
    EXPECT_EQ(add(4, kBuy, 102, 12),
              (Events{accepted(4, kBuy, 102, 12), trade(1, 4, 2, kBuy, 100, 5, 7, 0),
                      level(kSell, 100, 0, 0), trade(2, 4, 3, kBuy, 101, 5, 2, 0),
                      level(kSell, 101, 0, 0), trade(3, 4, 1, kBuy, 102, 2, 0, 3),
                      level(kSell, 102, 3, 1)}));
    EXPECT_EQ(book_.best_ask(), 102);
    EXPECT_EQ(book_.best_bid(), std::nullopt);
}

TEST_F(MapOrderBookTest, AggressorStopsAtItsLimitPrice) {
    add(1, kSell, 100, 5);
    add(2, kSell, 101, 5);
    add(3, kSell, 103, 5);
    EXPECT_EQ(add(4, kBuy, 101, 20),
              (Events{accepted(4, kBuy, 101, 20), trade(1, 4, 1, kBuy, 100, 5, 15, 0),
                      level(kSell, 100, 0, 0), trade(2, 4, 2, kBuy, 101, 5, 10, 0),
                      level(kSell, 101, 0, 0), level(kBuy, 101, 10, 1)}));
    EXPECT_EQ(book_.best_bid(), 101);
    EXPECT_EQ(book_.best_ask(), 103);  // never traded through its limit
}

TEST_F(MapOrderBookTest, SellSweepsBidsHighestFirst) {
    add(1, kBuy, 98, 1);
    add(2, kBuy, 100, 1);
    add(3, kBuy, 99, 1);
    const Events events = add(4, kSell, 98, 3);
    ASSERT_EQ(events.size(), 7U);
    EXPECT_EQ(std::get<Trade>(events[1]).price, 100);
    EXPECT_EQ(std::get<Trade>(events[3]).price, 99);
    EXPECT_EQ(std::get<Trade>(events[5]).price, 98);
}

// ---- Time priority ----------------------------------------------------------

TEST_F(MapOrderBookTest, OrdersAtOnePriceFillInArrivalOrder) {
    add(10, kSell, 100, 1);
    add(11, kSell, 100, 1);
    add(12, kSell, 100, 1);
    const Events events = add(20, kBuy, 100, 3);
    ASSERT_EQ(events.size(), 7U);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 10U);
    EXPECT_EQ(std::get<Trade>(events[3]).resting_id, 11U);
    EXPECT_EQ(std::get<Trade>(events[5]).resting_id, 12U);
}

TEST_F(MapOrderBookTest, BetterPriceBeatsEarlierArrival) {
    add(1, kSell, 101, 5);  // earlier, worse price
    add(2, kSell, 100, 5);  // later, better price
    const Events events = add(3, kBuy, 101, 5);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 2U);
}

TEST_F(MapOrderBookTest, CancelInMiddleOfQueuePreservesOthersOrder) {
    add(1, kSell, 100, 1);
    add(2, kSell, 100, 1);
    add(3, kSell, 100, 1);
    cancel(2);
    const Events events = add(4, kBuy, 100, 2);
    ASSERT_EQ(events.size(), 5U);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 1U);
    EXPECT_EQ(std::get<Trade>(events[3]).resting_id, 3U);
}

TEST_F(MapOrderBookTest, MatchIdsIncreaseAcrossCalls) {
    add(1, kSell, 100, 2);
    add(2, kSell, 101, 2);
    const Events a = add(3, kBuy, 101, 3);
    const Events b = add(4, kBuy, 101, 1);
    EXPECT_EQ(std::get<Trade>(a[1]).match_id, 1U);
    EXPECT_EQ(std::get<Trade>(a[3]).match_id, 2U);
    EXPECT_EQ(std::get<Trade>(b[1]).match_id, 3U);
}

// ---- Cancels ----------------------------------------------------------------

TEST_F(MapOrderBookTest, CancelRemovesRestingOrder) {
    add(1, kBuy, 100, 10);
    EXPECT_EQ(cancel(1),
              (Events{OrderCanceled{.order_id = 1, .canceled_qty = 10}, level(kBuy, 100, 0, 0)}));
    EXPECT_FALSE(book_.contains(1));
    EXPECT_EQ(book_.best_bid(), std::nullopt);
}

TEST_F(MapOrderBookTest, CancelLeavesOtherOrdersAtTheLevel) {
    add(1, kSell, 100, 10);
    add(2, kSell, 100, 3);
    EXPECT_EQ(cancel(1),
              (Events{OrderCanceled{.order_id = 1, .canceled_qty = 10}, level(kSell, 100, 3, 1)}));
    EXPECT_EQ(book_.best_ask(), 100);
}

TEST_F(MapOrderBookTest, CancelOfPartiallyFilledOrderCancelsOnlyTheRemainder) {
    add(1, kSell, 100, 10);
    add(2, kBuy, 100, 4);
    EXPECT_EQ(cancel(1),
              (Events{OrderCanceled{.order_id = 1, .canceled_qty = 6}, level(kSell, 100, 0, 0)}));
}

TEST_F(MapOrderBookTest, CancelOfUnknownIdIsRejected) {
    EXPECT_EQ(cancel(42),
              (Events{CancelRejected{.order_id = 42, .reason = RejectReason::UnknownOrderId}}));
}

TEST_F(MapOrderBookTest, CancelTwiceRejectsTheSecond) {
    add(1, kBuy, 100, 1);
    cancel(1);
    EXPECT_EQ(cancel(1),
              (Events{CancelRejected{.order_id = 1, .reason = RejectReason::UnknownOrderId}}));
}

TEST_F(MapOrderBookTest, CancelOfFullyFilledOrderIsRejected) {
    add(1, kSell, 100, 5);
    add(2, kBuy, 100, 5);
    EXPECT_EQ(cancel(1),
              (Events{CancelRejected{.order_id = 1, .reason = RejectReason::UnknownOrderId}}));
    EXPECT_EQ(cancel(2),
              (Events{CancelRejected{.order_id = 2, .reason = RejectReason::UnknownOrderId}}));
}

TEST_F(MapOrderBookTest, CancelBestLevelExposesNextLevel) {
    add(1, kBuy, 100, 1);
    add(2, kBuy, 99, 1);
    cancel(1);
    EXPECT_EQ(book_.best_bid(), 99);
}

// ---- Rejects ----------------------------------------------------------------

TEST_F(MapOrderBookTest, ZeroQuantityIsRejected) {
    EXPECT_EQ(add(1, kBuy, 100, 0),
              (Events{OrderRejected{.order_id = 1, .reason = RejectReason::ZeroQuantity}}));
    EXPECT_FALSE(book_.contains(1));
}

TEST_F(MapOrderBookTest, DuplicateRestingIdIsRejectedAndBookUnchanged) {
    add(1, kBuy, 100, 10);
    EXPECT_EQ(add(1, kSell, 100, 5),
              (Events{OrderRejected{.order_id = 1, .reason = RejectReason::DuplicateOrderId}}));
    EXPECT_EQ(book_.volume_at(kBuy, 100), 10U);
    EXPECT_EQ(book_.order_count(), 1U);
}

TEST_F(MapOrderBookTest, IdOfFilledOrderMayBeReused) {
    // Documented behavior: the book only guarantees uniqueness among *resting* orders.
    // Session-level uniqueness is the gateway's job (Phase 6).
    add(1, kSell, 100, 5);
    add(2, kBuy, 100, 5);
    EXPECT_EQ(add(1, kBuy, 99, 1), (Events{accepted(1, kBuy, 99, 1), level(kBuy, 99, 1, 1)}));
}

// ---- Numeric edges ----------------------------------------------------------

TEST_F(MapOrderBookTest, NegativePricesWork) {
    add(1, kBuy, -5, 1);
    add(2, kBuy, -3, 1);
    add(3, kSell, -1, 1);
    EXPECT_EQ(book_.best_bid(), -3);
    EXPECT_EQ(book_.best_ask(), -1);
    // A sell limited at -4 trades with the -3 bid but not the -5 bid (-5 < -4).
    const Events limited = add(4, kSell, -4, 2);
    ASSERT_EQ(limited.size(), 4U);
    EXPECT_EQ(std::get<Trade>(limited[1]).price, -3);
    EXPECT_EQ(limited[3], level(kSell, -4, 1, 1));
    // A sell limited at -5 reaches the -5 bid.
    const Events deeper = add(5, kSell, -5, 1);
    EXPECT_EQ(std::get<Trade>(deeper[1]).price, -5);
}

TEST_F(MapOrderBookTest, ExtremePricesWork) {
    constexpr Price kMin = std::numeric_limits<Price>::min();
    constexpr Price kMax = std::numeric_limits<Price>::max();
    add(1, kBuy, kMin, 1);
    add(2, kSell, kMax, 1);
    EXPECT_EQ(book_.best_bid(), kMin);
    EXPECT_EQ(book_.best_ask(), kMax);
    const Events events = add(3, kBuy, kMax, 1);  // market-like buy
    EXPECT_EQ(std::get<Trade>(events[1]).price, kMax);
}

TEST_F(MapOrderBookTest, LevelTotalDoesNotOverflowWithManyMaxSizeOrders) {
    constexpr Qty kMaxQty = std::numeric_limits<Qty>::max();
    for (OrderId id = 1; id <= 4; ++id) {
        add(id, kBuy, 100, kMaxQty);
    }
    EXPECT_EQ(book_.volume_at(kBuy, 100), AggregateQty{kMaxQty} * 4);
}

}  // namespace
}  // namespace litbook
