#include "litbook/loadgen/pacer.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace litbook::loadgen {
namespace {

// The plain formulas in 128 bits, where nothing can overflow, to check the split ones.
__extension__ using Wide = unsigned __int128;

std::uint64_t reference_offset(std::uint64_t index, std::uint64_t rate) {
    return static_cast<std::uint64_t>(Wide{index} * kNanosPerSecond / rate);
}

std::uint64_t reference_due(std::uint64_t elapsed, std::uint64_t rate) {
    const Wide scaled = (Wide{elapsed} + 1) * rate;
    return static_cast<std::uint64_t>((scaled + kNanosPerSecond - 1) / kNanosPerSecond);
}

constexpr std::array<std::uint64_t, 8> kRates = {1,       3,         7,           1'000,
                                                 999'983, 1'000'000, 123'456'789, kMaxRate};

TEST(PacerTest, FirstRequestIsDueAtTheStart) {
    for (const std::uint64_t rate : kRates) {
        EXPECT_EQ(scheduled_offset_ns(0, rate), 0U);
        EXPECT_EQ(sends_due(0, rate), 1U) << rate;
    }
}

TEST(PacerTest, ThirdsOfASecondRoundDown) {
    EXPECT_EQ(scheduled_offset_ns(1, 3), 333'333'333U);
    EXPECT_EQ(scheduled_offset_ns(2, 3), 666'666'666U);
    EXPECT_EQ(scheduled_offset_ns(3, 3), 1'000'000'000U);
    EXPECT_EQ(scheduled_offset_ns(4, 3), 1'333'333'333U);
}

TEST(PacerTest, DueCountStepsExactlyAtEachScheduledTime) {
    // Request i is due at its offset and not one nanosecond earlier, for every rate.
    for (const std::uint64_t rate : kRates) {
        for (std::uint64_t i = 1; i < 2'000; ++i) {
            const std::uint64_t at = scheduled_offset_ns(i, rate);
            ASSERT_EQ(sends_due(at, rate), i + 1) << "rate " << rate << " index " << i;
            ASSERT_EQ(sends_due(at - 1, rate), i) << "rate " << rate << " index " << i;
        }
    }
}

TEST(PacerTest, MatchesTheWideFormulaFarIntoARun) {
    // Up to about 31 years at each rate, where index * 1e9 no longer fits in 64 bits.
    for (const std::uint64_t rate : kRates) {
        for (const std::uint64_t seconds :
             std::array<std::uint64_t, 4>{1, 3'600, 86'400, 1'000'000'000}) {
            for (const std::uint64_t extra :
                 std::array<std::uint64_t, 4>{0, 1, rate / 2, rate - 1}) {
                const std::uint64_t index = seconds * rate + extra;
                ASSERT_EQ(scheduled_offset_ns(index, rate), reference_offset(index, rate))
                    << rate << " " << index;
            }
            const std::uint64_t elapsed = seconds * kNanosPerSecond + 12'345;
            ASSERT_EQ(sends_due(elapsed, rate), reference_due(elapsed, rate)) << rate;
        }
    }
}

TEST(PacerTest, NeverDriftsOverAWholeDay) {
    // Adding a rounded interval again and again would drift. The schedule lands exactly on
    // whole seconds at any rate.
    for (const std::uint64_t rate : kRates) {
        EXPECT_EQ(scheduled_offset_ns(86'400 * rate, rate), 86'400 * kNanosPerSecond) << rate;
        EXPECT_EQ(sends_due(86'400 * kNanosPerSecond - 1, rate), 86'400 * rate) << rate;
    }
}

TEST(PacerTest, OneRequestPerNanosecondAtTheHighestRate) {
    EXPECT_EQ(scheduled_offset_ns(12'345, kMaxRate), 12'345U);
    EXPECT_EQ(sends_due(12'345, kMaxRate), 12'346U);
}

}  // namespace
}  // namespace litbook::loadgen
