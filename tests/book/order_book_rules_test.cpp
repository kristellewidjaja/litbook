// These tests cover the Phase 2 rules. That means market, IOC, FOK and post-only orders,
// modify, self-trade prevention, tick and lot validation, and the price band and capacity.
// Each test asserts the exact event stream, for every book implementation (BookTypes).

#include "litbook/book/book_config.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <variant>
#include <vector>

#include "book/book_test_support.hpp"

namespace litbook {
namespace {

using namespace testing;  // NOLINT(google-build-using-namespace): event builders

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;
constexpr auto kIoc = TimeInForce::ImmediateOrCancel;
constexpr auto kFok = TimeInForce::FillOrKill;
using Events = std::vector<BookEvent>;

template <class Book>
class RulesTest : public ::testing::Test {
protected:
    // Band and capacity fields left unset get the shared test limits
    // (book_test_support.hpp).
    void use_config(BookConfig config) { book_ = Book(rec_, with_test_limits(config)); }

    void TearDown() override { EXPECT_TRUE(book_.check_invariants()); }

    Events submit(OrderRequest r) {
        rec_.clear();
        book_.submit(r);
        EXPECT_TRUE(book_.check_invariants());
        return rec_.take();
    }
    // Submits a plain resting limit order, with no owner by default.
    Events limit(OrderId id, Side side, Price price, Qty qty, OwnerId owner = kNoOwner) {
        return submit({.id = id, .owner = owner, .side = side, .price = price, .qty = qty});
    }
    Events modify(OrderId id, Price price, Qty qty) {
        rec_.clear();
        book_.modify(id, price, qty);
        EXPECT_TRUE(book_.check_invariants());
        return rec_.take();
    }

    RecordingListener rec_;
    Book book_{rec_, with_test_limits({})};
};

TYPED_TEST_SUITE(RulesTest, BookTypes, BookTypeNames);

// Tests for configuration and validation.

TYPED_TEST(RulesTest, PriceOffTickIsRejected) {
    this->use_config({.tick_size = 5});
    EXPECT_EQ(this->limit(1, kBuy, 102, 1), (Events{rejected(1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(this->limit(2, kBuy, -7, 1), (Events{rejected(2, RejectReason::InvalidPrice)}));
    EXPECT_EQ(this->limit(3, kBuy, 105, 1).size(), 2U);  // accepted + level
    EXPECT_EQ(this->limit(4, kBuy, -10, 1).size(), 2U);  // negative multiple of the tick is fine
}

TYPED_TEST(RulesTest, QuantityNotAMultipleOfLotIsRejected) {
    this->use_config({.lot_size = 100});
    EXPECT_EQ(this->limit(1, kBuy, 100, 150), (Events{rejected(1, RejectReason::InvalidLotSize)}));
    EXPECT_EQ(this->limit(2, kBuy, 100, 99), (Events{rejected(2, RejectReason::InvalidLotSize)}));
    EXPECT_EQ(this->limit(3, kBuy, 100, 300).size(), 2U);
}

TYPED_TEST(RulesTest, ValidationOrderIsFixed) {
    this->use_config({.tick_size = 5, .lot_size = 10});
    // Zero quantity is reported before an off-tick price, and off-tick before odd lot.
    EXPECT_EQ(this->limit(1, kBuy, 3, 0), (Events{rejected(1, RejectReason::ZeroQuantity)}));
    EXPECT_EQ(this->limit(2, kBuy, 3, 7), (Events{rejected(2, RejectReason::InvalidPrice)}));
}

TYPED_TEST(RulesTest, ContradictoryOrderTypesAreRejected) {
    EXPECT_EQ(this->submit({.id = 1, .type = OrderType::Market, .post_only = true, .qty = 1}),
              (Events{rejected(1, RejectReason::InvalidOrderType)}));
    EXPECT_EQ(this->submit({.id = 2, .tif = kIoc, .post_only = true, .price = 100, .qty = 1}),
              (Events{rejected(2, RejectReason::InvalidOrderType)}));
    EXPECT_EQ(this->submit({.id = 3, .tif = kFok, .post_only = true, .price = 100, .qty = 1}),
              (Events{rejected(3, RejectReason::InvalidOrderType)}));
}

// Tests for market orders.

TYPED_TEST(RulesTest, MarketOrderSweepsEveryLevelRegardlessOfPrice) {
    this->limit(1, kSell, 100, 2);
    this->limit(2, kSell, 5'000, 3);
    const OrderRequest r{.id = 3, .side = kBuy, .type = OrderType::Market, .qty = 5};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 2, 3, 0), level(kSell, 100, 0, 0),
                      trade(2, 3, 2, kBuy, 5'000, 3, 0, 0), level(kSell, 5'000, 0, 0)}));
}

TYPED_TEST(RulesTest, MarketOrderRemainderIsCanceledNotRested) {
    this->limit(1, kBuy, 100, 2);
    const OrderRequest r{.id = 2, .side = kSell, .type = OrderType::Market, .qty = 5};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), trade(1, 2, 1, kSell, 100, 2, 3, 0), level(kBuy, 100, 0, 0),
                      canceled(2, 3, 0, CancelReason::Unfilled)}));
    EXPECT_FALSE(this->book_.contains(2));
}

TYPED_TEST(RulesTest, MarketOrderIntoEmptyBookIsCanceled) {
    const OrderRequest r{.id = 1, .side = kBuy, .type = OrderType::Market, .qty = 5};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), canceled(1, 5, 0, CancelReason::Unfilled)}));
}

TYPED_TEST(RulesTest, MarketOrderIgnoresPriceAndTick) {
    this->use_config({.tick_size = 5});
    this->limit(1, kSell, 100, 1);
    const OrderRequest r{
        .id = 2, .side = kBuy, .type = OrderType::Market, .price = 3, .qty = 1};  // off-tick
    const Events events = this->submit(r);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(std::get<OrderAccepted>(events[0]).price, 0);
}

// Tests for IOC (immediate-or-cancel) orders.

TYPED_TEST(RulesTest, IocFillsWhatItCanAndCancelsTheRest) {
    this->limit(1, kSell, 100, 4);
    this->limit(2, kSell, 102, 4);  // beyond the IOC's limit
    const OrderRequest r{.id = 3, .side = kBuy, .tif = kIoc, .price = 101, .qty = 10};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 4, 6, 0), level(kSell, 100, 0, 0),
                      canceled(3, 6, 0, CancelReason::Unfilled)}));
    EXPECT_EQ(this->book_.best_bid(), std::nullopt);
    EXPECT_EQ(this->book_.best_ask(), 102);
}

TYPED_TEST(RulesTest, IocWithNothingToTradeIsFullyCanceled) {
    this->limit(1, kSell, 105, 4);
    const OrderRequest r{.id = 2, .side = kBuy, .tif = kIoc, .price = 100, .qty = 3};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), canceled(2, 3, 0, CancelReason::Unfilled)}));
}

TYPED_TEST(RulesTest, FullyFilledIocHasNoCancel) {
    this->limit(1, kSell, 100, 5);
    const OrderRequest r{.id = 2, .side = kBuy, .tif = kIoc, .price = 100, .qty = 5};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), trade(1, 2, 1, kBuy, 100, 5, 0, 0), level(kSell, 100, 0, 0)}));
}

// Tests for FOK (fill-or-kill) orders.

TYPED_TEST(RulesTest, FokFillsAcrossLevelsWhenEnoughIsAvailable) {
    this->limit(1, kSell, 100, 3);
    this->limit(2, kSell, 101, 3);
    const OrderRequest r{.id = 3, .side = kBuy, .tif = kFok, .price = 101, .qty = 6};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 3, 3, 0), level(kSell, 100, 0, 0),
                      trade(2, 3, 2, kBuy, 101, 3, 0, 0), level(kSell, 101, 0, 0)}));
}

TYPED_TEST(RulesTest, FokIsKilledWithoutTradingWhenShortByOne) {
    this->limit(1, kSell, 100, 3);
    this->limit(2, kSell, 101, 3);
    const OrderRequest r{.id = 3, .side = kBuy, .tif = kFok, .price = 101, .qty = 7};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), canceled(3, 7, 0, CancelReason::Unfilled)}));
    EXPECT_EQ(this->book_.volume_at(kSell, 100), 3U);  // untouched
    EXPECT_EQ(this->book_.volume_at(kSell, 101), 3U);
}

TYPED_TEST(RulesTest, FokOnlyCountsLevelsWithinItsLimit) {
    this->limit(1, kSell, 100, 3);
    this->limit(2, kSell, 102, 10);
    const OrderRequest r{.id = 3, .side = kBuy, .tif = kFok, .price = 101, .qty = 4};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), canceled(3, 4, 0, CancelReason::Unfilled)}));
}

TYPED_TEST(RulesTest, FokIsKilledIfAnOwnOrderIsInTheWay) {
    this->limit(1, kSell, 100, 1, /*owner=*/7);
    this->limit(2, kSell, 100, 10, /*owner=*/8);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .tif = kFok, .price = 100, .qty = 5};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), canceled(3, 5, 0, CancelReason::Unfilled)}));
}

TYPED_TEST(RulesTest, FokFillsIfOwnOrderIsBeyondWhatItNeeds) {
    this->limit(1, kSell, 100, 10, /*owner=*/8);
    this->limit(2, kSell, 100, 1, /*owner=*/7);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .tif = kFok, .price = 100, .qty = 5};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 5, 0, 5), level(kSell, 100, 6, 2)}));
}

TYPED_TEST(RulesTest, MarketFokUsesWholeBook) {
    this->limit(1, kSell, 100, 2);
    this->limit(2, kSell, 900, 2);
    const OrderRequest r{.id = 3, .side = kBuy, .type = OrderType::Market, .tif = kFok, .qty = 4};
    EXPECT_EQ(this->submit(r).size(), 5U);  // accepted + 2 x (trade, level)
}

// Tests for post-only orders.

TYPED_TEST(RulesTest, PostOnlyRestsWhenItDoesNotCross) {
    this->limit(1, kSell, 101, 5);
    const OrderRequest r{.id = 2, .side = kBuy, .post_only = true, .price = 100, .qty = 5};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), level(kBuy, 100, 5, 1)}));
}

TYPED_TEST(RulesTest, PostOnlyThatWouldTradeIsRejectedAndBookUnchanged) {
    this->limit(1, kSell, 101, 5);
    EXPECT_EQ(this->submit({.id = 2, .side = kBuy, .post_only = true, .price = 101, .qty = 5}),
              (Events{rejected(2, RejectReason::PostOnlyWouldCross)}));
    EXPECT_EQ(this->submit({.id = 3, .side = kBuy, .post_only = true, .price = 200, .qty = 5}),
              (Events{rejected(3, RejectReason::PostOnlyWouldCross)}));
    EXPECT_EQ(this->book_.volume_at(kSell, 101), 5U);
}

TYPED_TEST(RulesTest, PostOnlyChecksEvenAgainstOwnOrders) {
    this->limit(1, kSell, 101, 5, /*owner=*/7);
    EXPECT_EQ(this->submit(
                  {.id = 2, .owner = 7, .side = kBuy, .post_only = true, .price = 101, .qty = 5}),
              (Events{rejected(2, RejectReason::PostOnlyWouldCross)}));
}

// Tests for modify.

TYPED_TEST(RulesTest, QuantityDecreaseKeepsQueuePriority) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kSell, 100, 10);
    EXPECT_EQ(this->modify(1, 100, 4),
              (Events{modified(1, kSell, 100, 4, true), level(kSell, 100, 14, 2)}));
    // Order 1 is still first in line.
    const Events events = this->limit(3, kBuy, 100, 1);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 1U);
}

TYPED_TEST(RulesTest, QuantityIncreaseLosesQueuePriority) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kSell, 100, 10);
    EXPECT_EQ(this->modify(1, 100, 12),
              (Events{modified(1, kSell, 100, 12, false), level(kSell, 100, 10, 1),
                      level(kSell, 100, 22, 2)}));
    const Events events = this->limit(3, kBuy, 100, 1);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 2U);
}

TYPED_TEST(RulesTest, PriceChangeLosesQueuePriorityEvenWhenSmaller) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kSell, 101, 10);
    EXPECT_EQ(this->modify(1, 101, 5), (Events{modified(1, kSell, 101, 5, false),
                                               level(kSell, 100, 0, 0), level(kSell, 101, 15, 2)}));
    const Events events = this->limit(3, kBuy, 101, 1);
    EXPECT_EQ(std::get<Trade>(events[1]).resting_id, 2U);  // 1 is now behind 2
}

TYPED_TEST(RulesTest, SameQuantitySamePriceIsANoOpThatKeepsPriority) {
    this->limit(1, kBuy, 100, 10);
    EXPECT_EQ(this->modify(1, 100, 10),
              (Events{modified(1, kBuy, 100, 10, true), level(kBuy, 100, 10, 1)}));
}

TYPED_TEST(RulesTest, ModifyToCrossingPriceTradesImmediately) {
    this->limit(1, kSell, 101, 3);
    this->limit(2, kBuy, 99, 5);
    EXPECT_EQ(this->modify(2, 101, 5),
              (Events{modified(2, kBuy, 101, 5, false), level(kBuy, 99, 0, 0),
                      trade(1, 2, 1, kBuy, 101, 3, 2, 0), level(kSell, 101, 0, 0),
                      level(kBuy, 101, 2, 1)}));
}

TYPED_TEST(RulesTest, ModifyQuantityRefersToRemainingAfterPartialFill) {
    this->limit(1, kSell, 100, 10);
    this->limit(2, kBuy, 100, 6);  // order 1 has 4 left
    EXPECT_EQ(this->modify(1, 100, 3),
              (Events{modified(1, kSell, 100, 3, true), level(kSell, 100, 3, 1)}));
    EXPECT_EQ(this->modify(1, 100, 5), (Events{modified(1, kSell, 100, 5, false),
                                               level(kSell, 100, 0, 0), level(kSell, 100, 5, 1)}));
}

TYPED_TEST(RulesTest, InvalidModifiesAreRejectedAndChangeNothing) {
    this->use_config({.tick_size = 5, .lot_size = 10});
    this->limit(1, kBuy, 100, 50);
    const auto rejected_modify = [](OrderId id, RejectReason r) {
        return Events{ModifyRejected{.order_id = id, .reason = r}};
    };
    EXPECT_EQ(this->modify(9, 100, 10), rejected_modify(9, RejectReason::UnknownOrderId));
    EXPECT_EQ(this->modify(1, 100, 0), rejected_modify(1, RejectReason::ZeroQuantity));
    EXPECT_EQ(this->modify(1, 101, 10), rejected_modify(1, RejectReason::InvalidPrice));
    EXPECT_EQ(this->modify(1, 100, 15), rejected_modify(1, RejectReason::InvalidLotSize));
    EXPECT_EQ(this->book_.volume_at(kBuy, 100), 50U);
}

TYPED_TEST(RulesTest, PostOnlyOrderCannotBeModifiedIntoACross) {
    this->limit(1, kSell, 101, 5);
    this->submit({.id = 2, .side = kBuy, .post_only = true, .price = 100, .qty = 5});
    EXPECT_EQ(this->modify(2, 101, 5),
              (Events{ModifyRejected{.order_id = 2, .reason = RejectReason::PostOnlyWouldCross}}));
    EXPECT_EQ(this->book_.volume_at(kBuy, 100), 5U);
    // A non-crossing reprice is fine and the order stays post-only.
    EXPECT_EQ(this->modify(2, 99, 5).size(), 3U);
    EXPECT_EQ(this->modify(2, 101, 5).size(), 1U);  // still rejected
}

TYPED_TEST(RulesTest, ModifyOfFilledOrderIsUnknown) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kBuy, 100, 5);
    EXPECT_EQ(this->modify(1, 100, 1),
              (Events{ModifyRejected{.order_id = 1, .reason = RejectReason::UnknownOrderId}}));
}

// Tests for self-trade prevention.

TYPED_TEST(RulesTest, DifferentOwnersTradeNormally) {
    this->limit(1, kSell, 100, 5, /*owner=*/7);
    EXPECT_EQ(this->limit(2, kBuy, 100, 5, /*owner=*/8).size(), 3U);  // accepted, trade, level
}

TYPED_TEST(RulesTest, OwnerZeroIsExemptFromSelfTradePrevention) {
    this->limit(1, kSell, 100, 5, kNoOwner);
    const Events events = this->limit(2, kBuy, 100, 5, kNoOwner);
    EXPECT_TRUE(std::holds_alternative<Trade>(events[1]));
}

TYPED_TEST(RulesTest, StpCancelNewestCancelsTheIncomingOrder) {
    this->use_config({.self_trade_policy = SelfTradePolicy::CancelNewest});
    this->limit(1, kSell, 100, 5, 7);
    const OrderRequest r{.id = 2, .owner = 7, .side = kBuy, .price = 100, .qty = 8};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), canceled(2, 8, 0, CancelReason::SelfTradePrevention)}));
    EXPECT_EQ(this->book_.volume_at(kSell, 100), 5U);  // resting order untouched
    EXPECT_FALSE(this->book_.contains(2));
}

TYPED_TEST(RulesTest, StpCancelNewestStillTradesWithOthersFirst) {
    this->use_config({.self_trade_policy = SelfTradePolicy::CancelNewest});
    this->limit(1, kSell, 100, 2, 8);
    this->limit(2, kSell, 100, 5, 7);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .price = 100, .qty = 4};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), trade(1, 3, 1, kBuy, 100, 2, 2, 0), level(kSell, 100, 5, 1),
                      canceled(3, 2, 0, CancelReason::SelfTradePrevention)}));
}

TYPED_TEST(RulesTest, StpCancelOldestRemovesRestingAndKeepsMatching) {
    this->use_config({.self_trade_policy = SelfTradePolicy::CancelOldest});
    this->limit(1, kSell, 100, 5, 7);
    this->limit(2, kSell, 100, 5, 8);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .price = 100, .qty = 3};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), canceled(1, 5, 0, CancelReason::SelfTradePrevention),
                      level(kSell, 100, 5, 1), trade(1, 3, 2, kBuy, 100, 3, 0, 2),
                      level(kSell, 100, 2, 1)}));
}

TYPED_TEST(RulesTest, StpCancelOldestThenRestsRemainder) {
    this->use_config({.self_trade_policy = SelfTradePolicy::CancelOldest});
    this->limit(1, kSell, 100, 5, 7);
    const OrderRequest r{.id = 2, .owner = 7, .side = kBuy, .price = 100, .qty = 3};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), canceled(1, 5, 0, CancelReason::SelfTradePrevention),
                      level(kSell, 100, 0, 0), level(kBuy, 100, 3, 1)}));
}

TYPED_TEST(RulesTest, StpCancelBothCancelsBoth) {
    this->use_config({.self_trade_policy = SelfTradePolicy::CancelBoth});
    this->limit(1, kSell, 100, 5, 7);
    this->limit(2, kSell, 100, 5, 8);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .price = 100, .qty = 3};
    EXPECT_EQ(
        this->submit(r),
        (Events{accepted(r), canceled(1, 5, 0, CancelReason::SelfTradePrevention),
                level(kSell, 100, 5, 1), canceled(3, 3, 0, CancelReason::SelfTradePrevention)}));
}

TYPED_TEST(RulesTest, StpDecrementShrinksBothWithoutTrading) {
    this->use_config({.self_trade_policy = SelfTradePolicy::Decrement});
    this->limit(1, kSell, 100, 5, 7);
    const OrderRequest r{.id = 2, .owner = 7, .side = kBuy, .price = 100, .qty = 3};
    EXPECT_EQ(
        this->submit(r),
        (Events{accepted(r), canceled(1, 3, 2, CancelReason::SelfTradePrevention),
                level(kSell, 100, 2, 1), canceled(2, 3, 0, CancelReason::SelfTradePrevention)}));
    EXPECT_EQ(this->book_.volume_at(kSell, 100), 2U);
}

TYPED_TEST(RulesTest, StpDecrementRemovesSmallerRestingAndContinues) {
    this->use_config({.self_trade_policy = SelfTradePolicy::Decrement});
    this->limit(1, kSell, 100, 2, 7);
    this->limit(2, kSell, 100, 5, 8);
    const OrderRequest r{.id = 3, .owner = 7, .side = kBuy, .price = 100, .qty = 6};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), canceled(1, 2, 0, CancelReason::SelfTradePrevention),
                      level(kSell, 100, 5, 1), canceled(3, 2, 4, CancelReason::SelfTradePrevention),
                      trade(1, 3, 2, kBuy, 100, 4, 0, 1), level(kSell, 100, 1, 1)}));
}

TYPED_TEST(RulesTest, StpAppliesToModifyReentry) {
    this->use_config({.self_trade_policy = SelfTradePolicy::CancelNewest});
    this->limit(1, kSell, 101, 5, 7);
    this->limit(2, kBuy, 99, 5, 7);
    EXPECT_EQ(this->modify(2, 101, 5),
              (Events{modified(2, kBuy, 101, 5, false), level(kBuy, 99, 0, 0),
                      canceled(2, 5, 0, CancelReason::SelfTradePrevention)}));
}

// Tests for the price band.

TYPED_TEST(RulesTest, PriceOutsideTheBandIsRejected) {
    this->use_config({.min_price = 90, .max_price = 110});
    EXPECT_EQ(this->limit(1, kBuy, 89, 1), (Events{rejected(1, RejectReason::PriceOutOfRange)}));
    EXPECT_EQ(this->limit(2, kSell, 111, 1), (Events{rejected(2, RejectReason::PriceOutOfRange)}));
    EXPECT_EQ(this->book_.order_count(), 0U);
}

TYPED_TEST(RulesTest, BandEdgesAreInsideTheBand) {
    this->use_config({.min_price = 90, .max_price = 110});
    EXPECT_EQ(this->limit(1, kBuy, 90, 1).size(), 2U);
    EXPECT_EQ(this->limit(2, kSell, 110, 1).size(), 2U);
    EXPECT_EQ(this->book_.best_bid(), 90);
    EXPECT_EQ(this->book_.best_ask(), 110);
}

TYPED_TEST(RulesTest, BandIsCheckedAfterTickAndBeforeLot) {
    this->use_config({.tick_size = 5, .lot_size = 10, .min_price = 90, .max_price = 110});
    // When an order is off tick and out of band, the tick is reported. When it is out of
    // band and an odd lot, the band is reported.
    EXPECT_EQ(this->limit(1, kBuy, 3, 10), (Events{rejected(1, RejectReason::InvalidPrice)}));
    EXPECT_EQ(this->limit(2, kBuy, 200, 7), (Events{rejected(2, RejectReason::PriceOutOfRange)}));
}

TYPED_TEST(RulesTest, MarketOrdersIgnoreTheBand) {
    this->use_config({.min_price = 90, .max_price = 110});
    this->limit(1, kSell, 110, 1);
    const OrderRequest r{
        .id = 2, .side = kBuy, .type = OrderType::Market, .price = 1'000'000, .qty = 1};
    EXPECT_EQ(this->submit(r),
              (Events{accepted(r), trade(1, 2, 1, kBuy, 110, 1, 0, 0), level(kSell, 110, 0, 0)}));
}

TYPED_TEST(RulesTest, ModifyOutsideTheBandIsRejected) {
    this->use_config({.min_price = 90, .max_price = 110});
    this->limit(1, kBuy, 100, 5);
    EXPECT_EQ(this->modify(1, 80, 5),
              (Events{ModifyRejected{.order_id = 1, .reason = RejectReason::PriceOutOfRange}}));
    EXPECT_EQ(this->book_.volume_at(kBuy, 100), 5U);
}

// Tests for capacity (max_orders).

TYPED_TEST(RulesTest, OrderThatWouldRestInAFullBookIsCanceledBookFull) {
    this->use_config({.max_orders = 2});
    this->limit(1, kBuy, 100, 1);
    this->limit(2, kBuy, 99, 1);
    const OrderRequest r{.id = 3, .side = kBuy, .price = 98, .qty = 4};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), canceled(3, 4, 0, CancelReason::BookFull)}));
    EXPECT_FALSE(this->book_.contains(3));
    EXPECT_EQ(this->book_.order_count(), 2U);
}

TYPED_TEST(RulesTest, FullBookStillTradesAndRestsIntoTheSlotsTradingFreed) {
    this->use_config({.max_orders = 2});
    this->limit(1, kSell, 101, 5);
    this->limit(2, kSell, 102, 5);
    // Fills order 1 completely, which frees its slot, so the remainder can rest.
    const OrderRequest r{.id = 3, .side = kBuy, .price = 101, .qty = 8};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), trade(1, 3, 1, kBuy, 101, 5, 3, 0),
                                       level(kSell, 101, 0, 0), level(kBuy, 101, 3, 1)}));
    EXPECT_EQ(this->book_.order_count(), 2U);
}

// A remainder exists only if every order it crossed was used up, and each of those
// freed a slot. So an order that trades never ends up BookFull.
TYPED_TEST(RulesTest, AnOrderThatTradedAlwaysHasRoomToRest) {
    this->use_config({.max_orders = 2});
    this->limit(1, kSell, 101, 5);
    this->limit(2, kSell, 102, 5);
    // This order is filled completely by part of order 1, so nothing needs to rest.
    const OrderRequest r{.id = 3, .side = kBuy, .price = 101, .qty = 4};
    const Events events = this->submit(r);
    EXPECT_EQ(events,
              (Events{accepted(r), trade(1, 3, 1, kBuy, 101, 4, 0, 1), level(kSell, 101, 1, 1)}));
    // Uses up order 1, freeing its slot for the remainder of 5.
    const OrderRequest big{.id = 4, .side = kBuy, .price = 101, .qty = 6};
    EXPECT_EQ(this->submit(big), (Events{accepted(big), trade(2, 4, 1, kBuy, 101, 1, 5, 0),
                                         level(kSell, 101, 0, 0), level(kBuy, 101, 5, 1)}));
}

TYPED_TEST(RulesTest, IocInAFullBookIsUnfilledNotBookFull) {
    this->use_config({.max_orders = 1});
    this->limit(1, kBuy, 100, 1);
    const OrderRequest r{.id = 2, .side = kBuy, .tif = kIoc, .price = 100, .qty = 1};
    EXPECT_EQ(this->submit(r), (Events{accepted(r), canceled(2, 1, 0, CancelReason::Unfilled)}));
}

TYPED_TEST(RulesTest, CancelMakesRoomInAFullBook) {
    this->use_config({.max_orders = 1});
    this->limit(1, kBuy, 100, 1);
    (void)this->submit({.id = 2, .side = kBuy, .price = 99, .qty = 1});  // BookFull
    this->book_.cancel(1);
    EXPECT_EQ(this->limit(3, kBuy, 99, 1).size(), 2U);
    EXPECT_TRUE(this->book_.contains(3));
}

TYPED_TEST(RulesTest, ModifyInAFullBookNeverRunsOutOfRoom) {
    this->use_config({.max_orders = 2});
    this->limit(1, kBuy, 100, 5);
    this->limit(2, kBuy, 99, 5);
    // Losing priority takes the order out and puts it back, so it reuses its own slot.
    EXPECT_EQ(this->modify(1, 98, 6), (Events{modified(1, kBuy, 98, 6, false),
                                              level(kBuy, 100, 0, 0), level(kBuy, 98, 6, 1)}));
    EXPECT_EQ(this->book_.order_count(), 2U);
}

}  // namespace
}  // namespace litbook
