#include "litbook/metrics/tick_clock.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

namespace litbook::metrics {
namespace {

TEST(TickClockTest, TickLengthIsPositiveAndPlausible) {
    const double ns = TickClock::tick_ns();
    EXPECT_GT(ns, 0.0);
    EXPECT_LT(ns, 1'000.0);  // any usable counter ticks at least once per microsecond
}

TEST(TickClockTest, NeverGoesBackwards) {
    std::uint64_t last = TickClock::now();
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t t = TickClock::now();
        ASSERT_GE(t, last);
        last = t;
    }
}

// A 20 ms sleep, measured in ticks and converted to nanoseconds, comes out as at least
// 20 ms. Sleeping can overshoot, especially on a busy CI machine, so the upper limit
// is loose.
TEST(TickClockTest, MeasuresASleepCorrectly) {
    const std::uint64_t start = TickClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t elapsed_ns = TickClock::to_ns(TickClock::now() - start);
    EXPECT_GE(elapsed_ns, 19'000'000U);
    EXPECT_LT(elapsed_ns, 500'000'000U);
}

TEST(TickClockTest, ToNsRoundsToTheNearestNanosecond) {
    EXPECT_EQ(TickClock::to_ns(0), 0U);
    const double ns = TickClock::tick_ns();
    const auto expected = static_cast<std::uint64_t>(std::llround(1'000 * ns));
    EXPECT_EQ(TickClock::to_ns(1'000), expected);
}

}  // namespace
}  // namespace litbook::metrics
