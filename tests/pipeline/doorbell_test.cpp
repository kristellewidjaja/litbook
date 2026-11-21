#include "litbook/pipeline/doorbell.hpp"

#include "litbook/concurrency/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <poll.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace litbook::pipeline {
namespace {

// True if the doorbell's eventfd is readable right now.
bool readable(const Doorbell& bell) {
    pollfd p{};
    p.fd = bell.fd();
    p.events = POLLIN;
    return ::poll(&p, 1, 0) == 1;
}

TEST(DoorbellTest, RingingAnAwakeConsumerMakesNoSystemCall) {
    Doorbell bell;
    bell.ring();
    bell.ring();
    EXPECT_EQ(bell.wakeups_sent(), 0U);
    EXPECT_FALSE(readable(bell));
}

TEST(DoorbellTest, ARingBeforeSleepingTellsTheConsumerToLookAgain) {
    Doorbell bell;
    bell.ring();
    EXPECT_FALSE(bell.prepare_to_sleep());
    // That answer used up the ring, so the next attempt may sleep.
    EXPECT_TRUE(bell.prepare_to_sleep());
    bell.woke_up();
}

TEST(DoorbellTest, RingingASleepingConsumerWritesTheEventfdOnce) {
    Doorbell bell;
    ASSERT_TRUE(bell.prepare_to_sleep());
    bell.ring();
    bell.ring();  // The state is Rung now, so this one writes nothing.
    EXPECT_EQ(bell.wakeups_sent(), 1U);
    EXPECT_TRUE(readable(bell));
    bell.woke_up();
    EXPECT_FALSE(readable(bell));
}

TEST(DoorbellTest, SleepReturnsAfterTheTimeoutWithoutARing) {
    Doorbell bell;
    ASSERT_TRUE(bell.prepare_to_sleep());
    const auto start = std::chrono::steady_clock::now();
    bell.sleep(20);
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(15));
    // It is awake again, so a ring writes nothing.
    bell.ring();
    EXPECT_EQ(bell.wakeups_sent(), 0U);
}

TEST(DoorbellTest, ARingFromAnotherThreadWakesASleepingConsumer) {
    Doorbell bell;
    ASSERT_TRUE(bell.prepare_to_sleep());
    std::thread producer([&bell] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bell.ring();
    });
    bell.sleep(-1);  // Hangs (and the test times out) if the ring is lost.
    producer.join();
    EXPECT_EQ(bell.wakeups_sent(), 1U);
}

// The real use. A producer pushes numbered messages one by one, and the consumer drains
// the queue and sleeps on the doorbell whenever it is empty, with no timeout. If the
// doorbell ever lost a wakeup, the consumer would sleep forever and ctest would time the
// test out. Random pauses in the producer make the consumer sleep often.
TEST(DoorbellTest, AConsumerThatSleepsWheneverItsQueueIsEmptyGetsEveryMessage) {
    constexpr std::uint64_t kMessages = 50'000;
    concurrency::SpscQueue<std::uint64_t> queue(64);
    Doorbell bell;
    std::thread producer([&] {
        std::uint32_t state = 12345;
        for (std::uint64_t i = 0; i < kMessages; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
            bell.ring();
            state = state * 1'103'515'245U + 12'345U;
            if ((state >> 16U) % 64U == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        }
    });
    std::uint64_t expected = 0;
    std::uint64_t sleeps = 0;
    while (expected < kMessages) {
        std::uint64_t value = 0;
        if (queue.try_pop(value)) {
            ASSERT_EQ(value, expected);
            ++expected;
            continue;
        }
        if (bell.prepare_to_sleep()) {
            ++sleeps;
            bell.sleep(-1);
        }
    }
    producer.join();
    // The test is only worth something if the consumer really slept and was woken.
    EXPECT_GT(sleeps, 10U);
    EXPECT_GT(bell.wakeups_sent(), 10U);
}

}  // namespace
}  // namespace litbook::pipeline
