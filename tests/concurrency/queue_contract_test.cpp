// Tests that SpscQueue and MutexQueue keep the same promises. Each test runs once per
// queue type. The single-threaded tests check the bookkeeping (empty, full, FIFO order,
// wrapping around the slot array). The two-thread tests run a real producer and
// consumer and check that every message arrives once, in order and in one piece. Under
// the tsan presets they also let ThreadSanitizer check the memory ordering.

#include "litbook/concurrency/message_queue.hpp"
#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#include "concurrency/queue_test_support.hpp"

namespace litbook::concurrency {
namespace {

using testing::is_intact;
using testing::make_message;
using testing::TestMessage;

static_assert(MessageQueue<SpscQueue<TestMessage>>);
static_assert(MessageQueue<MutexQueue<TestMessage>>);

template <class Queue>
class QueueContractTest : public ::testing::Test {};

using QueueTypes = ::testing::Types<SpscQueue<TestMessage>, MutexQueue<TestMessage>>;

// Prints SpscQueue or MutexQueue in the test names instead of 0 and 1.
class QueueNames {
public:
    template <class Queue>
    static std::string GetName(int /*index*/) {  // NOLINT(readability-identifier-naming)
        if constexpr (std::is_same_v<Queue, SpscQueue<TestMessage>>) {
            return "SpscQueue";
        } else {
            return "MutexQueue";
        }
    }
};

TYPED_TEST_SUITE(QueueContractTest, QueueTypes, QueueNames);

TYPED_TEST(QueueContractTest, RejectsCapacitiesThatAreNotPowersOfTwo) {
    EXPECT_THROW(TypeParam{0}, std::invalid_argument);
    EXPECT_THROW(TypeParam{3}, std::invalid_argument);
    EXPECT_THROW(TypeParam{1000}, std::invalid_argument);
    EXPECT_THROW(TypeParam{kMaxQueueCapacity * 2}, std::invalid_argument);
}

TYPED_TEST(QueueContractTest, AcceptsPowersOfTwo) {
    for (const std::size_t capacity : {std::size_t{1}, std::size_t{2}, std::size_t{1024}}) {
        const TypeParam queue(capacity);
        EXPECT_EQ(queue.capacity(), capacity);
    }
}

TYPED_TEST(QueueContractTest, NewQueueIsEmpty) {
    TypeParam queue(8);
    TestMessage out = make_message(77);
    EXPECT_EQ(queue.size_approx(), 0U);
    EXPECT_EQ(queue.front(), nullptr);
    EXPECT_FALSE(queue.try_pop(out));
    EXPECT_EQ(out.sequence, 77U);  // A failed pop leaves out alone.
}

TYPED_TEST(QueueContractTest, FillsToCapacityThenRefuses) {
    TypeParam queue(8);
    for (std::uint64_t i = 0; i < 8; ++i) {
        EXPECT_TRUE(queue.try_push(make_message(i)));
        EXPECT_EQ(queue.size_approx(), i + 1);
    }
    EXPECT_FALSE(queue.try_push(make_message(8)));
    EXPECT_EQ(queue.size_approx(), 8U);

    // The refused message must not have overwritten the oldest one.
    const TestMessage* oldest = queue.front();
    ASSERT_NE(oldest, nullptr);
    EXPECT_EQ(oldest->sequence, 0U);
}

TYPED_TEST(QueueContractTest, PopsInTheOrderPushed) {
    TypeParam queue(16);
    for (std::uint64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(queue.try_push(make_message(i)));
    }
    for (std::uint64_t i = 0; i < 10; ++i) {
        TestMessage out;
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out.sequence, i);
        EXPECT_TRUE(is_intact(out));
    }
    EXPECT_EQ(queue.size_approx(), 0U);
}

TYPED_TEST(QueueContractTest, FrontLooksWithoutRemovingAndPopRemoves) {
    TypeParam queue(4);
    ASSERT_TRUE(queue.try_push(make_message(5)));
    ASSERT_TRUE(queue.try_push(make_message(6)));

    const TestMessage* first = queue.front();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->sequence, 5U);
    // Looking again gives the same message, because front() does not remove it.
    const TestMessage* again = queue.front();
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->sequence, 5U);
    EXPECT_EQ(queue.size_approx(), 2U);

    queue.pop();
    const TestMessage* second = queue.front();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->sequence, 6U);
    queue.pop();
    EXPECT_EQ(queue.front(), nullptr);
}

TYPED_TEST(QueueContractTest, SpaceFreedByAPopIsReused) {
    TypeParam queue(4);
    for (std::uint64_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(queue.try_push(make_message(i)));
    }
    TestMessage out;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_TRUE(queue.try_push(make_message(4)));
    EXPECT_FALSE(queue.try_push(make_message(5)));

    for (std::uint64_t expected = 1; expected <= 4; ++expected) {
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out.sequence, expected);
    }
}

TYPED_TEST(QueueContractTest, CapacityOneHoldsOneMessageAtATime) {
    TypeParam queue(1);
    for (std::uint64_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(queue.try_push(make_message(i)));
        ASSERT_FALSE(queue.try_push(make_message(i + 1000)));
        TestMessage out;
        ASSERT_TRUE(queue.try_pop(out));
        ASSERT_EQ(out.sequence, i);
    }
}

// Pushes and pops batches of different sizes so the counters pass the end of the slot
// array thousands of times, at every possible offset.
TYPED_TEST(QueueContractTest, WrapsAroundTheSlotArrayManyTimes) {
    TypeParam queue(4);
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    for (int round = 0; round < 10'000; ++round) {
        const int batch = 1 + (round % 4);
        for (int i = 0; i < batch; ++i) {
            ASSERT_TRUE(queue.try_push(make_message(pushed)));
            ++pushed;
        }
        for (int i = 0; i < batch; ++i) {
            TestMessage out;
            ASSERT_TRUE(queue.try_pop(out));
            ASSERT_EQ(out.sequence, popped);
            ASSERT_TRUE(is_intact(out));
            ++popped;
        }
    }
    EXPECT_EQ(pushed, popped);
    EXPECT_EQ(queue.size_approx(), 0U);
}

// What one two-thread run saw. full_seen and empty_seen prove that the run really
// exercised the full and empty paths, which is where ordering bugs hide.
struct StressResult {
    std::uint64_t received = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t torn = 0;
    std::uint64_t full_seen = 0;
    std::uint64_t empty_seen = 0;
    std::size_t largest_size_seen = 0;
};

enum class PopStyle : std::uint8_t { TryPop, FrontThenPop };

// Runs one producer thread that pushes count numbered messages and consumes them on the
// calling thread. Both sides yield now and then at random, so the two threads drift in
// and out of step and the queue goes full and empty many times.
template <class Queue>
StressResult run_stress(std::size_t capacity, std::uint64_t count, PopStyle style, unsigned seed) {
    Queue queue(capacity);
    StressResult result;
    std::uint64_t full_seen = 0;

    std::thread producer([&queue, &full_seen, count, seed] {
        std::minstd_rand random(seed);
        for (std::uint64_t i = 0; i < count; ++i) {
            const TestMessage message = make_message(i);
            while (!queue.try_push(message)) {
                ++full_seen;
                std::this_thread::yield();
            }
            if ((random() & 63U) == 0) {
                std::this_thread::yield();
            }
        }
    });

    std::minstd_rand random(seed + 1);
    std::uint64_t expected = 0;
    while (expected < count) {
        TestMessage message;
        bool got = false;
        if (style == PopStyle::TryPop) {
            got = queue.try_pop(message);
        } else if (const TestMessage* front = queue.front(); front != nullptr) {
            message = *front;
            queue.pop();
            got = true;
        }
        if (!got) {
            ++result.empty_seen;
            std::this_thread::yield();
            continue;
        }
        if (message.sequence != expected) {
            ++result.out_of_order;
        }
        if (!is_intact(message)) {
            ++result.torn;
        }
        ++result.received;
        ++expected;
        if ((random() & 63U) == 0) {
            result.largest_size_seen = std::max(result.largest_size_seen, queue.size_approx());
            std::this_thread::yield();
        }
    }
    producer.join();
    result.full_seen = full_seen;
    return result;
}

void expect_clean(const StressResult& result, std::uint64_t count) {
    EXPECT_EQ(result.received, count);
    EXPECT_EQ(result.out_of_order, 0U);
    EXPECT_EQ(result.torn, 0U);
}

TYPED_TEST(QueueContractTest, TwoThreadsDeliverEveryMessageInOrder) {
    constexpr std::uint64_t kCount = 100'000;
    unsigned seed = 1;
    for (const std::size_t capacity :
         {std::size_t{1}, std::size_t{2}, std::size_t{8}, std::size_t{1024}}) {
        SCOPED_TRACE(capacity);
        const StressResult result =
            run_stress<TypeParam>(capacity, kCount, PopStyle::TryPop, seed++);
        expect_clean(result, kCount);
        EXPECT_LE(result.largest_size_seen, capacity);
        if (capacity <= 2) {
            // With one or two slots the producer must have run into a full queue and
            // the consumer into an empty one, or this test did not test much.
            EXPECT_GT(result.full_seen, 0U);
            EXPECT_GT(result.empty_seen, 0U);
        }
    }
}

TYPED_TEST(QueueContractTest, TwoThreadsDeliverEveryMessageThroughFrontAndPop) {
    constexpr std::uint64_t kCount = 100'000;
    unsigned seed = 100;
    for (const std::size_t capacity : {std::size_t{1}, std::size_t{8}, std::size_t{1024}}) {
        SCOPED_TRACE(capacity);
        const StressResult result =
            run_stress<TypeParam>(capacity, kCount, PopStyle::FrontThenPop, seed++);
        expect_clean(result, kCount);
        if (capacity == 1) {
            EXPECT_GT(result.full_seen, 0U);
            EXPECT_GT(result.empty_seen, 0U);
        }
    }
}

// Two queues in a loop. The main thread sends a request, the echo thread sends back the
// same message, and only then does the next request go out. This is the pattern the
// latency benchmark uses, and it makes every message cross both queues while empty.
TYPED_TEST(QueueContractTest, PingPongBetweenTwoQueues) {
    constexpr std::uint64_t kRoundTrips = 20'000;
    TypeParam requests(1);
    TypeParam replies(1);

    std::thread echo([&requests, &replies] {
        for (std::uint64_t i = 0; i < kRoundTrips; ++i) {
            TestMessage message;
            while (!requests.try_pop(message)) {
                std::this_thread::yield();
            }
            while (!replies.try_push(message)) {
                std::this_thread::yield();
            }
        }
    });

    std::uint64_t wrong = 0;
    for (std::uint64_t i = 0; i < kRoundTrips; ++i) {
        while (!requests.try_push(make_message(i))) {
            std::this_thread::yield();
        }
        TestMessage reply;
        while (!replies.try_pop(reply)) {
            std::this_thread::yield();
        }
        if (reply.sequence != i || !is_intact(reply)) {
            ++wrong;
        }
    }
    echo.join();
    EXPECT_EQ(wrong, 0U);
    EXPECT_EQ(requests.size_approx(), 0U);
    EXPECT_EQ(replies.size_approx(), 0U);
}

}  // namespace
}  // namespace litbook::concurrency
