// Tests for ItchBookBuilder, which turns decoded ITCH messages into calls on one book
// per stock. The messages are built as structs here, because the byte-level decoding is
// already covered by itch_parser_test.cpp.

#include "litbook/itch/book_builder.hpp"

#include "litbook/book/map_order_book.hpp"
#include "litbook/book/recording_listener.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/itch/itch_messages.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <variant>
#include <vector>

namespace litbook::itch {
namespace {

constexpr std::uint16_t kAapl = 13;
constexpr std::uint16_t kUntracked = 99;

AddOrder add(std::uint16_t locate, std::uint64_t ref, char side, std::uint32_t shares,
             std::uint32_t price) {
    return {
        .header = {.type = 'A', .stock_locate = locate, .tracking_number = 0, .timestamp_ns = 0},
        .order_ref = ref,
        .side = side,
        .shares = shares,
        .stock = {},
        .price = price,
        .has_mpid = false,
        .mpid = {}};
}
Header header(char type) {
    return {.type = type, .stock_locate = kAapl, .tracking_number = 0, .timestamp_ns = 0};
}

// Counts how many times the builder wrapped a group of book calls.
struct CountCalls {
    int* calls;
    template <class F>
    void operator()(F&& f) const {
        ++*calls;
        f();
    }
};

class BookBuilderTest : public ::testing::Test {
protected:
    BookBuilderTest() { builder_.add_book(kAapl, rec_, BookConfig{.max_orders = 64}); }

    MapOrderBook& book() { return *builder_.book(kAapl); }

    RecordingListener rec_;
    ItchBookBuilder<MapOrderBook> builder_{64};
};

TEST_F(BookBuilderTest, AddsBecomeRestingOrders) {
    builder_.on_add_order(add(kAapl, 1, 'B', 100, 2'934'500));
    builder_.on_add_order(add(kAapl, 2, 'S', 50, 2'935'000));
    EXPECT_EQ(book().best_bid(), 2'934'500);
    EXPECT_EQ(book().best_ask(), 2'935'000);
    EXPECT_EQ(book().volume_at(Side::Buy, 2'934'500), 100U);
    EXPECT_EQ(builder_.live_orders(), 2U);
    EXPECT_EQ(builder_.book_messages(), 2U);
}

TEST_F(BookBuilderTest, MessagesForStocksWithoutABookAreIgnored) {
    builder_.on_add_order(add(kUntracked, 1, 'B', 100, 10'000));
    builder_.on_order_delete({.header = header('D'), .order_ref = 1});
    EXPECT_EQ(builder_.live_orders(), 0U);
    EXPECT_EQ(builder_.book_messages(), 0U);
    EXPECT_TRUE(rec_.events().empty());
}

// A partial execution is a size reduction that keeps the order's place in line.
TEST_F(BookBuilderTest, PartialExecutionShrinksTheOrderAndKeepsPriority) {
    builder_.on_add_order(add(kAapl, 1, 'B', 100, 10'000));
    rec_.clear();
    builder_.on_order_executed(
        {.header = header('E'), .order_ref = 1, .executed_shares = 30, .match_number = 1});
    EXPECT_EQ(book().volume_at(Side::Buy, 10'000), 70U);
    const auto events = rec_.take();
    ASSERT_FALSE(events.empty());
    const auto* modified = std::get_if<OrderModified>(&events[0]);
    ASSERT_NE(modified, nullptr);
    EXPECT_TRUE(modified->kept_priority);
    EXPECT_EQ(modified->qty, 70U);
}

TEST_F(BookBuilderTest, FullExecutionRemovesTheOrder) {
    builder_.on_add_order(add(kAapl, 1, 'S', 100, 10'000));
    builder_.on_order_executed(
        {.header = header('E'), .order_ref = 1, .executed_shares = 60, .match_number = 1});
    builder_.on_order_executed_with_price({.header = header('C'),
                                           .order_ref = 1,
                                           .executed_shares = 40,
                                           .match_number = 2,
                                           .printable = 'Y',
                                           .execution_price = 9'900});
    EXPECT_EQ(book().best_ask(), std::nullopt);
    EXPECT_EQ(builder_.live_orders(), 0U);
    EXPECT_TRUE(book().check_invariants());
}

TEST_F(BookBuilderTest, PartialCancelAndDelete) {
    builder_.on_add_order(add(kAapl, 1, 'B', 100, 10'000));
    builder_.on_order_cancel({.header = header('X'), .order_ref = 1, .canceled_shares = 25});
    EXPECT_EQ(book().volume_at(Side::Buy, 10'000), 75U);
    builder_.on_order_delete({.header = header('D'), .order_ref = 1});
    EXPECT_EQ(book().order_count(), 0U);
    EXPECT_EQ(builder_.live_orders(), 0U);
}

// A replace removes the old order and adds a new one with a new reference, on the same
// side, which loses its place in line.
TEST_F(BookBuilderTest, ReplaceMovesTheOrderToANewReferenceAndPrice) {
    builder_.on_add_order(add(kAapl, 1, 'S', 100, 10'100));
    builder_.on_order_replace({.header = header('U'),
                               .original_order_ref = 1,
                               .new_order_ref = 2,
                               .shares = 300,
                               .price = 10'200});
    EXPECT_FALSE(book().contains(1));
    EXPECT_TRUE(book().contains(2));
    EXPECT_EQ(book().best_ask(), 10'200);
    EXPECT_EQ(book().volume_at(Side::Sell, 10'200), 300U);
    EXPECT_EQ(builder_.book_messages(), 2U);  // the add and the replace

    // Later messages use the new reference.
    builder_.on_order_executed(
        {.header = header('E'), .order_ref = 2, .executed_shares = 300, .match_number = 1});
    EXPECT_EQ(book().order_count(), 0U);
}

TEST_F(BookBuilderTest, ReferencesToUnknownOrdersAreIgnored) {
    builder_.on_order_executed(
        {.header = header('E'), .order_ref = 42, .executed_shares = 1, .match_number = 1});
    builder_.on_order_cancel({.header = header('X'), .order_ref = 42, .canceled_shares = 1});
    builder_.on_order_delete({.header = header('D'), .order_ref = 42});
    builder_.on_order_replace({.header = header('U'),
                               .original_order_ref = 42,
                               .new_order_ref = 43,
                               .shares = 1,
                               .price = 1});
    EXPECT_EQ(builder_.book_messages(), 0U);
    EXPECT_TRUE(rec_.events().empty());
}

// Regression test. During a halt, a pause or a quotation-only period, Nasdaq accepts
// orders but does not match them, so its book can cross, and it is cleared later by a
// reopening auction. Our books always match, so on the real 2019-12-30 file the replay
// traded 1,577 times in 15 stocks, every time while the stock was not trading. The
// builder now stops replaying a stock from its first halt, pause or quotation-only
// message on, and counts it.
TEST_F(BookBuilderTest, AStockStopsBeingReplayedFromItsFirstHalt) {
    builder_.on_add_order(add(kAapl, 1, 'S', 100, 10'000));
    builder_.on_stock_trading_action(
        {.header = header('H'), .stock = {}, .trading_state = 'P', .reason = {}});
    EXPECT_FALSE(builder_.replaying(kAapl));
    EXPECT_EQ(builder_.suspended_stocks(), 1U);

    // A crossing order during the pause would trade in our book. It must not reach it.
    rec_.clear();
    builder_.on_add_order(add(kAapl, 2, 'B', 100, 10'100));
    builder_.on_order_delete({.header = header('D'), .order_ref = 1});
    EXPECT_TRUE(rec_.events().empty());

    // Trading resumes after Nasdaq's reopening auction, which our book cannot replay, so
    // the stock stays out for the rest of the day.
    builder_.on_stock_trading_action(
        {.header = header('H'), .stock = {}, .trading_state = 'T', .reason = {}});
    builder_.on_add_order(add(kAapl, 3, 'B', 100, 9'000));
    EXPECT_TRUE(rec_.events().empty());
    EXPECT_FALSE(builder_.replaying(kAapl));
    EXPECT_EQ(builder_.suspended_stocks(), 1U);
}

TEST_F(BookBuilderTest, TradingResumptionForATradingStockChangesNothing) {
    builder_.on_stock_trading_action(
        {.header = header('H'), .stock = {}, .trading_state = 'T', .reason = {}});
    builder_.on_add_order(add(kAapl, 1, 'B', 100, 10'000));
    EXPECT_TRUE(builder_.replaying(kAapl));
    EXPECT_EQ(book().order_count(), 1U);
    EXPECT_EQ(builder_.suspended_stocks(), 0U);
}

// When more orders rest at once than the builder can track, the extra ones are dropped
// and counted, and nothing reaches the book for them.
TEST(BookBuilderCapacityTest, OrdersBeyondCapacityAreCountedAsDropped) {
    RecordingListener rec;
    ItchBookBuilder<MapOrderBook> builder(2);
    builder.add_book(kAapl, rec, BookConfig{.max_orders = 8});
    builder.on_add_order(add(kAapl, 1, 'B', 1, 100));
    builder.on_add_order(add(kAapl, 2, 'B', 1, 100));
    builder.on_add_order(add(kAapl, 3, 'B', 1, 100));
    EXPECT_EQ(builder.dropped(), 1U);
    EXPECT_EQ(builder.book(kAapl)->order_count(), 2U);
    builder.on_order_delete({.header = header('D'), .order_ref = 1});
    builder.on_add_order(add(kAapl, 4, 'B', 1, 100));  // there is room again
    EXPECT_EQ(builder.dropped(), 1U);
    EXPECT_EQ(builder.live_orders(), 2U);
}

// The hook wraps every message that reaches a book exactly once, including a replace,
// which makes two book calls.
TEST(BookBuilderHookTest, AroundWrapsEachMessageOnce) {
    RecordingListener rec;
    int calls = 0;
    ItchBookBuilder<SortedOrderBook, CountCalls> builder(16, CountCalls{&calls});
    builder.add_book(kAapl, rec, BookConfig{.max_orders = 16});
    builder.on_add_order(add(kAapl, 1, 'B', 10, 100));
    builder.on_order_replace({.header = header('U'),
                              .original_order_ref = 1,
                              .new_order_ref = 2,
                              .shares = 5,
                              .price = 101});
    builder.on_order_cancel({.header = header('X'), .order_ref = 2, .canceled_shares = 1});
    builder.on_order_delete({.header = header('D'), .order_ref = 2});
    builder.on_add_order(add(kUntracked, 3, 'B', 1, 1));  // no book, so no call
    EXPECT_EQ(calls, 4);
    EXPECT_EQ(builder.book_messages(), 4U);
}

}  // namespace
}  // namespace litbook::itch
