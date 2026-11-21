#include "litbook/pipeline/reply_pump.hpp"

#include "litbook/concurrency/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace litbook::pipeline {
namespace {

// Stands in for the session layer and records what the pump hands it.
struct FakeSessions {
    struct Call {
        enum class Kind : std::uint8_t { Deliver, Done };
        Kind kind;
        OwnerId owner;
        std::size_t bytes;
        Timestamp now;
    };

    void deliver(OwnerId owner, std::span<const std::byte> frame, Timestamp now) {
        calls.push_back(
            Call{.kind = Call::Kind::Deliver, .owner = owner, .bytes = frame.size(), .now = now});
    }
    void request_done(OwnerId owner, Timestamp now) {
        calls.push_back(Call{.kind = Call::Kind::Done, .owner = owner, .bytes = 0, .now = now});
    }
    void retry_waiting(Timestamp /*now*/) { ++retries; }

    std::vector<Call> calls;
    int retries = 0;
};

using Replies = concurrency::SpscQueue<Reply>;
using Pump = ReplyPump<FakeSessions, Replies>;

Reply message(OwnerId owner, std::size_t n) {
    const std::vector<std::byte> frame(n, std::byte{'E'});
    return make_message_reply(owner, frame);
}

TEST(ReplyPumpTest, MessagesAreDeliveredAndDoneEndsTheRequest) {
    FakeSessions sessions;
    Replies replies(8);
    Pump pump(sessions, replies, nullptr, 16);
    ASSERT_TRUE(replies.try_push(message(1, 40)));
    ASSERT_TRUE(replies.try_push(make_done_reply(1)));
    pump.drain(77);
    ASSERT_EQ(sessions.calls.size(), 2U);
    EXPECT_EQ(sessions.calls[0].kind, FakeSessions::Call::Kind::Deliver);
    EXPECT_EQ(sessions.calls[0].owner, 1U);
    EXPECT_EQ(sessions.calls[0].bytes, 40U);
    EXPECT_EQ(sessions.calls[0].now, 77U);
    EXPECT_EQ(sessions.calls[1].kind, FakeSessions::Call::Kind::Done);
    EXPECT_EQ(pump.stats().messages, 1U);
    EXPECT_EQ(pump.stats().done, 1U);
    EXPECT_EQ(pump.stats().drains, 1U);
    EXPECT_EQ(sessions.retries, 1);
    EXPECT_EQ(replies.size_approx(), 0U);
}

TEST(ReplyPumpTest, AnEmptyDrainStillLetsTheSessionsRetry) {
    FakeSessions sessions;
    Replies replies(8);
    Pump pump(sessions, replies, nullptr, 16);
    pump.drain(0);
    EXPECT_TRUE(sessions.calls.empty());
    EXPECT_EQ(pump.stats().drains, 0U);
    EXPECT_EQ(sessions.retries, 1);
}

TEST(ReplyPumpTest, OneDrainHandlesAtMostOneBatch) {
    FakeSessions sessions;
    Replies replies(8);
    Pump pump(sessions, replies, nullptr, 2);
    for (OwnerId owner = 1; owner <= 5; ++owner) {
        ASSERT_TRUE(replies.try_push(message(owner, 10)));
    }
    pump.drain(0);
    EXPECT_EQ(sessions.calls.size(), 2U);
    pump.drain(0);
    pump.drain(0);
    EXPECT_EQ(sessions.calls.size(), 5U);
    EXPECT_EQ(sessions.calls[4].owner, 5U);
}

// A drain that stopped at its batch limit left replies behind. The gateway must not
// sleep then, even though no new ring came, or those replies would wait for the next
// ring or tick.
TEST(ReplyPumpTest, TheGatewayMayNotSleepWhileRepliesAreLeftOver) {
    FakeSessions sessions;
    Replies replies(8);
    Doorbell bell;
    Pump pump(sessions, replies, &bell, 1);
    ASSERT_TRUE(replies.try_push(message(1, 10)));
    ASSERT_TRUE(replies.try_push(message(1, 10)));
    pump.drain(0);
    ASSERT_EQ(replies.size_approx(), 1U);
    EXPECT_FALSE(pump.prepare_to_sleep());
    pump.drain(0);
    EXPECT_TRUE(pump.prepare_to_sleep());  // Empty now, and no ring came in.
    pump.woke_up();
}

TEST(ReplyPumpTest, ARingSinceTheLastWakeKeepsTheGatewayAwake) {
    FakeSessions sessions;
    Replies replies(8);
    Doorbell bell;
    Pump pump(sessions, replies, &bell, 16);
    EXPECT_EQ(pump.wake_fd(), bell.fd());
    bell.ring();
    EXPECT_FALSE(pump.prepare_to_sleep());
}

TEST(ReplyPumpTest, StoppedIsSeenAndNothingIsDeliveredForIt) {
    FakeSessions sessions;
    Replies replies(8);
    Pump pump(sessions, replies, nullptr, 16);
    EXPECT_EQ(pump.wake_fd(), -1);
    EXPECT_FALSE(pump.stopped());
    ASSERT_TRUE(replies.try_push(make_stopped_reply()));
    pump.drain(0);
    EXPECT_TRUE(pump.stopped());
    EXPECT_TRUE(sessions.calls.empty());
}

}  // namespace
}  // namespace litbook::pipeline
