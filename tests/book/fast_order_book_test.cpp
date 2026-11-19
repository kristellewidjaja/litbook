// These tests are specific to FastOrderBook. They cover construction limits, ladder edges,
// best-price tracking across empty levels, and pool reuse. The behavior shared by every book
// is covered by order_book_test.cpp and order_book_rules_test.cpp.

#include "litbook/book/fast_order_book.hpp"

#include "litbook/book/book_config.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

namespace litbook {
namespace {

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;
constexpr Price kInt64Min = std::numeric_limits<Price>::min();
constexpr Price kInt64Max = std::numeric_limits<Price>::max();

constexpr BookConfig kSmall{.min_price = 0, .max_price = 1'000, .max_orders = 16};

TEST(FastOrderBookTest, InvalidConfigThrowsAtConstruction) {
    RecordingListener rec;
    const auto make = [&](BookConfig c) { return FastOrderBook(rec, c); };
    // First come the rules that every book shares.
    EXPECT_THROW(make({.tick_size = 0, .min_price = 0, .max_price = 10, .max_orders = 1}),
                 std::invalid_argument);
    EXPECT_THROW(make({.lot_size = 0, .min_price = 0, .max_price = 10, .max_orders = 1}),
                 std::invalid_argument);
    EXPECT_THROW(make({.min_price = 10, .max_price = 9, .max_orders = 1}), std::invalid_argument);
    EXPECT_THROW(make({.min_price = 0, .max_price = 10, .max_orders = 0}), std::invalid_argument);
    // Then come FastOrderBook's own rules. The band must be finite, on tick and not too
    // wide, and the pool must be bounded.
    EXPECT_THROW(make({}), std::invalid_argument);  // the default band is unbounded
    EXPECT_THROW(make({.tick_size = 5, .min_price = 3, .max_price = 100, .max_orders = 1}),
                 std::invalid_argument);
    EXPECT_THROW(make({.tick_size = 5, .min_price = 0, .max_price = 101, .max_orders = 1}),
                 std::invalid_argument);
    EXPECT_THROW(make({.min_price = 0,
                       .max_price = static_cast<Price>(FastOrderBook::kMaxLevels),
                       .max_orders = 1}),
                 std::invalid_argument);  // kMaxLevels + 1 levels
    EXPECT_THROW(
        make({.min_price = 0, .max_price = 10, .max_orders = FastOrderBook::kMaxOrders + 1}),
        std::invalid_argument);
}

TEST(FastOrderBookTest, SingleLevelBandWorks) {
    RecordingListener rec;
    FastOrderBook book(rec, {.min_price = 50, .max_price = 50, .max_orders = 4});
    book.submit({.id = 1, .side = kBuy, .price = 50, .qty = 3});
    book.submit({.id = 2, .side = kSell, .price = 50, .qty = 1});
    EXPECT_EQ(book.best_bid(), 50);
    EXPECT_EQ(book.volume_at(kBuy, 50), 2U);
    EXPECT_TRUE(book.check_invariants());
}

TEST(FastOrderBookTest, OrdersAtBothEndsOfTheLadder) {
    RecordingListener rec;
    FastOrderBook book(rec, kSmall);
    book.submit({.id = 1, .side = kBuy, .price = 0, .qty = 1});       // index 0
    book.submit({.id = 2, .side = kSell, .price = 1'000, .qty = 1});  // last index
    EXPECT_EQ(book.best_bid(), 0);
    EXPECT_EQ(book.best_ask(), 1'000);
    book.cancel(1);
    book.cancel(2);
    EXPECT_EQ(book.best_bid(), std::nullopt);  // moving past index 0 must not wrap around
    EXPECT_EQ(book.best_ask(), std::nullopt);  // moving past the last index must stop
    EXPECT_TRUE(book.check_invariants());
}

TEST(FastOrderBookTest, MarketOrderSweepsTheWholeLadder) {
    RecordingListener rec;
    FastOrderBook book(rec, kSmall);
    book.submit({.id = 1, .side = kBuy, .price = 0, .qty = 1});
    book.submit({.id = 2, .side = kBuy, .price = 1'000, .qty = 1});
    book.submit({.id = 3, .side = kSell, .type = OrderType::Market, .qty = 2});
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_TRUE(book.check_invariants());
}

// The band sits at the very bottom or top of int64. Index and price arithmetic must not
// overflow (it is done in unsigned 64-bit).
TEST(FastOrderBookTest, BandAtTheInt64Extremes) {
    RecordingListener rec;
    FastOrderBook low(
        rec,
        {.tick_size = 4, .min_price = kInt64Min, .max_price = kInt64Min + 400, .max_orders = 8});
    low.submit({.id = 1, .side = kBuy, .price = kInt64Min, .qty = 1});
    low.submit({.id = 2, .side = kSell, .price = kInt64Min + 400, .qty = 1});
    EXPECT_EQ(low.best_bid(), kInt64Min);
    EXPECT_EQ(low.best_ask(), kInt64Min + 400);
    EXPECT_EQ(low.volume_at(kBuy, kInt64Min), 1U);
    EXPECT_TRUE(low.check_invariants());

    constexpr Price kTop = kInt64Max - 3;  // the largest multiple of 4
    FastOrderBook high(
        rec, {.tick_size = 4, .min_price = kTop - 400, .max_price = kTop, .max_orders = 8});
    high.submit({.id = 1, .side = kSell, .price = kTop, .qty = 1});
    rec.clear();
    high.submit({.id = 2, .side = kBuy, .price = kTop, .qty = 1});
    const auto events = rec.take();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(std::get<Trade>(events[1]).price, kTop);
    EXPECT_EQ(high.volume_at(kBuy, kInt64Max), 0U);  // off tick, so no level and no crash
    EXPECT_TRUE(high.check_invariants());
}

// After the best level empties, the best price jumps over a long run of empty levels
// (many 64-bit bitmap words) to the next occupied one.
TEST(FastOrderBookTest, BestPriceSkipsLongGapsOfEmptyLevels) {
    RecordingListener rec;
    FastOrderBook book(rec, {.min_price = 0, .max_price = 100'000, .max_orders = 8});
    book.submit({.id = 1, .side = kBuy, .price = 3, .qty = 1});
    book.submit({.id = 2, .side = kBuy, .price = 99'000, .qty = 1});
    book.submit({.id = 3, .side = kSell, .price = 99'500, .qty = 1});
    book.submit({.id = 4, .side = kSell, .price = 100'000, .qty = 1});
    book.cancel(2);
    EXPECT_EQ(book.best_bid(), 3);
    book.cancel(3);
    EXPECT_EQ(book.best_ask(), 100'000);
    EXPECT_EQ(book.level_count(kBuy), 1U);
    EXPECT_EQ(book.level_count(kSell), 1U);
    EXPECT_TRUE(book.check_invariants());
}

TEST(FastOrderBookTest, VolumeAtOutsideTheBandOrOffTickIsZero) {
    RecordingListener rec;
    const FastOrderBook book(rec,
                             {.tick_size = 5, .min_price = 0, .max_price = 100, .max_orders = 4});
    EXPECT_EQ(book.volume_at(kBuy, -5), 0U);
    EXPECT_EQ(book.volume_at(kBuy, 105), 0U);
    EXPECT_EQ(book.volume_at(kBuy, 7), 0U);
    EXPECT_EQ(book.volume_at(kBuy, kInt64Min), 0U);
}

// Many times the pool's capacity of orders come and go, and the slots must be recycled
// correctly.
TEST(FastOrderBookTest, PoolSlotsAreRecycledUnderChurn) {
    RecordingListener rec;
    FastOrderBook book(rec, {.min_price = 0, .max_price = 100, .max_orders = 4});
    OrderId id = 1;
    for (int round = 0; round < 1'000; ++round) {
        const OrderId first = id;
        for (int i = 0; i < 4; ++i) {
            book.submit({.id = id++, .side = kBuy, .price = 10 + i, .qty = 1});
        }
        ASSERT_EQ(book.order_count(), 4U);
        book.cancel(first + 1);
        book.submit({.id = id++, .side = kSell, .price = 0, .qty = 3});  // fills 3 bids
        ASSERT_EQ(book.order_count(), 0U);
        ASSERT_TRUE(book.check_invariants()) << "round " << round;
    }
    rec.clear();
    book.cancel(1);
    EXPECT_EQ(rec.take().size(), 1U);  // long gone, so CancelRejected
}

TEST(FastOrderBookTest, MoveAssignmentGivesAFreshBook) {
    RecordingListener rec;
    FastOrderBook book(rec, kSmall);
    book.submit({.id = 1, .side = kBuy, .price = 10, .qty = 1});
    book = FastOrderBook(rec, {.min_price = -10, .max_price = 10, .max_orders = 2});
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.config().min_price, -10);
    book.submit({.id = 1, .side = kBuy, .price = -10, .qty = 1});
    EXPECT_EQ(book.best_bid(), -10);
    EXPECT_TRUE(book.check_invariants());
}

}  // namespace
}  // namespace litbook
