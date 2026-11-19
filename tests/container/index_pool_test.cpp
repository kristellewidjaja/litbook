#include "litbook/container/index_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

namespace litbook {
namespace {

using Pool = IndexPool<int>;

TEST(IndexPoolTest, StartsEmptyWithAllSlotsFree) {
    const Pool pool(4);
    EXPECT_EQ(pool.capacity(), 4U);
    EXPECT_EQ(pool.in_use(), 0U);
    EXPECT_FALSE(pool.full());
}

TEST(IndexPoolTest, HandsOutIndexesInAscendingOrderFirst) {
    Pool pool(3);
    EXPECT_EQ(pool.acquire(), 0U);
    EXPECT_EQ(pool.acquire(), 1U);
    EXPECT_EQ(pool.acquire(), 2U);
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.in_use(), 3U);
}

TEST(IndexPoolTest, ReturnsNoneWhenFull) {
    Pool pool(1);
    EXPECT_EQ(pool.acquire(), 0U);
    EXPECT_EQ(pool.acquire(), Pool::kNone);
    EXPECT_EQ(pool.in_use(), 1U);
}

TEST(IndexPoolTest, ReusesTheMostRecentlyReleasedSlotFirst) {
    Pool pool(4);
    for (int i = 0; i < 4; ++i) {
        (void)pool.acquire();
    }
    pool.release(1);
    pool.release(3);
    EXPECT_EQ(pool.acquire(), 3U);  // LIFO: the slot most likely still in cache
    EXPECT_EQ(pool.acquire(), 1U);
    EXPECT_EQ(pool.acquire(), Pool::kNone);
}

TEST(IndexPoolTest, SlotsAreWritableAndKeepTheirValue) {
    Pool pool(2);
    const auto a = pool.acquire();
    const auto b = pool.acquire();
    pool[a] = 10;
    pool[b] = 20;
    const Pool& view = pool;
    EXPECT_EQ(view[a], 10);
    EXPECT_EQ(view[b], 20);
}

TEST(IndexPoolTest, InvalidCapacityThrows) {
    EXPECT_THROW(Pool{0}, std::invalid_argument);  // braces: "Pool(x)" can parse as a declaration
    EXPECT_THROW(Pool{Pool::kNone}, std::invalid_argument);
}

// Random acquire/release against a std::set of live indexes.
TEST(IndexPoolTest, RandomChurnNeverDuplicatesOrLosesSlots) {
    constexpr std::uint32_t kCapacity = 64;
    Pool pool(kCapacity);
    std::set<std::uint32_t> live;
    std::mt19937_64 rng(7);
    for (int step = 0; step < 100'000; ++step) {
        if (live.empty() || (rng() % 2 == 0 && live.size() < kCapacity)) {
            const auto i = pool.acquire();
            ASSERT_LT(i, kCapacity);
            ASSERT_TRUE(live.insert(i).second) << "slot " << i << " handed out twice";
        } else if (rng() % 3 == 0 && live.size() == kCapacity) {
            ASSERT_EQ(pool.acquire(), Pool::kNone);
        } else {
            auto it = live.begin();
            std::advance(it, static_cast<long>(rng() % live.size()));
            pool.release(*it);
            live.erase(it);
        }
        ASSERT_EQ(pool.in_use(), live.size());
        ASSERT_EQ(pool.full(), live.size() == kCapacity);
    }
}

}  // namespace
}  // namespace litbook
