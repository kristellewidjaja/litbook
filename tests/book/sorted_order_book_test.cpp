// Tests specific to SortedOrderBook. Its shared behavior is covered for every book by
// order_book_test.cpp, order_book_rules_test.cpp, the model test and the differential
// test. These tests cover what is different about it. It needs no price band, it finds
// prices with a short scan plus binary search, and it creates levels anywhere in its
// sorted arrays.

#include "litbook/book/sorted_order_book.hpp"

#include "litbook/book/book_config.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

namespace litbook {
namespace {

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;

TEST(SortedOrderBookTest, InvalidConfigThrowsAtConstruction) {
    RecordingListener rec;
    const auto make = [&](BookConfig c) { return SortedOrderBook(rec, c); };
    EXPECT_THROW(make({.tick_size = 0, .max_orders = 4}), std::invalid_argument);
    EXPECT_THROW(make({.lot_size = 0, .max_orders = 4}), std::invalid_argument);
    EXPECT_THROW(make({.min_price = 10, .max_price = 9, .max_orders = 4}), std::invalid_argument);
    EXPECT_THROW(make({.max_orders = 0}), std::invalid_argument);
    EXPECT_THROW(make({}), std::invalid_argument);  // max_orders defaults to unlimited
    EXPECT_THROW(make({.max_orders = SortedOrderBook::kMaxOrders + 1}), std::invalid_argument);
    EXPECT_NO_THROW(make({.max_orders = 4}));  // no band is needed
}

// The prices a real Nasdaq day contains, in Price (4) units: stub quotes at $0.0001 and
// $199,999.99 next to normal orders around $300. No band is set at all.
TEST(SortedOrderBookTest, HoldsStubQuotesFarFromTheMarket) {
    RecordingListener rec;
    SortedOrderBook book(rec, {.max_orders = 16});
    book.submit({.id = 1, .side = kBuy, .price = 1, .qty = 100});               // $0.0001
    book.submit({.id = 2, .side = kSell, .price = 1'999'999'900, .qty = 100});  // $199,999.99
    book.submit({.id = 3, .side = kBuy, .price = 2'999'900, .qty = 100});       // $299.99
    book.submit({.id = 4, .side = kSell, .price = 3'000'100, .qty = 100});      // $300.01
    EXPECT_EQ(book.best_bid(), 2'999'900);
    EXPECT_EQ(book.best_ask(), 3'000'100);
    EXPECT_EQ(book.level_count(kBuy), 2U);
    EXPECT_EQ(book.volume_at(kSell, 1'999'999'900), 100U);

    // A market sell sweeps the real bid and then the stub at $0.0001.
    book.submit({.id = 5, .side = kSell, .type = OrderType::Market, .qty = 200});
    EXPECT_EQ(book.best_bid(), std::nullopt);
    EXPECT_TRUE(book.check_invariants());
}

TEST(SortedOrderBookTest, ExtremeInt64PricesWork) {
    RecordingListener rec;
    SortedOrderBook book(rec, {.max_orders = 8});
    constexpr Price kMin = std::numeric_limits<Price>::min();
    constexpr Price kMax = std::numeric_limits<Price>::max();
    book.submit({.id = 1, .side = kBuy, .price = kMin, .qty = 1});
    book.submit({.id = 2, .side = kSell, .price = kMax, .qty = 1});
    book.submit({.id = 3, .side = kBuy, .price = 0, .qty = 1});
    EXPECT_EQ(book.best_bid(), 0);
    EXPECT_EQ(book.best_ask(), kMax);
    book.cancel(3);
    EXPECT_EQ(book.best_bid(), kMin);
    EXPECT_TRUE(book.check_invariants());
}

// New levels arrive at the best end, the worst end and in the middle of the array, and
// leave from all three, while the array has to stay sorted.
TEST(SortedOrderBookTest, LevelsAreInsertedAndRemovedAnywhere) {
    RecordingListener rec;
    SortedOrderBook book(rec, {.max_orders = 64});
    OrderId id = 1;
    for (const Price p : {100, 50, 75, 99, 1, 60, 80, 55}) {  // best, worst and middle
        book.submit({.id = id++, .side = kBuy, .price = p, .qty = 1});
        ASSERT_TRUE(book.check_invariants()) << "after adding " << p;
    }
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.level_count(kBuy), 8U);
    for (const OrderId cancel :
         {OrderId{6}, OrderId{1}, OrderId{3}, OrderId{8}}) {  // 60 (middle), 100 (best), 75, 55
        book.cancel(cancel);
        ASSERT_TRUE(book.check_invariants()) << "after canceling " << cancel;
    }
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.level_count(kBuy), 4U);
    const std::vector<LevelSummary> depth = book.depth(kBuy, 10);
    ASSERT_EQ(depth.size(), 4U);
    EXPECT_EQ(depth[0].price, 99);
    EXPECT_EQ(depth[1].price, 80);
    EXPECT_EQ(depth[2].price, 50);
    EXPECT_EQ(depth[3].price, 1);
}

// With thousands of levels, most lookups go past the short scan at the best end and use
// binary search. Random adds and cancels at random depths are checked against
// MapOrderBook after every operation.
TEST(SortedOrderBookTest, ManyLevelsMatchTheMapBook) {
    RecordingListener sorted_rec;
    RecordingListener map_rec;
    const BookConfig config{.max_orders = 8'192};
    SortedOrderBook sorted(sorted_rec, config);
    MapOrderBook map(map_rec, config);
    std::mt19937_64 rng(5);
    std::vector<OrderId> live;
    OrderId next_id = 1;
    for (int step = 0; step < 40'000; ++step) {
        if (!live.empty() && rng() % 3 == 0) {
            const std::size_t i = rng() % live.size();
            sorted.cancel(live[i]);
            map.cancel(live[i]);
            live[i] = live.back();
            live.pop_back();
        } else {
            // Bids at 0..4999 and asks at 5000..9999, so the sides never cross and each
            // side holds up to 5,000 levels.
            const bool buy = rng() % 2 == 0;
            const auto offset = static_cast<Price>(rng() % 5'000);
            const OrderRequest r{.id = next_id++,
                                 .side = buy ? kBuy : kSell,
                                 .price = buy ? offset : 5'000 + offset,
                                 .qty = static_cast<Qty>(1 + rng() % 10)};
            sorted.submit(r);
            map.submit(r);
            live.push_back(r.id);
        }
        ASSERT_EQ(sorted_rec.events(), map_rec.events()) << "step " << step;
        sorted_rec.clear();
        map_rec.clear();
        if (step % 1'000 == 0) {
            ASSERT_TRUE(sorted.check_invariants()) << "step " << step;
            for (const Side side : {kBuy, kSell}) {
                ASSERT_EQ(sorted.depth(side, 5'000), map.depth(side, 5'000)) << "step " << step;
            }
        }
    }
    EXPECT_GT(sorted.level_count(kBuy), 1'000U);  // the binary search path really ran
    EXPECT_TRUE(sorted.check_invariants());
}

TEST(SortedOrderBookTest, VolumeAtAMissingPriceIsZero) {
    RecordingListener rec;
    SortedOrderBook book(rec, {.max_orders = 8});
    EXPECT_EQ(book.volume_at(kBuy, 100), 0U);
    book.submit({.id = 1, .side = kBuy, .price = 100, .qty = 5});
    EXPECT_EQ(book.volume_at(kBuy, 100), 5U);
    EXPECT_EQ(book.volume_at(kBuy, 101), 0U);
    EXPECT_EQ(book.volume_at(kSell, 100), 0U);
}

// The level pool is as large as the order pool. Filling the book with one order per
// level and emptying it again, many times over, must never leak a level slot.
TEST(SortedOrderBookTest, LevelSlotsAreRecycled) {
    RecordingListener rec;
    SortedOrderBook book(rec, {.max_orders = 4});
    OrderId id = 1;
    for (int round = 0; round < 1'000; ++round) {
        const OrderId first = id;
        for (Price p = 10; p < 14; ++p) {
            book.submit({.id = id++, .side = kBuy, .price = p + round % 7, .qty = 1});
        }
        ASSERT_EQ(book.level_count(kBuy), 4U);
        book.cancel(first + 2);
        book.submit({.id = id++, .side = kSell, .price = 0, .qty = 3});  // fills the other 3
        ASSERT_EQ(book.order_count(), 0U);
        ASSERT_EQ(book.level_count(kBuy), 0U);
        ASSERT_TRUE(book.check_invariants()) << "round " << round;
    }
}

TEST(SortedOrderBookTest, MoveAssignmentGivesAFreshBook) {
    RecordingListener rec;
    SortedOrderBook book(rec, {.max_orders = 8});
    book.submit({.id = 1, .side = kBuy, .price = 10, .qty = 1});
    book = SortedOrderBook(rec, {.max_orders = 2});
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.config().max_orders, 2U);
    book.submit({.id = 1, .side = kBuy, .price = -10, .qty = 1});
    EXPECT_EQ(book.best_bid(), -10);
    EXPECT_TRUE(book.check_invariants());
}

}  // namespace
}  // namespace litbook
