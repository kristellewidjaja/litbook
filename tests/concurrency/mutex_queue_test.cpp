// Tests that only apply to MutexQueue: the waiting calls push_wait and pop_wait. The
// behavior it shares with SpscQueue is tested in queue_contract_test.cpp.

#include "litbook/concurrency/mutex_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "concurrency/queue_test_support.hpp"

namespace litbook::concurrency {
namespace {

using testing::is_intact;
using testing::make_message;
using testing::TestMessage;

// A waiting push on a full queue must not return until the consumer makes room. The
// sleep can only make this test pass wrongly on a very slow machine (if the helper
// thread had not reached push_wait yet), never fail a correct queue.
TEST(MutexQueueTest, PushWaitBlocksWhileFullAndFinishesAfterAPop) {
    MutexQueue<TestMessage> queue(2);
    queue.push_wait(make_message(0));
    queue.push_wait(make_message(1));

    std::atomic<bool> pushed{false};
    std::thread producer([&queue, &pushed] {
        queue.push_wait(make_message(2));
        pushed.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(pushed.load());

    TestMessage out;
    ASSERT_TRUE(queue.try_pop(out));
    producer.join();
    EXPECT_TRUE(pushed.load());

    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out.sequence, 1U);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out.sequence, 2U);
}

TEST(MutexQueueTest, PopWaitBlocksWhileEmptyAndFinishesAfterAPush) {
    MutexQueue<TestMessage> queue(2);
    std::atomic<bool> popped{false};
    TestMessage received;
    std::thread consumer([&queue, &popped, &received] {
        queue.pop_wait(received);
        popped.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(popped.load());

    ASSERT_TRUE(queue.try_push(make_message(42)));
    consumer.join();
    EXPECT_TRUE(popped.load());
    EXPECT_EQ(received.sequence, 42U);
    EXPECT_TRUE(is_intact(received));
}

// Both sides sleep instead of spinning, through a queue small enough that each side
// waits many times.
TEST(MutexQueueTest, WaitingProducerAndConsumerDeliverEveryMessageInOrder) {
    constexpr std::uint64_t kCount = 50'000;
    MutexQueue<TestMessage> queue(4);

    std::thread producer([&queue] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            queue.push_wait(make_message(i));
        }
    });

    std::uint64_t wrong = 0;
    for (std::uint64_t i = 0; i < kCount; ++i) {
        TestMessage out;
        queue.pop_wait(out);
        if (out.sequence != i || !is_intact(out)) {
            ++wrong;
        }
    }
    producer.join();
    EXPECT_EQ(wrong, 0U);
    EXPECT_EQ(queue.size_approx(), 0U);
}

}  // namespace
}  // namespace litbook::concurrency
