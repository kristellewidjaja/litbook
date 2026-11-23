#include "litbook/loadgen/in_flight_queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>

namespace litbook::loadgen {
namespace {

InFlight request(RequestKind kind, protocol::UserRefNum ref, std::uint64_t at = 0) {
    return InFlight{.kind = kind, .ref = ref, .pass = 0, .scheduled_ns = at, .sent_ns = at + 1};
}

std::uint32_t ignored(const Answer& a, RequestKind kind) {
    return a.ignored[static_cast<std::size_t>(kind)];
}

TEST(InFlightQueueTest, ZeroCapacityIsRefused) {
    EXPECT_THROW(InFlightQueue(0), std::invalid_argument);
}

TEST(InFlightQueueTest, PushUntilFull) {
    InFlightQueue q(2);
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.push(request(RequestKind::Enter, 1)));
    EXPECT_TRUE(q.push(request(RequestKind::Enter, 2)));
    EXPECT_TRUE(q.full());
    EXPECT_FALSE(q.push(request(RequestKind::Enter, 3)));
    EXPECT_EQ(q.size(), 2U);
    EXPECT_EQ(q.front().ref, 1U);
}

TEST(InFlightQueueTest, FirstReplyAnswersTheFront) {
    InFlightQueue q(4);
    ASSERT_TRUE(q.push(request(RequestKind::Enter, 7, 100)));
    const Answer a = q.answer(ReplyKind::Accepted, 7);
    ASSERT_TRUE(a.found);
    EXPECT_EQ(a.request.ref, 7U);
    EXPECT_EQ(a.request.scheduled_ns, 100U);
    EXPECT_EQ(a.request.sent_ns, 101U);
    EXPECT_TRUE(q.empty());
}

TEST(InFlightQueueTest, OlderRequestsWithoutAReplyAreCountedAsIgnored) {
    // A cancel and a replace for orders that had filled get no reply. The Accepted of a
    // later enter proves the exchange handled them.
    InFlightQueue q(8);
    ASSERT_TRUE(q.push(request(RequestKind::Cancel, 3)));
    ASSERT_TRUE(q.push(request(RequestKind::Replace, 10)));
    ASSERT_TRUE(q.push(request(RequestKind::Cancel, 4)));
    ASSERT_TRUE(q.push(request(RequestKind::Enter, 11)));
    ASSERT_TRUE(q.push(request(RequestKind::Enter, 12)));
    const Answer a = q.answer(ReplyKind::Accepted, 11);
    ASSERT_TRUE(a.found);
    EXPECT_EQ(a.request.ref, 11U);
    EXPECT_EQ(ignored(a, RequestKind::Cancel), 2U);
    EXPECT_EQ(ignored(a, RequestKind::Replace), 1U);
    EXPECT_EQ(ignored(a, RequestKind::Enter), 0U);
    ASSERT_EQ(q.size(), 1U);
    EXPECT_EQ(q.front().ref, 12U);
}

TEST(InFlightQueueTest, AReplyThatOnlyFollowsChangesNothing) {
    // The Order Canceled after a failed replace names the original order, and no Cancel
    // for it is on its way. It must not take anything out.
    InFlightQueue q(4);
    ASSERT_TRUE(q.push(request(RequestKind::Enter, 20)));
    const Answer a = q.answer(ReplyKind::UserCanceled, 5);
    EXPECT_FALSE(a.found);
    EXPECT_EQ(ignored(a, RequestKind::Enter), 0U);
    EXPECT_EQ(q.size(), 1U);
}

TEST(InFlightQueueTest, KindMustMatchAsWellAsTheNumber) {
    // An Order Canceled U for UserRefNum 9 does not answer the Enter with the same number,
    // and Order Replaced does not answer an Enter either.
    InFlightQueue q(4);
    ASSERT_TRUE(q.push(request(RequestKind::Enter, 9)));
    EXPECT_FALSE(q.answer(ReplyKind::UserCanceled, 9).found);
    EXPECT_FALSE(q.answer(ReplyKind::Replaced, 9).found);
    EXPECT_EQ(q.size(), 1U);
    EXPECT_TRUE(q.answer(ReplyKind::Accepted, 9).found);
}

TEST(InFlightQueueTest, RejectedAnswersEnterReplaceAndFence) {
    InFlightQueue q(4);
    ASSERT_TRUE(q.push(request(RequestKind::Replace, 1)));
    ASSERT_TRUE(q.push(request(RequestKind::Enter, 2)));
    ASSERT_TRUE(q.push(request(RequestKind::Fence, 3)));
    ASSERT_TRUE(q.push(request(RequestKind::Cancel, 4)));
    EXPECT_EQ(q.answer(ReplyKind::Rejected, 1).request.kind, RequestKind::Replace);
    EXPECT_EQ(q.answer(ReplyKind::Rejected, 2).request.kind, RequestKind::Enter);
    EXPECT_EQ(q.answer(ReplyKind::Rejected, 3).request.kind, RequestKind::Fence);
    EXPECT_FALSE(q.answer(ReplyKind::Rejected, 4).found);  // A Cancel is never rejected.
}

TEST(InFlightQueueTest, AnswersTable) {
    EXPECT_TRUE(answers(ReplyKind::Accepted, RequestKind::Enter));
    EXPECT_FALSE(answers(ReplyKind::Accepted, RequestKind::Fence));
    EXPECT_FALSE(answers(ReplyKind::Accepted, RequestKind::Replace));
    EXPECT_TRUE(answers(ReplyKind::Replaced, RequestKind::Replace));
    EXPECT_FALSE(answers(ReplyKind::Replaced, RequestKind::Cancel));
    EXPECT_TRUE(answers(ReplyKind::UserCanceled, RequestKind::Cancel));
    EXPECT_FALSE(answers(ReplyKind::UserCanceled, RequestKind::Enter));
    EXPECT_FALSE(answers(ReplyKind::Rejected, RequestKind::Cancel));
}

TEST(InFlightQueueTest, WrapsAroundTheRing) {
    InFlightQueue q(3);
    protocol::UserRefNum next = 1;
    protocol::UserRefNum oldest = 1;
    for (int round = 0; round < 50; ++round) {
        while (!q.full()) {
            ASSERT_TRUE(q.push(request(RequestKind::Enter, next++)));
        }
        // Answer the second oldest, so the oldest goes as ignored.
        const Answer a = q.answer(ReplyKind::Accepted, oldest + 1);
        ASSERT_TRUE(a.found);
        ASSERT_EQ(a.request.ref, oldest + 1);
        ASSERT_EQ(ignored(a, RequestKind::Enter), 1U);
        oldest += 2;
        ASSERT_EQ(q.front().ref, oldest);
    }
}

TEST(InFlightQueueTest, ClearEmptiesAndCounts) {
    InFlightQueue q(4);
    ASSERT_TRUE(q.push(request(RequestKind::Enter, 1)));
    ASSERT_TRUE(q.push(request(RequestKind::Cancel, 1)));
    EXPECT_EQ(q.clear(), 2U);
    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.push(request(RequestKind::Enter, 2)));
    EXPECT_EQ(q.front().ref, 2U);
}

}  // namespace
}  // namespace litbook::loadgen
