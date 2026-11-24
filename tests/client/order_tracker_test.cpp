#include "litbook/client/order_tracker.hpp"

#include <gtest/gtest.h>

#include "support/unwrap.hpp"

namespace litbook::client {
namespace {

using protocol::AcceptedMessage;
using protocol::CanceledMessage;
using protocol::ExecutedMessage;
using protocol::Liquidity;
using protocol::RejectedMessage;
using protocol::ReplacedMessage;
using protocol::WireCancelReason;
using protocol::WireRejectReason;

Ticker aapl() {
    return testing::unwrap(make_ticker("AAPL"));
}

AcceptedMessage accepted(protocol::UserRefNum ref, Qty qty, Price price = 1'000'000) {
    return AcceptedMessage{.timestamp = 1,
                           .user_ref = ref,
                           .side = Side::Buy,
                           .qty = qty,
                           .symbol = aapl(),
                           .price = price,
                           .tif = TimeInForce::GoodTillCancel,
                           .type = OrderType::Limit,
                           .post_only = false,
                           .order_ref = 100 + ref};
}

ExecutedMessage executed(protocol::UserRefNum ref, Qty qty) {
    return ExecutedMessage{.timestamp = 2,
                           .user_ref = ref,
                           .qty = qty,
                           .price = 1'000'000,
                           .liquidity = Liquidity::Added,
                           .match = 7};
}

CanceledMessage canceled(protocol::UserRefNum ref, Qty qty,
                         WireCancelReason reason = WireCancelReason::UserRequested) {
    return CanceledMessage{.timestamp = 3, .user_ref = ref, .qty = qty, .reason = reason};
}

ReplacedMessage replaced(protocol::UserRefNum orig, protocol::UserRefNum ref, Qty qty,
                         Price price) {
    return ReplacedMessage{.timestamp = 4,
                           .orig_user_ref = orig,
                           .user_ref = ref,
                           .side = Side::Buy,
                           .qty = qty,
                           .symbol = aapl(),
                           .price = price,
                           .order_ref = 500,
                           .kept_priority = false};
}

RejectedMessage rejected(protocol::UserRefNum ref) {
    return RejectedMessage{
        .timestamp = 5, .user_ref = ref, .reason = WireRejectReason::InvalidPrice};
}

TEST(OrderTrackerTest, AcceptedOrderIsOpenWithItsFullSize) {
    OrderTracker t;
    t.on_reply(accepted(1, 100));
    const OpenOrder& o = testing::unwrap_ptr(t.find(1));
    EXPECT_EQ(o.open, 100U);
    EXPECT_EQ(o.executed, 0U);
    EXPECT_EQ(o.price, 1'000'000);
    EXPECT_EQ(o.symbol, aapl());
    EXPECT_EQ(o.order_ref, 101U);
    EXPECT_EQ(t.open_count(), 1U);
    EXPECT_EQ(t.find(2), nullptr);
}

TEST(OrderTrackerTest, ExecutionsTakeSharesOffAndAFullFillRemovesTheOrder) {
    OrderTracker t;
    t.on_reply(accepted(1, 100));
    t.on_reply(executed(1, 30));
    EXPECT_EQ(testing::unwrap_ptr(t.find(1)).open, 70U);
    EXPECT_EQ(testing::unwrap_ptr(t.find(1)).executed, 30U);
    t.on_reply(executed(1, 70));
    EXPECT_EQ(t.find(1), nullptr);
    EXPECT_EQ(t.open_count(), 0U);
}

TEST(OrderTrackerTest, CancelsShrinkOrRemoveTheOrderWhateverTheReason) {
    OrderTracker t;
    t.on_reply(accepted(1, 100));
    t.on_reply(canceled(1, 40));
    EXPECT_EQ(testing::unwrap_ptr(t.find(1)).open, 60U);
    EXPECT_EQ(testing::unwrap_ptr(t.find(1)).executed, 0U);
    t.on_reply(canceled(1, 60, WireCancelReason::System));
    EXPECT_EQ(t.find(1), nullptr);
}

TEST(OrderTrackerTest, AnIocRemainderCancelEndsTheOrder) {
    OrderTracker t;
    t.on_reply(accepted(1, 100));
    t.on_reply(executed(1, 25));
    t.on_reply(canceled(1, 75, WireCancelReason::ImmediateOrCancel));
    EXPECT_EQ(t.find(1), nullptr);
}

TEST(OrderTrackerTest, TakingOffMoreThanIsOpenClampsToZero) {
    OrderTracker t;
    t.on_reply(accepted(1, 10));
    t.on_reply(executed(1, 50));
    EXPECT_EQ(t.find(1), nullptr);
}

TEST(OrderTrackerTest, ReplacedMovesTheOrderToItsNewNumberAndKeepsWhatExecuted) {
    OrderTracker t;
    t.on_reply(accepted(1, 100));
    t.on_reply(executed(1, 30));
    t.on_reply(replaced(1, 2, 120, 990'000));
    EXPECT_EQ(t.find(1), nullptr);
    const OpenOrder& o = testing::unwrap_ptr(t.find(2));
    EXPECT_EQ(o.open, 120U);
    EXPECT_EQ(o.executed, 30U);
    EXPECT_EQ(o.price, 990'000);
    EXPECT_EQ(o.order_ref, 500U);
    EXPECT_EQ(t.open_count(), 1U);
    // Executions after the replace carry the new number.
    t.on_reply(executed(2, 20));
    EXPECT_EQ(testing::unwrap_ptr(t.find(2)).executed, 50U);
}

TEST(OrderTrackerTest, ReplacedWithNothingOpenRemovesTheOrder) {
    OrderTracker t;
    t.on_reply(accepted(1, 100));
    t.on_reply(replaced(1, 2, 0, 990'000));
    EXPECT_EQ(t.open_count(), 0U);
}

TEST(OrderTrackerTest, RejectedChangesNothing) {
    OrderTracker t;
    t.on_reply(accepted(1, 100));
    t.on_reply(rejected(2));
    t.on_reply(rejected(1));
    EXPECT_EQ(testing::unwrap_ptr(t.find(1)).open, 100U);
}

TEST(OrderTrackerTest, RepliesAboutUnknownOrdersAreIgnored) {
    OrderTracker t;
    t.on_reply(executed(9, 10));
    t.on_reply(canceled(9, 10));
    t.on_reply(replaced(9, 10, 50, 1));
    EXPECT_EQ(t.open_count(), 0U);
}

TEST(AnswersTest, EnterIsAnsweredByAcceptedOrRejectedForItsNumberOnly) {
    const Awaited enter{.kind = RequestKind::Enter, .ref = 5, .orig = 0};
    EXPECT_TRUE(answers(enter, accepted(5, 1)));
    EXPECT_TRUE(answers(enter, rejected(5)));
    EXPECT_FALSE(answers(enter, accepted(4, 1)));
    EXPECT_FALSE(answers(enter, rejected(6)));
    EXPECT_FALSE(answers(enter, executed(5, 1)));
    EXPECT_FALSE(answers(enter, canceled(5, 1, WireCancelReason::ImmediateOrCancel)));
}

TEST(AnswersTest, ReplaceIsAnsweredByReplacedNamingBothNumbersOrRejectedForTheNewOne) {
    const Awaited replace{.kind = RequestKind::Replace, .ref = 6, .orig = 2};
    EXPECT_TRUE(answers(replace, replaced(2, 6, 10, 1)));
    EXPECT_TRUE(answers(replace, rejected(6)));
    EXPECT_FALSE(answers(replace, replaced(3, 6, 10, 1)));
    EXPECT_FALSE(answers(replace, replaced(2, 7, 10, 1)));
    EXPECT_FALSE(answers(replace, rejected(2)));
    EXPECT_FALSE(answers(replace, canceled(2, 10)));
    EXPECT_FALSE(answers(replace, accepted(6, 1)));
}

TEST(AnswersTest, CancelIsAnsweredOnlyByAUserRequestedCancelOfItsOrder) {
    const Awaited cancel{.kind = RequestKind::Cancel, .ref = 3, .orig = 0};
    EXPECT_TRUE(answers(cancel, canceled(3, 10)));
    EXPECT_FALSE(answers(cancel, canceled(3, 10, WireCancelReason::System)));
    EXPECT_FALSE(answers(cancel, canceled(3, 10, WireCancelReason::SelfMatchPrevention)));
    EXPECT_FALSE(answers(cancel, canceled(4, 10)));
    EXPECT_FALSE(answers(cancel, rejected(3)));
    EXPECT_FALSE(answers(cancel, executed(3, 10)));
}

TEST(GoneTest, CancelAndReplaceAreGoneOnceTheirOrderIsNotOpen) {
    OrderTracker t;
    t.on_reply(accepted(1, 100));
    const Awaited cancel{.kind = RequestKind::Cancel, .ref = 1, .orig = 0};
    const Awaited replace{.kind = RequestKind::Replace, .ref = 2, .orig = 1};
    const Awaited enter{.kind = RequestKind::Enter, .ref = 3, .orig = 0};
    EXPECT_FALSE(gone(cancel, t));
    EXPECT_FALSE(gone(replace, t));
    t.on_reply(executed(1, 100));
    EXPECT_TRUE(gone(cancel, t));
    EXPECT_TRUE(gone(replace, t));
    // An Enter always gets an answer, so it is never gone.
    EXPECT_FALSE(gone(enter, t));
}

}  // namespace
}  // namespace litbook::client
