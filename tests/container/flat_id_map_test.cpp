#include "litbook/container/flat_id_map.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace litbook {
namespace {

constexpr auto kNotFound = FlatIdMap::kNotFound;

TEST(FlatIdMapTest, EmptyMapFindsNothing) {
    const FlatIdMap map(10);
    EXPECT_EQ(map.size(), 0U);
    EXPECT_EQ(map.find(1), kNotFound);
    EXPECT_FALSE(map.contains(0));
    EXPECT_TRUE(map.check_invariants());
}

TEST(FlatIdMapTest, TableIsAPowerOfTwoAtLeastTwiceCapacity) {
    EXPECT_EQ(FlatIdMap(1).table_size(), 16U);  // the minimum table size
    EXPECT_EQ(FlatIdMap(8).table_size(), 16U);
    EXPECT_EQ(FlatIdMap(9).table_size(), 32U);
    EXPECT_EQ(FlatIdMap(1000).table_size(), 2048U);
}

TEST(FlatIdMapTest, InsertFindErase) {
    FlatIdMap map(10);
    EXPECT_TRUE(map.insert(42, 7));
    EXPECT_EQ(map.find(42), 7U);
    EXPECT_TRUE(map.contains(42));
    EXPECT_EQ(map.size(), 1U);
    EXPECT_TRUE(map.erase(42));
    EXPECT_EQ(map.find(42), kNotFound);
    EXPECT_EQ(map.size(), 0U);
}

TEST(FlatIdMapTest, DuplicateInsertIsRefusedAndKeepsTheOldValue) {
    FlatIdMap map(10);
    EXPECT_TRUE(map.insert(5, 1));
    EXPECT_FALSE(map.insert(5, 2));
    EXPECT_EQ(map.find(5), 1U);
    EXPECT_EQ(map.size(), 1U);
}

TEST(FlatIdMapTest, EraseOfMissingKeyReturnsFalse) {
    FlatIdMap map(10);
    EXPECT_FALSE(map.erase(5));
    EXPECT_TRUE(map.insert(5, 1));
    EXPECT_FALSE(map.erase(6));
    EXPECT_EQ(map.size(), 1U);
}

TEST(FlatIdMapTest, InsertBeyondMaxEntriesIsRefused) {
    FlatIdMap map(3);
    EXPECT_TRUE(map.insert(1, 1));
    EXPECT_TRUE(map.insert(2, 2));
    EXPECT_TRUE(map.insert(3, 3));
    EXPECT_FALSE(map.insert(4, 4));
    EXPECT_TRUE(map.erase(2));
    EXPECT_TRUE(map.insert(4, 4));  // an erase makes room again
}

TEST(FlatIdMapTest, ExtremeKeysWork) {
    FlatIdMap map(4);
    EXPECT_TRUE(map.insert(0, 1));
    EXPECT_TRUE(map.insert(std::numeric_limits<std::uint64_t>::max(), 2));
    EXPECT_EQ(map.find(0), 1U);
    EXPECT_EQ(map.find(std::numeric_limits<std::uint64_t>::max()), 2U);
}

TEST(FlatIdMapTest, InvalidCapacityThrows) {
    EXPECT_THROW(FlatIdMap{0}, std::invalid_argument);
}

// Fill a tiny table completely (16 slots, 8 keys) so keys collide and probe runs wrap
// around the end of the array, then erase in every position of those runs.
TEST(FlatIdMapTest, FullTinyTableWithCollisionsAndWraparound) {
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        FlatIdMap map(8);
        std::mt19937_64 rng(seed);
        std::vector<std::uint64_t> keys;
        while (keys.size() < 8) {
            const std::uint64_t k = rng() % 64;
            if (map.insert(k, static_cast<std::uint32_t>(keys.size()))) {
                keys.push_back(k);
            }
        }
        ASSERT_TRUE(map.check_invariants());
        std::shuffle(keys.begin(), keys.end(), rng);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            ASSERT_TRUE(map.erase(keys[i]));
            ASSERT_TRUE(map.check_invariants()) << "seed " << seed;
            for (std::size_t j = i + 1; j < keys.size(); ++j) {
                ASSERT_TRUE(map.contains(keys[j])) << "seed " << seed << " lost key " << keys[j];
            }
        }
        ASSERT_EQ(map.size(), 0U);
    }
}

// Keys with a large power-of-two stride are the worst case for a hash that only keeps the
// low bits of the key. Every key would land in the same home slot and form one long run.
// The hash must spread them, so lookups stay short.
TEST(FlatIdMapTest, StridedKeysDoNotPileUp) {
    for (const unsigned stride_bits : {10U, 16U, 20U, 32U}) {
        FlatIdMap map(4'096);
        for (FlatIdMap::Key k = 1; k <= 4'096; ++k) {
            ASSERT_TRUE(map.insert(k << stride_bits, static_cast<FlatIdMap::Value>(k)));
        }
        EXPECT_LT(map.longest_probe(), 64U) << "stride 2^" << stride_bits;
        EXPECT_TRUE(map.check_invariants());
    }
}

// Sequential keys, the usual shape of order IDs, also stay short.
TEST(FlatIdMapTest, SequentialKeysDoNotPileUp) {
    FlatIdMap map(100'000);
    for (FlatIdMap::Key k = 1'000'000; k < 1'100'000; ++k) {
        ASSERT_TRUE(map.insert(k, static_cast<FlatIdMap::Value>(k % 1'000)));
    }
    EXPECT_LT(map.longest_probe(), 64U);
}

// Regression test. Sequential keys each get their own home slot, so a few hundred live
// orders with nearby IDs form one long run of slots. Erasing a key near the front of that
// run used to read every slot to the end of the run, even though no key had to move. A
// load test that canceled its oldest orders spent most of the matching thread's time
// there. An erase must stop as soon as the next key already sits in its home slot.
TEST(FlatIdMapTest, EraseInARunOfSequentialKeysIsShort) {
    constexpr FlatIdMap::Key kFirst = 1'000'000;
    constexpr FlatIdMap::Key kCount = 1'000;
    FlatIdMap map(4'096);
    for (FlatIdMap::Key k = kFirst; k < kFirst + kCount; ++k) {
        ASSERT_TRUE(map.insert(k, static_cast<FlatIdMap::Value>(k - kFirst)));
    }
    ASSERT_EQ(map.longest_probe(), 0U);  // One run, every key in its home slot.

    ASSERT_TRUE(map.erase(kFirst));
    EXPECT_LE(map.erase_walk(), 1U);

    // Taking the oldest key each time, as a client canceling its oldest orders does, stays
    // short for every erase.
    for (FlatIdMap::Key k = kFirst + 1; k < kFirst + kCount; ++k) {
        ASSERT_TRUE(map.erase(k));
    }
    EXPECT_LE(map.erase_walk(), kCount);
    EXPECT_EQ(map.size(), 0U);
    EXPECT_TRUE(map.check_invariants());
}

// Random insert/erase/find against std::unordered_map, with keys from a small range
// so the same keys come and go many times (the backward-shift erase is exercised hard).
TEST(FlatIdMapTest, RandomChurnMatchesUnorderedMap) {
    constexpr std::size_t kMax = 100;
    FlatIdMap map(kMax);
    std::unordered_map<std::uint64_t, std::uint32_t> model;
    std::mt19937_64 rng(11);
    for (std::uint32_t step = 0; step < 200'000; ++step) {
        const std::uint64_t key = rng() % 300;
        switch (rng() % 3) {
            case 0: {
                const bool expected = model.size() < kMax && !model.contains(key);
                ASSERT_EQ(map.insert(key, step), expected);
                if (expected) {
                    model[key] = step;
                }
                break;
            }
            case 1:
                ASSERT_EQ(map.erase(key), model.erase(key) == 1);
                break;
            default: {
                const auto it = model.find(key);
                ASSERT_EQ(map.find(key), it == model.end() ? kNotFound : it->second);
            }
        }
        ASSERT_EQ(map.size(), model.size());
        if (step % 1000 == 0) {
            ASSERT_TRUE(map.check_invariants()) << "step " << step;
        }
    }
    ASSERT_TRUE(map.check_invariants());
}

}  // namespace
}  // namespace litbook
