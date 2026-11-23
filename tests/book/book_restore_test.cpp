// Tests for the snapshot calls of the OrderBook concept: for_each_resting lists a book's
// orders in priority order, and restore_resting rebuilds a book from that list without
// any event. The strongest check is that a restored book behaves exactly like the
// original from then on, for every standard workload and every book type.

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/book/recording_listener.hpp"
#include "litbook/book/sorted_order_book.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "book/book_test_support.hpp"
#include "book/test_limits.hpp"
#include "book/workload.hpp"

namespace litbook {
namespace {

using testing::BookTypeNames;
using testing::BookTypes;
using testing::with_test_limits;

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;

// Every resting order of one side, in the order for_each_resting visits them.
template <class Book>
std::vector<RestingOrder> listing(const Book& book, Side side) {
    std::vector<RestingOrder> orders;
    book.for_each_resting(side, [&](const RestingOrder& order) { orders.push_back(order); });
    return orders;
}

// Both sides, bids first. This is the order a snapshot stores them in.
template <class Book>
std::vector<RestingOrder> listing(const Book& book) {
    std::vector<RestingOrder> orders = listing(book, kBuy);
    const std::vector<RestingOrder> asks = listing(book, kSell);
    orders.insert(orders.end(), asks.begin(), asks.end());
    return orders;
}

// Restores every order of `orders` into `book` and its match counter, and expects every
// one to be accepted.
template <class Book>
void restore_all(Book& book, const std::vector<RestingOrder>& orders, MatchId next_match) {
    for (const RestingOrder& order : orders) {
        ASSERT_EQ(book.restore_resting(order), RestoreStatus::Ok) << "order " << order.id;
    }
    book.restore_next_match_id(next_match);
}

// Expects two books to look the same from outside, level by level and order by order.
template <class A, class B>
void expect_same_book(const A& a, const B& b) {
    EXPECT_EQ(a.order_count(), b.order_count());
    EXPECT_EQ(a.best_bid(), b.best_bid());
    EXPECT_EQ(a.best_ask(), b.best_ask());
    for (const Side side : {kBuy, kSell}) {
        EXPECT_EQ(a.level_count(side), b.level_count(side));
        EXPECT_EQ(a.depth(side, a.level_count(side)), b.depth(side, b.level_count(side)));
        EXPECT_EQ(listing(a, side), listing(b, side));
    }
    EXPECT_EQ(a.next_match_id(), b.next_match_id());
    EXPECT_TRUE(a.check_invariants());
    EXPECT_TRUE(b.check_invariants());
}

template <class Book>
class BookRestoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Two bids at 100 (ids 1 then 3), one at 99, and asks at 101 and 102. Order 3 is
        // post-only.
        add(1, kBuy, 100, 10);
        add(2, kBuy, 99, 5);
        book_.submit({.id = 3, .side = kBuy, .post_only = true, .price = 100, .qty = 7});
        add(4, kSell, 102, 4);
        add(5, kSell, 101, 6);
        rec_.clear();
    }

    void add(OrderId id, Side side, Price price, Qty qty, OwnerId owner = 0) {
        book_.submit({.id = id, .owner = owner, .side = side, .price = price, .qty = qty});
    }

    RecordingListener rec_;
    Book book_{rec_, with_test_limits({})};
};

TYPED_TEST_SUITE(BookRestoreTest, BookTypes, BookTypeNames);

TYPED_TEST(BookRestoreTest, ListingVisitsTheBestPriceFirstAndEachQueueInPriorityOrder) {
    const std::vector<RestingOrder> bids{
        {.id = 1, .side = kBuy, .price = 100, .remaining = 10},
        {.id = 3, .side = kBuy, .price = 100, .remaining = 7, .post_only = true},
        {.id = 2, .side = kBuy, .price = 99, .remaining = 5},
    };
    const std::vector<RestingOrder> asks{
        {.id = 5, .side = kSell, .price = 101, .remaining = 6},
        {.id = 4, .side = kSell, .price = 102, .remaining = 4},
    };
    EXPECT_EQ(listing(this->book_, kBuy), bids);
    EXPECT_EQ(listing(this->book_, kSell), asks);
}

TYPED_TEST(BookRestoreTest, ListingShowsWhatIsLeftAfterAPartialFill) {
    this->add(6, kSell, 100, 4);  // Takes 4 of order 1's 10.
    const std::vector<RestingOrder> bids = listing(this->book_, kBuy);
    ASSERT_EQ(bids.size(), 3U);
    EXPECT_EQ(bids[0].id, 1U);
    EXPECT_EQ(bids[0].remaining, 6U);
}

TYPED_TEST(BookRestoreTest, AnEmptyBookListsNothing) {
    RecordingListener rec;
    const TypeParam empty{rec, with_test_limits({})};
    EXPECT_TRUE(listing(empty).empty());
}

TYPED_TEST(BookRestoreTest, RestoringTheListingRebuildsTheSameBookWithoutAnyEvent) {
    RecordingListener rec;
    TypeParam restored{rec, with_test_limits({})};
    restore_all(restored, listing(this->book_), this->book_.next_match_id());
    EXPECT_TRUE(rec.events().empty());
    expect_same_book(this->book_, restored);
}

TYPED_TEST(BookRestoreTest, ARestoredQueueFillsInTheSameOrder) {
    RecordingListener rec;
    TypeParam restored{rec, with_test_limits({})};
    restore_all(restored, listing(this->book_), this->book_.next_match_id());

    // A sell for 20 at 99 sweeps order 1, then 3, then part of 2, in both books.
    const OrderRequest sweep{.id = 9, .side = kSell, .price = 99, .qty = 20};
    this->book_.submit(sweep);
    restored.submit(sweep);
    EXPECT_EQ(this->rec_.take(), rec.take());
    expect_same_book(this->book_, restored);
}

TYPED_TEST(BookRestoreTest, ARestoredPostOnlyOrderStillCannotBeModifiedIntoACross) {
    RecordingListener rec;
    TypeParam restored{rec, with_test_limits({})};
    restore_all(restored, listing(this->book_), this->book_.next_match_id());
    this->book_.modify(3, 101, 7);
    restored.modify(3, 101, 7);
    const std::vector<BookEvent> original = this->rec_.take();
    EXPECT_EQ(original, rec.take());
    ASSERT_EQ(original.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<ModifyRejected>(original[0]));
}

TYPED_TEST(BookRestoreTest, TheNextTradeGetsTheRestoredMatchNumber) {
    RecordingListener rec;
    TypeParam restored{rec, with_test_limits({})};
    restore_all(restored, listing(this->book_), 42);
    EXPECT_EQ(restored.next_match_id(), 42U);
    restored.submit({.id = 9, .side = kSell, .price = 100, .qty = 1});
    bool traded = false;
    for (const BookEvent& event : rec.take()) {
        if (const auto* trade = std::get_if<Trade>(&event)) {
            EXPECT_EQ(trade->match_id, 42U);
            traded = true;
        }
    }
    EXPECT_TRUE(traded);
    EXPECT_EQ(restored.next_match_id(), 43U);
}

TYPED_TEST(BookRestoreTest, EachBrokenRuleIsRefusedAndChangesNothing) {
    BookConfig config = with_test_limits({});
    config.tick_size = 5;
    config.lot_size = 10;
    config.min_price = 50;
    config.max_price = 200;
    config.max_orders = 2;
    RecordingListener rec;
    TypeParam book{rec, config};
    ASSERT_EQ(book.restore_resting({.id = 1, .side = kBuy, .price = 100, .remaining = 10}),
              RestoreStatus::Ok);

    struct Case {
        RestingOrder order;
        RestoreStatus expected = RestoreStatus::Ok;
    };
    const std::vector<Case> cases{
        {{.id = 2, .side = kBuy, .price = 101, .remaining = 10}, RestoreStatus::BadPrice},
        {{.id = 2, .side = kBuy, .price = 45, .remaining = 10}, RestoreStatus::BadPrice},
        {{.id = 2, .side = kSell, .price = 205, .remaining = 10}, RestoreStatus::BadPrice},
        {{.id = 2, .side = kBuy, .price = 95, .remaining = 0}, RestoreStatus::BadQuantity},
        {{.id = 2, .side = kBuy, .price = 95, .remaining = 15}, RestoreStatus::BadQuantity},
        {{.id = 1, .side = kBuy, .price = 95, .remaining = 10}, RestoreStatus::DuplicateId},
        {{.id = 2, .side = kSell, .price = 100, .remaining = 10}, RestoreStatus::WouldCross},
        {{.id = 2, .side = kSell, .price = 95, .remaining = 10}, RestoreStatus::WouldCross},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(book.restore_resting(c.order), c.expected) << "order at " << c.order.price;
        EXPECT_EQ(book.order_count(), 1U);
    }
    // The second order fits, and a third finds the book full.
    ASSERT_EQ(book.restore_resting({.id = 2, .side = kSell, .price = 105, .remaining = 10}),
              RestoreStatus::Ok);
    EXPECT_EQ(book.restore_resting({.id = 3, .side = kSell, .price = 110, .remaining = 10}),
              RestoreStatus::BookFull);
    EXPECT_EQ(book.order_count(), 2U);
    EXPECT_TRUE(rec.events().empty());
    EXPECT_TRUE(book.check_invariants());
}

TYPED_TEST(BookRestoreTest, ARestoredOrderAtANewBestPriceMovesTheBest) {
    RecordingListener rec;
    TypeParam book{rec, with_test_limits({})};
    ASSERT_EQ(book.restore_resting({.id = 1, .side = kBuy, .price = 90, .remaining = 1}),
              RestoreStatus::Ok);
    ASSERT_EQ(book.restore_resting({.id = 2, .side = kBuy, .price = 95, .remaining = 1}),
              RestoreStatus::Ok);
    ASSERT_EQ(book.restore_resting({.id = 3, .side = kSell, .price = 110, .remaining = 1}),
              RestoreStatus::Ok);
    ASSERT_EQ(book.restore_resting({.id = 4, .side = kSell, .price = 105, .remaining = 1}),
              RestoreStatus::Ok);
    EXPECT_EQ(book.best_bid(), 95);
    EXPECT_EQ(book.best_ask(), 105);
    EXPECT_TRUE(book.check_invariants());
}

TEST(RestoreStatusTest, PrintsItsName) {
    std::ostringstream os;
    os << RestoreStatus::WouldCross << ' ' << RestoreStatus::BookFull;
    EXPECT_EQ(os.str(), "WouldCross BookFull");
}

// Runs the first half of a workload, restores the result into a fresh book of type To,
// then runs the second half on both and compares every event and the final books.
template <class From, class To>
void restore_halfway_and_compare(const testing::WorkloadParams& params) {
    const std::vector<testing::Operation> ops = testing::generate(params);
    const BookConfig config = testing::book_config(params);
    RecordingListener original_events;
    From original{original_events, config};
    const std::size_t half = ops.size() / 2;
    for (std::size_t i = 0; i < half; ++i) {
        testing::apply(original, ops[i]);
    }
    original_events.clear();

    RecordingListener restored_events;
    To restored{restored_events, config};
    restore_all(restored, listing(original), original.next_match_id());
    ASSERT_TRUE(restored_events.events().empty());
    expect_same_book(original, restored);

    for (std::size_t i = half; i < ops.size(); ++i) {
        testing::apply(original, ops[i]);
        testing::apply(restored, ops[i]);
    }
    EXPECT_EQ(original_events.events(), restored_events.events());
    expect_same_book(original, restored);
}

class WorkloadRestoreTest : public ::testing::TestWithParam<testing::WorkloadParams> {};

TEST_P(WorkloadRestoreTest, EveryBookBehavesTheSameAfterARestore) {
    restore_halfway_and_compare<MapOrderBook, MapOrderBook>(GetParam());
    restore_halfway_and_compare<FastOrderBook, FastOrderBook>(GetParam());
    restore_halfway_and_compare<SortedOrderBook, SortedOrderBook>(GetParam());
}

TEST_P(WorkloadRestoreTest, AListingFromOneBookTypeRestoresIntoTheOthers) {
    restore_halfway_and_compare<MapOrderBook, FastOrderBook>(GetParam());
    restore_halfway_and_compare<MapOrderBook, SortedOrderBook>(GetParam());
    restore_halfway_and_compare<SortedOrderBook, MapOrderBook>(GetParam());
}

INSTANTIATE_TEST_SUITE_P(Standard, WorkloadRestoreTest,
                         ::testing::ValuesIn(testing::standard_workloads()),
                         [](const auto& test_info) { return std::string(test_info.param.name); });

}  // namespace
}  // namespace litbook
