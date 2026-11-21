// Tests that only apply to SpscQueue: its memory layout, which element types it takes,
// and the cache line constant it is built on. The behavior it shares with MutexQueue is
// tested in queue_contract_test.cpp.

#include "litbook/concurrency/spsc_queue.hpp"

#include "litbook/concurrency/cache_line.hpp"
#include "litbook/concurrency/message_queue.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/queue_test_support.hpp"

namespace litbook::concurrency {
namespace {

using testing::make_message;
using testing::TestMessage;

// Messages must be plain bytes, so a type that owns heap memory is refused at compile
// time.
static_assert(QueueElement<TestMessage>);
static_assert(QueueElement<int>);
static_assert(QueueElement<std::array<std::byte, 47>>);
static_assert(!QueueElement<std::string>);
static_assert(!QueueElement<std::vector<int>>);
static_assert(!QueueElement<std::unique_ptr<int>>);

TEST(CacheLineTest, IsAPowerOfTwoAndAtLeastWhatTheCpuReports) {
    EXPECT_EQ(kCacheLineSize & (kCacheLineSize - 1), 0U);
    EXPECT_GE(kCacheLineSize, 64U);
    // Linux reports the first level data cache line here. Some systems report 0 or -1
    // when they do not know, which is fine.
    const long reported = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (reported > 0) {
        EXPECT_GE(kCacheLineSize, static_cast<std::size_t>(reported));
    }
}

// The layout is what keeps the two threads from sharing cache lines. The class is
// aligned to a cache line and holds five lines, one read only and four written.
TEST(SpscQueueTest, LayoutGivesEveryWrittenCounterItsOwnCacheLine) {
    using Queue = SpscQueue<TestMessage>;
    EXPECT_EQ(alignof(Queue), kCacheLineSize);
    EXPECT_EQ(sizeof(Queue) % kCacheLineSize, 0U);
    EXPECT_GE(sizeof(Queue), 5 * kCacheLineSize);

    // A queue on the heap is aligned too (C++17 aligned new).
    const auto queue = std::make_unique<Queue>(8);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(queue.get()) % kCacheLineSize, 0U);
}

TEST(SpscQueueTest, WorksWithSmallAndOddSizedElements) {
    SpscQueue<std::uint8_t> bytes(4);
    for (std::uint8_t i = 0; i < 200; ++i) {
        ASSERT_TRUE(bytes.try_push(i));
        std::uint8_t out = 0;
        ASSERT_TRUE(bytes.try_pop(out));
        ASSERT_EQ(out, i);
    }

    // 47 bytes is the largest OUCH frame, the kind of thing Phase 7b sends.
    using Frame = std::array<std::byte, 47>;
    SpscQueue<Frame> frames(2);
    Frame frame{};
    frame[0] = std::byte{'O'};
    frame[46] = std::byte{0x2A};
    ASSERT_TRUE(frames.try_push(frame));
    Frame out{};
    ASSERT_TRUE(frames.try_pop(out));
    EXPECT_EQ(out, frame);
}

// The producer works from its cached copy of head_ until that copy says the queue is
// full. This checks the copy is refreshed then, so space the consumer freed is found.
TEST(SpscQueueTest, ProducerSeesSpaceFreedAfterItsCopyWentStale) {
    SpscQueue<TestMessage> queue(2);
    ASSERT_TRUE(queue.try_push(make_message(0)));
    ASSERT_TRUE(queue.try_push(make_message(1)));
    ASSERT_FALSE(queue.try_push(make_message(2)));  // The cached head is now 0.

    TestMessage out;
    ASSERT_TRUE(queue.try_pop(out));
    ASSERT_TRUE(queue.try_pop(out));
    // The cached head is still 0, so this push must reread the real head.
    EXPECT_TRUE(queue.try_push(make_message(2)));
    EXPECT_TRUE(queue.try_push(make_message(3)));
    EXPECT_FALSE(queue.try_push(make_message(4)));
}

// The same for the consumer's cached copy of tail_.
TEST(SpscQueueTest, ConsumerSeesMessagesPushedAfterItsCopyWentStale) {
    SpscQueue<TestMessage> queue(4);
    EXPECT_EQ(queue.front(), nullptr);  // The cached tail is now 0.
    ASSERT_TRUE(queue.try_push(make_message(9)));
    const TestMessage* front = queue.front();
    ASSERT_NE(front, nullptr);
    EXPECT_EQ(front->sequence, 9U);
}

// size_approx from the producer side while the consumer is running must never report
// more than the capacity, even though it reads the two counters at different times.
TEST(SpscQueueTest, SizeSeenFromTheProducerNeverExceedsCapacity) {
    constexpr std::uint64_t kCount = 50'000;
    constexpr std::size_t kCapacity = 4;
    SpscQueue<TestMessage> queue(kCapacity);

    std::thread consumer([&queue] {
        std::uint64_t received = 0;
        while (received < kCount) {
            TestMessage out;
            if (queue.try_pop(out)) {
                ++received;
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::size_t largest = 0;
    for (std::uint64_t i = 0; i < kCount; ++i) {
        while (!queue.try_push(make_message(i))) {
            std::this_thread::yield();
        }
        largest = std::max(largest, queue.size_approx());
    }
    consumer.join();
    EXPECT_LE(largest, kCapacity);
    EXPECT_EQ(queue.size_approx(), 0U);
}

}  // namespace
}  // namespace litbook::concurrency
