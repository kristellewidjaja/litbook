// Phase 2 rules: market / IOC / FOK / post-only, modify, self-trade prevention,
// tick and lot validation. Each test asserts the exact event stream.

#include "litbook/book/book_config.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace litbook {
namespace {

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;
constexpr auto kIoc = TimeInForce::ImmediateOrCancel;
constexpr auto kFok = TimeInForce::FillOrKill;
using Events = std::vector<BookEvent>;

class RulesTest : public ::testing::Test {
protected:
    void use_config(BookConfig config) { book_ = MapOrderBook(rec_, config); }

    void TearDown() override { EXPECT_TRUE(book_.check_invariants()); }

    Events submit(OrderRequest r) {
        rec_.clear();
        book_.submit(r);
        EXPECT_TRUE(book_.check_invariants());
        return rec_.take();
    }
    // Plain resting limit order, no owner.
    Events limit(OrderId id, Side side, Price price, Qty qty, OwnerId owner = kNoOwner) {
        return submit({.id = id, .owner = owner, .side = side, .price = price, .qty = qty});
    }
    Events modify(OrderId id, Price price, Qty qty) {
        rec_.clear();
        book_.modify(id, price, qty);
        EXPECT_TRUE(book_.check_invariants());
        return rec_.take();
    }

    static BookEvent accepted(const OrderRequest& r) {
        return OrderAccepted{.order_id = r.id,
                             .owner = r.owner,
                             .side = r.side,
                             .type = r.type,
                             .tif = r.tif,
                             .post_only = r.post_only,
                             .price = r.type == OrderType::Market ? 0 : r.price,
                             .qty = r.qty};
    }
    static BookEvent rejected(OrderId id, RejectReason reason) {
        return OrderRejected{.order_id = id, .reason = reason};
    }
    static BookEvent canceled(OrderId id, Qty qty, Qty left, CancelReason reason) {
        return OrderCanceled{
            .order_id = id, .canceled_qty = qty, .remaining_qty = left, .reason = reason};
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
    static BookEvent modified(OrderId id, Side side, Price price, Qty qty, bool kept) {
        return OrderModified{
            .order_id = id, .side = side, .price = price, .qty = qty, .kept_priority = kept};
    }

    RecordingListener rec_;
    MapOrderBook book_{rec_};
};

// ---- Configuration and validation ---------------------------------------------

TEST(BookConfigTest, InvalidConfigThrowsAtConstruction) {
    RecordingListener rec;
    EXPECT_THROW(MapOrderBook(rec, BookConfig{.tick_size = 0}), std::invalid_argument);
    EXPECT_THROW(MapOrderBook(rec, BookConfig{.tick_size = -5}), std::invalid_argument);
    EXPECT_THROW(MapOrderBook(rec, BookConfig{.lot_size = 0}), std::invalid_argument);
}

TEST_F(RulesTest, PriceOffTickIsRejected) {
    use_config({.tick_size = 5});
    EXPECT_EQ(limit(1, kBuy, 102, 1), (Events{rejected(1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(limit(2, kBuy, -7, 1), (Events{rejected(2, RejectReason::InvalidPrice)}));
    EXPECT_EQ(limit(3, kBuy, 105, 1).size(), 2U);  // accepted + level
    EXPECT_EQ(limit(4, kBuy, -10, 1).size(), 2U);  // negative multiple of the tick is fine
}

TEST_F(RulesTest, QuantityNotAMultipleOfLotIsRejected) {
    use_config({.lot_size = 100});
    EXPECT_EQ(limit(1, kBuy, 100, 150), (Events{rejected(1, RejectReason::InvalidLotSize)}));
    EXPECT_EQ(limit(2, kBuy, 100, 99), (Events{rejected(2, RejectReason::InvalidLotSize)}));
    EXPECT_EQ(limit(3, kBuy, 100, 300).size(), 2U);
}

TEST_F(RulesTest, ValidationOrderIsFixed) {
    use_config({.tick_size = 5, .lot_size = 10});
    // Zero quantity is reported before an off-tick price, and off-tick before odd lot.
    EXPECT_EQ(limit(1, kBuy, 3, 0), (Events{rejected(1, RejectReason::ZeroQuantity)}));
    EXPECT_EQ(limit(2, kBuy, 3, 7), (Events{rejected(2, RejectReason::InvalidPrice)}));
}

TEST_F(RulesTest, ContradictoryOrderTypesAreRejected) {
    EXPECT_EQ(submit({.id = 1, .type = OrderType::Market, .post_only = true, .qty = 1}),
              (Events{rejected(1, RejectReason::InvalidOrderType)}));
    EXPECT_EQ(submit({.id = 2, .tif = kIoc, .post_only = true, .price = 100, .qty = 1}),
              (Events{rejected(2, RejectReason::InvalidOrderType)}));
    EXPECT_EQ(submit({.id = 3, .tif = kFok, .post_only = true, .price = 100, .qty = 1}),
              (Events{rejected(3, RejectReason::InvalidOrderType)}));
}

// ---- Market orders ----------------------------------------------------------

TEST_F(RulesTest, MarketOrderSweepsEveryLevelRegardlessOfPrice) {
    limit(1, kSell, 100, 2);
    limit(2, kSell, 5'000, 3);
    const OrderRequest r{.id = 3, .side = kBuy, .type = OrderType::Market, .qty = 5};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 2, 3, 0), level(kSell, 100, 0, 0),
                      trade(2, 3, 2, kBuy, 5'000, 3, 0, 0), level(kSell, 5'000, 0, 0)}));
}

TEST_F(RulesTest, MarketOrderRemainderIsCanceledNotRested) {
    limit(1, kBuy, 100, 2);
    const OrderRequest r{.id = 2, .side = kSell, .type = OrderType::Market, .qty = 5};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), trade(1, 2, 1, kSell, 100, 2, 3, 0), level(kBuy, 100, 0, 0),
                      canceled(2, 3, 0, CancelReason::Unfilled)}));
    EXPECT_FALSE(book_.contains(2));
}

TEST_F(RulesTest, MarketOrderIntoEmptyBookIsCanceled) {
    const OrderRequest r{.id = 1, .side = kBuy, .type = OrderType::Market, .qty = 5};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(1, 5, 0, CancelReason::Unfilled)}));
}

TEST_F(RulesTest, MarketOrderIgnoresPriceAndTick) {
    use_config({.tick_size = 5});
    limit(1, kSell, 100, 1);
    const OrderRequest r{
        .id = 2, .side = kBuy, .type = OrderType::Market, .price = 3, .qty = 1};  // off-tick
    const Events events = submit(r);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(std::get<OrderAccepted>(events[0]).price, 0);
}

// ---- IOC ------------------------------------------------------------------------

TEST_F(RulesTest, IocFillsWhatItCanAndCancelsTheRest) {
    limit(1, kSell, 100, 4);
    limit(2, kSell, 102, 4);  // beyond the IOC's limit
    const OrderRequest r{.id = 3, .side = kBuy, .tif = kIoc, .price = 101, .qty = 10};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 4, 6, 0), level(kSell, 100, 0, 0),
                      canceled(3, 6, 0, CancelReason::Unfilled)}));
    EXPECT_EQ(book_.best_bid(), std::nullopt);
    EXPECT_EQ(book_.best_ask(), 102);
}

TEST_F(RulesTest, IocWithNothingToTradeIsFullyCanceled) {
    limit(1, kSell, 105, 4);
    const OrderRequest r{.id = 2, .side = kBuy, .tif = kIoc, .price = 100, .qty = 3};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(2, 3, 0, CancelReason::Unfilled)}));
}

TEST_F(RulesTest, FullyFilledIocHasNoCancel) {
    limit(1, kSell, 100, 5);
    const OrderRequest r{.id = 2, .side = kBuy, .tif = kIoc, .price = 100, .qty = 5};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), trade(1, 2, 1, kBuy, 100, 5, 0, 0), level(kSell, 100, 0, 0)}));
}

// ---- FOK ------------------------------------------------------------------------

TEST_F(RulesTest, FokFillsAcrossLevelsWhenEnoughIsAvailable) {
    limit(1, kSell, 100, 3);
    limit(2, kSell, 101, 3);
    const OrderRequest r{.id = 3, .side = kBuy, .tif = kFok, .price = 101, .qty = 6};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 3, 3, 0), level(kSell, 100, 0, 0),
                      trade(2, 3, 2, kBuy, 101, 3, 0, 0), level(kSell, 101, 0, 0)}));
}

TEST_F(RulesTest, FokIsKilledWithoutTradingWhenShortByOne) {
    limit(1, kSell, 100, 3);
    limit(2, kSell, 101, 3);
    const OrderRequest r{.id = 3, .side = kBuy, .tif = kFok, .price = 101, .qty = 7};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(3, 7, 0, CancelReason::Unfilled)}));
    EXPECT_EQ(book_.volume_at(kSell, 100), 3U);  // untouched
    EXPECT_EQ(book_.volume_at(kSell, 101), 3U);
}

TEST_F(RulesTest, FokOnlyCountsLevelsWithinItsLimit) {
    limit(1, kSell, 100, 3);
    limit(2, kSell, 102, 10);
    const OrderRequest r{.id = 3, .side = kBuy, .tif = kFok, .price = 101, .qty = 4};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(3, 4, 0, CancelReason::Unfilled)}));
}

TEST_F(RulesTest, FokIsKilledIfAnOwnOrderIsInTheWay) {
    limit(1, kSell, 100, 1, /*owner=*/7);
    limit(2, kSell, 100, 10, /*owner=*/8);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .tif = kFok, .price = 100, .qty = 5};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(3, 5, 0, CancelReason::Unfilled)}));
}

TEST_F(RulesTest, FokFillsIfOwnOrderIsBeyondWhatItNeeds) {
    limit(1, kSell, 100, 10, /*owner=*/8);
    limit(2, kSell, 100, 1, /*owner=*/7);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .tif = kFok, .price = 100, .qty = 5};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 5, 0, 5), level(kSell, 100, 6, 2)}));
}

TEST_F(RulesTest, MarketFokUsesWholeBook) {
    limit(1, kSell, 100, 2);
    limit(2, kSell, 900, 2);
    const OrderRequest r{.id = 3, .side = kBuy, .type = OrderType::Market, .tif = kFok, .qty = 4};
    EXPECT_EQ(submit(r).size(), 5U);  // accepted + 2 x (trade, level)
}

// ---- Post-only ------------------------------------------------------------------

TEST_F(RulesTest, PostOnlyRestsWhenItDoesNotCross) {
    limit(1, kSell, 101, 5);
    const OrderRequest r{.id = 2, .side = kBuy, .post_only = true, .price = 100, .qty = 5};
    EXPECT_EQ(submit(r), (Events{accepted(r), level(kBuy, 100, 5, 1)}));
}

TEST_F(RulesTest, PostOnlyThatWouldTradeIsRejectedAndBookUnchanged) {
    limit(1, kSell, 101, 5);
    EXPECT_EQ(submit({.id = 2, .side = kBuy, .post_only = true, .price = 101, .qty = 5}),
              (Events{rejected(2, RejectReason::PostOnlyWouldCross)}));
    EXPECT_EQ(submit({.id = 3, .side = kBuy, .post_only = true, .price = 200, .qty = 5}),
              (Events{rejected(3, RejectReason::PostOnlyWouldCross)}));
    EXPECT_EQ(book_.volume_at(kSell, 101), 5U);
}

TEST_F(RulesTest, PostOnlyChecksEvenAgainstOwnOrders) {
    limit(1, kSell, 101, 5, /*owner=*/7);
    EXPECT_EQ(
        submit({.id = 2, .owner = 7, .side = kBuy, .post_only = true, .price = 101, .qty = 5}),
        (Events{rejected(2, RejectReason::PostOnlyWouldCross)}));
}

// ---- Modify -------------------------------------------------------------------

TEST_F(RulesTest, QuantityDecreaseKeepsQueuePriority) {
    limit(1, kSell, 100, 10);
    limit(2, kSell, 100, 10);
    EXPECT_EQ(modify(1, 100, 4),
              (Events{modified(1, kSell, 100, 4, true), level(kSell, 100, 14, 2)}));
    // Order 1 is still first in line.
    const Events events = limit(3, kBuy, 100, 1);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 1U);
}

TEST_F(RulesTest, QuantityIncreaseLosesQueuePriority) {
    limit(1, kSell, 100, 10);
    limit(2, kSell, 100, 10);
    EXPECT_EQ(modify(1, 100, 12), (Events{modified(1, kSell, 100, 12, false),
                                          level(kSell, 100, 10, 1), level(kSell, 100, 22, 2)}));
    const Events events = limit(3, kBuy, 100, 1);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 2U);
}

TEST_F(RulesTest, PriceChangeLosesQueuePriorityEvenWhenSmaller) {
    limit(1, kSell, 100, 10);
    limit(2, kSell, 101, 10);
    EXPECT_EQ(modify(1, 101, 5), (Events{modified(1, kSell, 101, 5, false), level(kSell, 100, 0, 0),
                                         level(kSell, 101, 15, 2)}));
    const Events events = limit(3, kBuy, 101, 1);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 2U);  // 1 is now behind 2
}

TEST_F(RulesTest, SameQuantitySamePriceIsANoOpThatKeepsPriority) {
    limit(1, kBuy, 100, 10);
    EXPECT_EQ(modify(1, 100, 10),
              (Events{modified(1, kBuy, 100, 10, true), level(kBuy, 100, 10, 1)}));
}

TEST_F(RulesTest, ModifyToCrossingPriceTradesImmediately) {
    limit(1, kSell, 101, 3);
    limit(2, kBuy, 99, 5);
    EXPECT_EQ(modify(2, 101, 5), (Events{modified(2, kBuy, 101, 5, false), level(kBuy, 99, 0, 0),
                                         trade(1, 2, 1, kBuy, 101, 3, 2, 0),
                                         level(kSell, 101, 0, 0), level(kBuy, 101, 2, 1)}));
}

TEST_F(RulesTest, ModifyQuantityRefersToRemainingAfterPartialFill) {
    limit(1, kSell, 100, 10);
    limit(2, kBuy, 100, 6);  // order 1 has 4 left
    EXPECT_EQ(modify(1, 100, 3),
              (Events{modified(1, kSell, 100, 3, true), level(kSell, 100, 3, 1)}));
    EXPECT_EQ(modify(1, 100, 5), (Events{modified(1, kSell, 100, 5, false), level(kSell, 100, 0, 0),
                                         level(kSell, 100, 5, 1)}));
}

TEST_F(RulesTest, InvalidModifiesAreRejectedAndChangeNothing) {
    use_config({.tick_size = 5, .lot_size = 10});
    limit(1, kBuy, 100, 50);
    const auto rejected_modify = [](OrderId id, RejectReason r) {
        return Events{ModifyRejected{.order_id = id, .reason = r}};
    };
    EXPECT_EQ(modify(9, 100, 10), rejected_modify(9, RejectReason::UnknownOrderId));
    EXPECT_EQ(modify(1, 100, 0), rejected_modify(1, RejectReason::ZeroQuantity));
    EXPECT_EQ(modify(1, 101, 10), rejected_modify(1, RejectReason::InvalidPrice));
    EXPECT_EQ(modify(1, 100, 15), rejected_modify(1, RejectReason::InvalidLotSize));
    EXPECT_EQ(book_.volume_at(kBuy, 100), 50U);
}

TEST_F(RulesTest, PostOnlyOrderCannotBeModifiedIntoACross) {
    limit(1, kSell, 101, 5);
    submit({.id = 2, .side = kBuy, .post_only = true, .price = 100, .qty = 5});
    EXPECT_EQ(modify(2, 101, 5),
              (Events{ModifyRejected{.order_id = 2, .reason = RejectReason::PostOnlyWouldCross}}));
    EXPECT_EQ(book_.volume_at(kBuy, 100), 5U);
    // A non-crossing reprice is fine and the order stays post-only.
    EXPECT_EQ(modify(2, 99, 5).size(), 3U);
    EXPECT_EQ(modify(2, 101, 5).size(), 1U);  // still rejected
}

TEST_F(RulesTest, ModifyOfFilledOrderIsUnknown) {
    limit(1, kSell, 100, 5);
    limit(2, kBuy, 100, 5);
    EXPECT_EQ(modify(1, 100, 1),
              (Events{ModifyRejected{.order_id = 1, .reason = RejectReason::UnknownOrderId}}));
}

// ---- Self-trade prevention ---------------------------------------------------

TEST_F(RulesTest, DifferentOwnersTradeNormally) {
    limit(1, kSell, 100, 5, /*owner=*/7);
    EXPECT_EQ(limit(2, kBuy, 100, 5, /*owner=*/8).size(), 3U);  // accepted, trade, level
}

TEST_F(RulesTest, OwnerZeroIsExemptFromSelfTradePrevention) {
    limit(1, kSell, 100, 5, kNoOwner);
    const Events events = limit(2, kBuy, 100, 5, kNoOwner);
    EXPECT_TRUE(std::holds_alternative<Trade>(events[1]));
}

TEST_F(RulesTest, StpCancelNewestCancelsTheIncomingOrder) {
    use_config({.self_trade_policy = SelfTradePolicy::CancelNewest});
    limit(1, kSell, 100, 5, 7);
    const OrderRequest r{.id = 2, .owner = 7, .side = kBuy, .price = 100, .qty = 8};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), canceled(2, 8, 0, CancelReason::SelfTradePrevention)}));
    EXPECT_EQ(book_.volume_at(kSell, 100), 5U);  // resting order untouched
    EXPECT_FALSE(book_.contains(2));
}

TEST_F(RulesTest, StpCancelNewestStillTradesWithOthersFirst) {
    use_config({.self_trade_policy = SelfTradePolicy::CancelNewest});
    limit(1, kSell, 100, 2, 8);
    limit(2, kSell, 100, 5, 7);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .price = 100, .qty = 4};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 2, 2, 0), level(kSell, 100, 5, 1),
                      canceled(3, 2, 0, CancelReason::SelfTradePrevention)}));
}

TEST_F(RulesTest, StpCancelOldestRemovesRestingAndKeepsMatching) {
    use_config({.self_trade_policy = SelfTradePolicy::CancelOldest});
    limit(1, kSell, 100, 5, 7);
    limit(2, kSell, 100, 5, 8);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .price = 100, .qty = 3};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(1, 5, 0, CancelReason::SelfTradePrevention),
                                 level(kSell, 100, 5, 1), trade(1, 3, 2, kBuy, 100, 3, 0, 2),
                                 level(kSell, 100, 2, 1)}));
}

TEST_F(RulesTest, StpCancelOldestThenRestsRemainder) {
    use_config({.self_trade_policy = SelfTradePolicy::CancelOldest});
    limit(1, kSell, 100, 5, 7);
    const OrderRequest r{.id = 2, .owner = 7, .side = kBuy, .price = 100, .qty = 3};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(1, 5, 0, CancelReason::SelfTradePrevention),
                                 level(kSell, 100, 0, 0), level(kBuy, 100, 3, 1)}));
}

TEST_F(RulesTest, StpCancelBothCancelsBoth) {
    use_config({.self_trade_policy = SelfTradePolicy::CancelBoth});
    limit(1, kSell, 100, 5, 7);
    limit(2, kSell, 100, 5, 8);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .price = 100, .qty = 3};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(1, 5, 0, CancelReason::SelfTradePrevention),
                                 level(kSell, 100, 5, 1),
                                 canceled(3, 3, 0, CancelReason::SelfTradePrevention)}));
}

TEST_F(RulesTest, StpDecrementShrinksBothWithoutTrading) {
    use_config({.self_trade_policy = SelfTradePolicy::Decrement});
    limit(1, kSell, 100, 5, 7);
    const OrderRequest r{.id = 2, .owner = 7, .side = kBuy, .price = 100, .qty = 3};
    EXPECT_EQ(submit(r), (Events{accepted(r), canceled(1, 3, 2, CancelReason::SelfTradePrevention),
                                 level(kSell, 100, 2, 1),
                                 canceled(2, 3, 0, CancelReason::SelfTradePrevention)}));
    EXPECT_EQ(book_.volume_at(kSell, 100), 2U);
}

TEST_F(RulesTest, StpDecrementRemovesSmallerRestingAndContinues) {
    use_config({.self_trade_policy = SelfTradePolicy::Decrement});
    limit(1, kSell, 100, 2, 7);
    limit(2, kSell, 100, 5, 8);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .price = 100, .qty = 6};
    EXPECT_EQ(submit(r),
              (Events{accepted(r), canceled(1, 2, 0, CancelReason::SelfTradePrevention),
                      level(kSell, 100, 5, 1), canceled(3, 2, 4, CancelReason::SelfTradePrevention),
                      trade(1, 3, 2, kBuy, 100, 4, 0, 1), level(kSell, 100, 1, 1)}));
}

TEST_F(RulesTest, StpAppliesToModifyReentry) {
    use_config({.self_trade_policy = SelfTradePolicy::CancelNewest});
    limit(1, kSell, 101, 5, 7);
    limit(2, kBuy, 99, 5, 7);
    EXPECT_EQ(modify(2, 101, 5), (Events{modified(2, kBuy, 101, 5, false), level(kBuy, 99, 0, 0),
                                         canceled(2, 5, 0, CancelReason::SelfTradePrevention)}));
}

}  // namespace
}  // namespace litbook
