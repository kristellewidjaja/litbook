// Tests specific to MapOrderBook. The shared behavior is covered for every book by
// order_book_test.cpp and order_book_rules_test.cpp.

#include "litbook/book/map_order_book.hpp"

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

TEST(MapOrderBookTest, InvalidConfigThrowsAtConstruction) {
    RecordingListener rec;
    EXPECT_THROW(MapOrderBook(rec, BookConfig{.tick_size = 0}), std::invalid_argument);
    EXPECT_THROW(MapOrderBook(rec, BookConfig{.tick_size = -5}), std::invalid_argument);
    EXPECT_THROW(MapOrderBook(rec, BookConfig{.lot_size = 0}), std::invalid_argument);
    EXPECT_THROW(MapOrderBook(rec, BookConfig{.min_price = 10, .max_price = 9}),
                 std::invalid_argument);
    EXPECT_THROW(MapOrderBook(rec, BookConfig{.max_orders = 0}), std::invalid_argument);
}

TEST(MapOrderBookTest, DefaultConfigHasNoBandAndNoPracticalCapacityLimit) {
    RecordingListener rec;
    const MapOrderBook book(rec);
    EXPECT_EQ(book.config().min_price, std::numeric_limits<Price>::min());
    EXPECT_EQ(book.config().max_price, std::numeric_limits<Price>::max());
    EXPECT_EQ(book.config().max_orders, std::numeric_limits<std::uint32_t>::max());
}

// Only the map book can run without a band, so only it can rest at the int64 extremes.
TEST(MapOrderBookTest, ExtremePricesWorkWithoutABand) {
    constexpr Price kMin = std::numeric_limits<Price>::min();
    constexpr Price kMax = std::numeric_limits<Price>::max();
    RecordingListener rec;
    MapOrderBook book(rec);
    book.submit({.id = 1, .side = kBuy, .price = kMin, .qty = 1});
    book.submit({.id = 2, .side = kSell, .price = kMax, .qty = 1});
    EXPECT_EQ(book.best_bid(), kMin);
    EXPECT_EQ(book.best_ask(), kMax);
    rec.clear();
    book.submit({.id = 3, .side = kBuy, .price = kMax, .qty = 1});
    const auto events = rec.take();
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(std::get<Trade>(events[1]).price, kMax);
    EXPECT_TRUE(book.check_invariants());
}

}  // namespace
}  // namespace litbook
