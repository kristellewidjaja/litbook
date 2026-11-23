// Tests for OrderEntry::capture() and OrderEntry::restore(). A layer rebuilt from a
// captured state must answer every later request exactly like the layer it was captured
// from, and restore() must refuse any state that breaks an invariant of OrderEntryState.

#include "litbook/protocol/order_entry.hpp"
#include "litbook/protocol/order_entry_state.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "book/book_test_support.hpp"
#include "journal/replay_test_support.hpp"

namespace litbook::protocol {
namespace {

using journal::testing::apply_request;
using journal::testing::expect_same_state;
using journal::testing::RecordingEvents;
using journal::testing::RecordingReplies;
using journal::testing::replay_order_entry_config;
using journal::testing::replay_stream;
using journal::testing::set_up_exchange;

constexpr std::uint64_t kSeed = 20261122;
constexpr int kRequests = 3000;

// An order entry layer with its own sinks, listed like the replay tests expect.
template <class Book>
struct Exchange {
    Exchange() : events(&now) {
        orders.set_market_data_sink(&events);
        set_up_exchange(orders);
    }
    void apply(const pipeline::Request& request) {
        now = request.now;
        apply_request(orders, request);
    }
    [[nodiscard]] OrderEntryState capture() const {
        OrderEntryState state;
        orders.capture(state);
        return state;
    }

    pipeline::Timestamp now = 0;
    RecordingReplies replies;
    RecordingEvents events;
    OrderEntry<Book> orders{replies, replay_order_entry_config()};
};

template <class Book>
class OrderEntryRestoreTest : public ::testing::Test {
protected:
    // Runs the first `count` requests of the stream on the original layer.
    void run_original(std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            original_.apply(stream_[i]);
        }
    }

    // Captures the original, mutates the state, and expects restore() to refuse it with
    // std::invalid_argument.
    void expect_refused(const std::function<void(OrderEntryState&)>& damage) {
        OrderEntryState state = original_.capture();
        damage(state);
        Exchange<Book> fresh;
        EXPECT_THROW(fresh.orders.restore(state), std::invalid_argument);
    }

    std::vector<pipeline::Request> stream_ = replay_stream(kSeed, kRequests);
    Exchange<Book> original_;
};

TYPED_TEST_SUITE(OrderEntryRestoreTest, litbook::testing::BookTypes,
                 litbook::testing::BookTypeNames);

TYPED_TEST(OrderEntryRestoreTest, AFreshLayerCapturesAnEmptyState) {
    const OrderEntryState state = this->original_.capture();
    EXPECT_EQ(state.last_order_ref, 0U);
    EXPECT_EQ(state.last_match_id, 0U);
    EXPECT_EQ(state.last_user_ref,
              std::vector<std::uint32_t>(replay_order_entry_config().max_owners + 1, 0));
    EXPECT_TRUE(state.orders.empty());
    EXPECT_TRUE(state.resting.empty());
    ASSERT_EQ(state.books.size(), journal::testing::kReplayStocks.size());
    for (const BookState& book : state.books) {
        EXPECT_EQ(book, (BookState{.next_match_id = 1, .order_count = 0}));
    }
}

TYPED_TEST(OrderEntryRestoreTest, TheCapturedStateHoldsTheLiveOrders) {
    this->run_original(this->stream_.size());
    const OrderEntryState state = this->original_.capture();
    const OrderEntry<TypeParam>& orders = this->original_.orders;
    EXPECT_EQ(state.orders.size(), orders.live_orders());
    EXPECT_GT(state.orders.size(), 20U);  // The stream leaves a real book behind.
    EXPECT_EQ(state.last_match_id, orders.engine().last_match_id());
    EXPECT_GT(state.last_match_id, 0U);
    EXPECT_EQ(state.stats, orders.stats());
    for (OwnerId owner = 1; owner <= orders.config().max_owners; ++owner) {
        EXPECT_EQ(state.last_user_ref[owner], orders.last_user_ref(owner));
    }
}

TYPED_TEST(OrderEntryRestoreTest, RestoringAndCapturingAgainGivesTheSameState) {
    this->run_original(this->stream_.size());
    const OrderEntryState state = this->original_.capture();
    Exchange<TypeParam> restored;
    restored.orders.restore(state);
    EXPECT_EQ(restored.capture(), state);
    EXPECT_TRUE(restored.replies.replies.empty());
    EXPECT_TRUE(restored.events.events.empty());
    expect_same_state(this->original_.orders, restored.orders);
}

TYPED_TEST(OrderEntryRestoreTest, ARestoredLayerAnswersTheRestOfTheStreamTheSame) {
    const std::size_t half = this->stream_.size() / 2;
    this->run_original(half);
    Exchange<TypeParam> restored;
    restored.orders.restore(this->original_.capture());
    this->original_.replies.replies.clear();
    this->original_.events.events.clear();

    for (std::size_t i = half; i < this->stream_.size(); ++i) {
        this->original_.apply(this->stream_[i]);
        restored.apply(this->stream_[i]);
    }
    EXPECT_EQ(this->original_.replies.replies, restored.replies.replies);
    EXPECT_EQ(this->original_.events.events, restored.events.events);
    expect_same_state(this->original_.orders, restored.orders);
    EXPECT_EQ(this->original_.capture(), restored.capture());
}

TYPED_TEST(OrderEntryRestoreTest, CancelAllSendsTheCancelsInTheSameOrderAfterARestore) {
    this->run_original(this->stream_.size());
    Exchange<TypeParam> restored;
    restored.orders.restore(this->original_.capture());
    this->original_.replies.replies.clear();
    for (OwnerId owner = 1; owner <= replay_order_entry_config().max_owners; ++owner) {
        this->original_.orders.cancel_all(owner, 99'999'999);
        restored.orders.cancel_all(owner, 99'999'999);
    }
    EXPECT_GT(this->original_.replies.replies.size(), 20U);
    EXPECT_EQ(this->original_.replies.replies, restored.replies.replies);
}

TYPED_TEST(OrderEntryRestoreTest, ALayerThatHandledAMessageCannotBeRestored) {
    this->run_original(10);
    Exchange<TypeParam> other;
    other.apply(this->stream_[0]);
    EXPECT_THROW(other.orders.restore(this->original_.capture()), std::logic_error);
}

TYPED_TEST(OrderEntryRestoreTest, AStateWithAnotherShapeIsRefused) {
    this->run_original(this->stream_.size());
    this->expect_refused([](OrderEntryState& s) { s.last_user_ref.push_back(0); });
    this->expect_refused([](OrderEntryState& s) { s.last_user_ref[0] = 1; });
    this->expect_refused([](OrderEntryState& s) { s.books.pop_back(); });
    this->expect_refused([](OrderEntryState& s) { s.orders.pop_back(); });
    this->expect_refused([](OrderEntryState& s) { ++s.books[0].order_count; });
}

TYPED_TEST(OrderEntryRestoreTest, ALiveOrderThatBreaksARuleIsRefused) {
    this->run_original(this->stream_.size());
    ASSERT_GE(this->original_.capture().orders.size(), 2U);
    this->expect_refused([](OrderEntryState& s) { s.orders[0].owner = kNoOwner; });
    this->expect_refused([](OrderEntryState& s) { s.orders[0].owner = 99; });
    this->expect_refused([](OrderEntryState& s) { s.orders[0].symbol = 7; });
    this->expect_refused([](OrderEntryState& s) { s.orders[0].ref = s.last_order_ref + 1; });
    this->expect_refused(
        [](OrderEntryState& s) { s.orders[0].user_ref = s.last_user_ref[s.orders[0].owner] + 1; });
    this->expect_refused([](OrderEntryState& s) { s.orders[0].open = 0; });
    this->expect_refused([](OrderEntryState& s) { s.orders[1].ref = s.orders[0].ref; });
    this->expect_refused([](OrderEntryState& s) { ++s.orders[0].open; });
    this->expect_refused([](OrderEntryState& s) { ++s.orders[0].price; });
}

TYPED_TEST(OrderEntryRestoreTest, ABookThatRefusesAnOrderRefusesTheState) {
    this->run_original(this->stream_.size());
    this->expect_refused([](OrderEntryState& s) { s.resting[0].remaining = 0; });
    this->expect_refused([](OrderEntryState& s) { s.resting[1].id = s.resting[0].id; });
}

TYPED_TEST(OrderEntryRestoreTest, AStateOutOfCanonicalOrderIsRefused) {
    this->run_original(this->stream_.size());
    const OrderEntryState state = this->original_.capture();
    // Find two neighbors that belong to different accounts and swap them, so every
    // record is still valid but the accounts are out of order.
    std::size_t at = 0;
    while (at + 1 < state.orders.size() && state.orders[at].owner == state.orders[at + 1].owner) {
        ++at;
    }
    ASSERT_LT(at + 1, state.orders.size());
    this->expect_refused([at](OrderEntryState& s) { std::swap(s.orders[at], s.orders[at + 1]); });
}

TYPED_TEST(OrderEntryRestoreTest, TheOrderOfAnAccountsListIsKeptAsGiven) {
    // Any order of one account's records is a list some history could have built, so it
    // is not refused. It is kept exactly, which is why cancel_all() answers the same.
    this->run_original(this->stream_.size());
    OrderEntryState state = this->original_.capture();
    std::size_t same = 0;
    while (same + 1 < state.orders.size() &&
           state.orders[same].owner != state.orders[same + 1].owner) {
        ++same;
    }
    ASSERT_LT(same + 1, state.orders.size());
    std::swap(state.orders[same], state.orders[same + 1]);
    Exchange<TypeParam> restored;
    restored.orders.restore(state);
    EXPECT_EQ(restored.capture(), state);
}

}  // namespace
}  // namespace litbook::protocol
