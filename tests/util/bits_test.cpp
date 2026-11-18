#include "litbook/util/bits.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <random>

namespace litbook {
namespace {

constexpr std::uint64_t kTwoTo63 = std::uint64_t{1} << 63;

TEST(NextPowerOfTwoTest, ZeroRoundsUpToOne) {
    EXPECT_EQ(next_power_of_two(0), 1U);
}

TEST(NextPowerOfTwoTest, OneIsAlreadyAPowerOfTwo) {
    EXPECT_EQ(next_power_of_two(1), 1U);
}

TEST(NextPowerOfTwoTest, SmallValues) {
    EXPECT_EQ(next_power_of_two(2), 2U);
    EXPECT_EQ(next_power_of_two(3), 4U);
    EXPECT_EQ(next_power_of_two(5), 8U);
    EXPECT_EQ(next_power_of_two(7), 8U);
    EXPECT_EQ(next_power_of_two(9), 16U);
    EXPECT_EQ(next_power_of_two(1000), 1024U);
}

TEST(NextPowerOfTwoTest, PowersOfTwoMapToThemselves) {
    for (int bit = 0; bit < 64; ++bit) {
        const std::uint64_t p = std::uint64_t{1} << bit;
        EXPECT_EQ(next_power_of_two(p), p) << "2^" << bit;
    }
}

TEST(NextPowerOfTwoTest, OnePastAPowerOfTwoDoubles) {
    for (int bit = 1; bit < 63; ++bit) {
        const std::uint64_t p = std::uint64_t{1} << bit;
        EXPECT_EQ(next_power_of_two(p + 1), p << 1) << "2^" << bit << " + 1";
    }
}

TEST(NextPowerOfTwoTest, OneBelowAPowerOfTwoRoundsUpToIt) {
    for (int bit = 2; bit < 64; ++bit) {
        const std::uint64_t p = std::uint64_t{1} << bit;
        EXPECT_EQ(next_power_of_two(p - 1), p) << "2^" << bit << " - 1";
    }
}

TEST(NextPowerOfTwoTest, CrossesThe32BitBoundary) {
    // Catches implementations that only smear bits within the low 32 bits.
    EXPECT_EQ(next_power_of_two(0xFFFF'FFFFULL), 0x1'0000'0000ULL);
    EXPECT_EQ(next_power_of_two(0x1'0000'0001ULL), 0x2'0000'0000ULL);
}

TEST(NextPowerOfTwoTest, LargestValidInput) {
    EXPECT_EQ(next_power_of_two(kTwoTo63 - 1), kTwoTo63);
    EXPECT_EQ(next_power_of_two(kTwoTo63), kTwoTo63);
}

TEST(NextPowerOfTwoTest, UsableAtCompileTime) {
    constexpr std::uint64_t kValue = next_power_of_two(100);
    EXPECT_EQ(kValue, 128U);
}

TEST(NextPowerOfTwoTest, MatchesStdBitCeilOnRandomInputs) {
    std::mt19937_64 rng(42);  // fixed seed: failures are reproducible
    std::uniform_int_distribution<std::uint64_t> dist(1, kTwoTo63);
    for (int i = 0; i < 100'000; ++i) {
        const std::uint64_t x = dist(rng);
        ASSERT_EQ(next_power_of_two(x), std::bit_ceil(x)) << "x = " << x;
    }
}

}  // namespace
}  // namespace litbook
