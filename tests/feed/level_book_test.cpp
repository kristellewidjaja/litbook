#include "litbook/feed/level_book.hpp"

#include <gtest/gtest.h>

#include <map>
#include <random>

namespace litbook::feed {
namespace {

TEST(LevelBookTest, StartsEmpty) {
    const LevelBook book(4);
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.level_count(Side::Buy), 0U);
    EXPECT_FALSE(book.best(Side::Sell).has_value());
    EXPECT_EQ(book.max_levels_per_side(), 4U);
    EXPECT_TRUE(book.check_invariants());
}

TEST(LevelBookTest, KeepsEachSideFromBestToWorst) {
    LevelBook book(8);
    EXPECT_TRUE(book.set(Side::Buy, 100, 10, 1));
    EXPECT_TRUE(book.set(Side::Buy, 102, 20, 2));
    EXPECT_TRUE(book.set(Side::Buy, 101, 30, 3));
    EXPECT_TRUE(book.set(Side::Sell, 105, 5, 1));
    EXPECT_TRUE(book.set(Side::Sell, 103, 6, 1));
    EXPECT_TRUE(book.set(Side::Sell, 104, 7, 1));

    ASSERT_EQ(book.level_count(Side::Buy), 3U);
    EXPECT_EQ(book.level(Side::Buy, 0).price, 102);
    EXPECT_EQ(book.level(Side::Buy, 1).price, 101);
    EXPECT_EQ(book.level(Side::Buy, 2).price, 100);
    ASSERT_EQ(book.level_count(Side::Sell), 3U);
    EXPECT_EQ(book.level(Side::Sell, 0).price, 103);
    EXPECT_EQ(book.level(Side::Sell, 2).price, 105);
    EXPECT_EQ(book.best(Side::Buy), (PriceLevel{.price = 102, .total_qty = 20, .order_count = 2}));
    EXPECT_EQ(book.best(Side::Sell), (PriceLevel{.price = 103, .total_qty = 6, .order_count = 1}));
    EXPECT_TRUE(book.check_invariants());
}

TEST(LevelBookTest, SetReplacesAndZeroRemoves) {
    LevelBook book(4);
    ASSERT_TRUE(book.set(Side::Buy, 100, 10, 1));
    ASSERT_TRUE(book.set(Side::Buy, 100, 25, 3));
    EXPECT_EQ(book.find(Side::Buy, 100),
              (PriceLevel{.price = 100, .total_qty = 25, .order_count = 3}));
    EXPECT_EQ(book.level_count(Side::Buy), 1U);

    EXPECT_TRUE(book.set(Side::Buy, 100, 0, 0));
    EXPECT_FALSE(book.find(Side::Buy, 100).has_value());
    EXPECT_TRUE(book.empty());
    // Removing a level that is not there is harmless, because updates carry new states.
    EXPECT_TRUE(book.set(Side::Sell, 200, 0, 0));
    EXPECT_TRUE(book.empty());
}

TEST(LevelBookTest, RefusesAHalfEmptyLevel) {
    LevelBook book(4);
    EXPECT_FALSE(book.set(Side::Buy, 100, 10, 0));
    EXPECT_FALSE(book.set(Side::Buy, 100, 0, 1));
    EXPECT_TRUE(book.empty());
}

TEST(LevelBookTest, AFullSideRefusesNewLevelsButAcceptsChanges) {
    LevelBook book(2);
    ASSERT_TRUE(book.set(Side::Sell, 100, 1, 1));
    ASSERT_TRUE(book.set(Side::Sell, 101, 1, 1));
    EXPECT_FALSE(book.set(Side::Sell, 102, 1, 1));
    EXPECT_FALSE(book.find(Side::Sell, 102).has_value());
    EXPECT_TRUE(book.set(Side::Sell, 101, 9, 2));
    EXPECT_TRUE(book.set(Side::Buy, 99, 1, 1));  // The other side has its own room.
    EXPECT_TRUE(book.set(Side::Sell, 100, 0, 0));
    EXPECT_TRUE(book.set(Side::Sell, 102, 1, 1));
    EXPECT_TRUE(book.check_invariants());
}

TEST(LevelBookTest, CopyFromAndEquality) {
    LevelBook a(4);
    ASSERT_TRUE(a.set(Side::Buy, 10, 1, 1));
    ASSERT_TRUE(a.set(Side::Sell, 12, 2, 1));
    LevelBook b(8);
    ASSERT_TRUE(b.set(Side::Buy, 5, 5, 5));
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(b.copy_from(a));
    EXPECT_TRUE(a == b);
    EXPECT_EQ(b.max_levels_per_side(), 8U);

    LevelBook small(1);
    ASSERT_TRUE(b.set(Side::Buy, 11, 1, 1));
    ASSERT_TRUE(small.set(Side::Sell, 1, 1, 1));
    EXPECT_FALSE(small.copy_from(b));
    EXPECT_EQ(small.level_count(Side::Sell), 1U);  // Unchanged after a refused copy.

    b.clear();
    EXPECT_TRUE(b.empty());
}

// Random updates against a std::map per side as the model.
TEST(LevelBookTest, MatchesASimpleModelOverRandomUpdates) {
    LevelBook book(64);
    std::map<Price, PriceLevel> bids;
    std::map<Price, PriceLevel> asks;
    std::mt19937_64 rng(5);
    int removals = 0;
    int refusals = 0;
    for (int i = 0; i < 20'000; ++i) {
        const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        auto& model = side == Side::Buy ? bids : asks;
        const auto price = static_cast<Price>(rng() % 100);
        const bool remove = rng() % 3 == 0;
        const AggregateQty qty = remove ? 0 : 1 + rng() % 1000;
        const std::uint32_t count = remove ? 0 : static_cast<std::uint32_t>(1 + rng() % 9);
        const bool fits = remove || model.contains(price) || model.size() < 64;
        EXPECT_EQ(book.set(side, price, qty, count), fits);
        if (!fits) {
            ++refusals;
            continue;
        }
        if (remove) {
            removals += model.erase(price) > 0 ? 1 : 0;
        } else {
            model[price] = PriceLevel{.price = price, .total_qty = qty, .order_count = count};
        }
        ASSERT_EQ(book.level_count(Side::Buy), bids.size());
        ASSERT_EQ(book.level_count(Side::Sell), asks.size());
    }
    std::size_t rank = 0;
    for (auto it = bids.rbegin(); it != bids.rend(); ++it, ++rank) {
        EXPECT_EQ(book.level(Side::Buy, rank), it->second);
    }
    rank = 0;
    for (const auto& [price, level] : asks) {
        EXPECT_EQ(book.level(Side::Sell, rank++), level);
    }
    EXPECT_TRUE(book.check_invariants());
    EXPECT_GT(removals, 1000);
    EXPECT_GT(refusals, 0);  // Some updates found a full side, so that path ran.
}

}  // namespace
}  // namespace litbook::feed
