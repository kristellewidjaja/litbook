#include "litbook/container/occupancy_bitmap.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <random>
#include <vector>

namespace litbook {
namespace {

constexpr std::size_t kNotFound = OccupancyBitmap::kNotFound;

TEST(OccupancyBitmapTest, EmptyBitmapFindsNothing) {
    const OccupancyBitmap bits(200);
    EXPECT_EQ(bits.size(), 200U);
    EXPECT_EQ(bits.find_next(0), kNotFound);
    EXPECT_EQ(bits.find_prev(199), kNotFound);
    EXPECT_FALSE(bits.test(0));
}

TEST(OccupancyBitmapTest, ZeroSizeIsSafe) {
    const OccupancyBitmap bits(0);
    EXPECT_EQ(bits.find_next(0), kNotFound);
    EXPECT_EQ(bits.find_prev(0), kNotFound);
    EXPECT_EQ(bits.find_prev(kNotFound), kNotFound);
}

TEST(OccupancyBitmapTest, SetResetAndTest) {
    OccupancyBitmap bits(130);
    bits.set(0);
    bits.set(64);
    bits.set(129);
    EXPECT_TRUE(bits.test(0));
    EXPECT_TRUE(bits.test(64));
    EXPECT_TRUE(bits.test(129));
    EXPECT_FALSE(bits.test(63));
    bits.reset(64);
    EXPECT_FALSE(bits.test(64));
    bits.set(5);
    bits.set(5);  // idempotent
    bits.reset(5);
    EXPECT_FALSE(bits.test(5));
}

TEST(OccupancyBitmapTest, FindNextIsInclusiveAndCrossesWords) {
    OccupancyBitmap bits(300);
    bits.set(3);
    bits.set(64);
    bits.set(299);
    EXPECT_EQ(bits.find_next(0), 3U);
    EXPECT_EQ(bits.find_next(3), 3U);  // inclusive
    EXPECT_EQ(bits.find_next(4), 64U);
    EXPECT_EQ(bits.find_next(65), 299U);  // skips whole empty words
    EXPECT_EQ(bits.find_next(300), kNotFound);
    EXPECT_EQ(bits.find_next(kNotFound), kNotFound);
}

TEST(OccupancyBitmapTest, FindPrevIsInclusiveAndCrossesWords) {
    OccupancyBitmap bits(300);
    bits.set(0);
    bits.set(63);
    bits.set(200);
    EXPECT_EQ(bits.find_prev(299), 200U);
    EXPECT_EQ(bits.find_prev(200), 200U);  // inclusive
    EXPECT_EQ(bits.find_prev(199), 63U);
    EXPECT_EQ(bits.find_prev(62), 0U);
    EXPECT_EQ(bits.find_prev(kNotFound), 200U);  // past the end searches from the last bit
    bits.reset(0);
    EXPECT_EQ(bits.find_prev(62), kNotFound);
}

TEST(OccupancyBitmapTest, WordBoundaryBits) {
    OccupancyBitmap bits(128);
    bits.set(63);
    bits.set(64);
    EXPECT_EQ(bits.find_next(0), 63U);
    EXPECT_EQ(bits.find_next(64), 64U);
    EXPECT_EQ(bits.find_prev(127), 64U);
    EXPECT_EQ(bits.find_prev(63), 63U);
}

// Random sets/resets and searches against a std::vector<bool> scanned linearly.
TEST(OccupancyBitmapTest, RandomOperationsMatchLinearScan) {
    for (const std::size_t size : std::vector<std::size_t>{1, 63, 64, 65, 1000}) {
        OccupancyBitmap bits(size);
        std::vector<bool> model(size);
        std::mt19937_64 rng(size);
        for (int step = 0; step < 20'000; ++step) {
            const std::size_t i = rng() % size;
            if (rng() % 2 == 0) {
                bits.set(i);
                model[i] = true;
            } else {
                bits.reset(i);
                model[i] = false;
            }
            const std::size_t from = rng() % (size + 2);  // sometimes past the end
            std::size_t next = kNotFound;
            for (std::size_t j = from; j < size; ++j) {
                if (model[j]) {
                    next = j;
                    break;
                }
            }
            std::size_t prev = kNotFound;
            for (std::size_t j = std::min(from, size - 1) + 1; j-- > 0;) {
                if (model[j]) {
                    prev = j;
                    break;
                }
            }
            ASSERT_EQ(bits.find_next(from), next) << "size " << size << " from " << from;
            ASSERT_EQ(bits.find_prev(from), prev) << "size " << size << " from " << from;
        }
    }
}

}  // namespace
}  // namespace litbook
