#include "litbook/pipeline/matching_worker.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/pipeline/publisher.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "book/test_limits.hpp"
#include "pipeline/pipeline_test_support.hpp"

namespace litbook::pipeline {
namespace {

using testing::drain_all;
using testing::holds;
using testing::limit;
using testing::request_of;

constexpr OwnerId kAlice = 1;
constexpr OwnerId kBob = 2;
constexpr Price kTick = 100;

protocol::OrderEntryConfig entry_config() {
    return {.max_owners = 4, .max_live_orders = 1024, .max_symbols = 2};
}

// Names one kind of queue, so a test can be typed over SpscQueue and MutexQueue.
template <template <concurrency::QueueElement> class Q>
struct QueueKind {
    using Requests = Q<Request>;
    using Replies = Q<Reply>;
    using Events = Q<MarketEvent>;
};
using QueueKinds =
    ::testing::Types<QueueKind<concurrency::SpscQueue>, QueueKind<concurrency::MutexQueue>>;

// One worker with its three queues, driven by hand on the test thread.
template <class Kind>
struct Rig {
    using Worker = MatchingWorker<SortedOrderBook, typename Kind::Requests, typename Kind::Replies,
                                  typename Kind::Events>;

    explicit Rig(std::size_t reply_capacity = 1024)
        : requests(1024),
          replies(reply_capacity),
          events(1024),
          worker(entry_config(), requests, replies, events, WaitMode::Spin, Doorbells{}) {
        worker.order_entry().add_symbol(
            testing::aapl(), litbook::testing::with_test_limits(BookConfig{.tick_size = kTick}));
    }

    void push(const Request& r) { ASSERT_TRUE(requests.try_push(r)); }

    typename Kind::Requests requests;
    typename Kind::Replies replies;
    typename Kind::Events events;
    Worker worker;
};

template <class T>
class MatchingWorkerTest : public ::testing::Test {};

TYPED_TEST_SUITE(MatchingWorkerTest, QueueKinds);

TYPED_TEST(MatchingWorkerTest, AnEmptyQueueIsNoWork) {
    Rig<TypeParam> rig;
    EXPECT_FALSE(rig.worker.step());
    EXPECT_EQ(rig.worker.stats().requests, 0U);
    EXPECT_FALSE(rig.worker.stopped());
}

TYPED_TEST(MatchingWorkerTest, AnOrderGetsItsRepliesThenOneRequestDone) {
    Rig<TypeParam> rig;
    rig.push(request_of(kAlice, limit(Side::Buy, 1, 100, 100 * kTick), 77));
    ASSERT_TRUE(rig.worker.step());
    const auto replies = drain_all(rig.replies);
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_EQ(replies[0].owner, kAlice);
    ASSERT_TRUE(holds<protocol::AcceptedMessage>(replies[0]));
    // The timestamp comes from the request, not from a clock on the matching thread.
    EXPECT_EQ(testing::message_of<protocol::AcceptedMessage>(replies[0]).timestamp, 77U);
    EXPECT_EQ(replies[1].kind, ReplyKind::RequestDone);
    EXPECT_EQ(replies[1].owner, kAlice);

    const auto events = drain_all(rig.events);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, MarketEventKind::LevelUpdate);
    EXPECT_EQ(events[0].side, Side::Buy);
    EXPECT_EQ(events[0].price, 100 * kTick);
    EXPECT_EQ(events[0].qty, 100U);
    EXPECT_EQ(events[0].order_count, 1U);
    EXPECT_EQ(rig.worker.stats().frames, 1U);
    EXPECT_EQ(rig.worker.stats().replies, 1U);
}

TYPED_TEST(MatchingWorkerTest, ATradeReachesBothAccountsAndThePublisher) {
    Rig<TypeParam> rig;
    rig.push(request_of(kBob, limit(Side::Sell, 1, 50, 100 * kTick)));
    rig.push(request_of(kAlice, limit(Side::Buy, 1, 30, 101 * kTick)));
    ASSERT_TRUE(rig.worker.step());
    ASSERT_TRUE(rig.worker.step());
    const auto replies = drain_all(rig.replies);
    std::vector<Reply> executions;
    for (const Reply& r : replies) {
        if (holds<protocol::ExecutedMessage>(r)) {
            executions.push_back(r);
        }
    }
    ASSERT_EQ(executions.size(), 2U);
    EXPECT_NE(executions[0].owner, executions[1].owner);
    // Each request ends with its own RequestDone, after all of its other replies.
    EXPECT_EQ(replies.back().kind, ReplyKind::RequestDone);
    EXPECT_EQ(replies.back().owner, kAlice);

    std::size_t trades = 0;
    for (const MarketEvent& e : drain_all(rig.events)) {
        if (e.kind == MarketEventKind::Trade) {
            ++trades;
            EXPECT_EQ(e.price, 100 * kTick);
            EXPECT_EQ(e.qty, 30U);
            EXPECT_EQ(e.side, Side::Buy);
            EXPECT_EQ(e.match_id, 1U);
        }
    }
    EXPECT_EQ(trades, 1U);
}

TYPED_TEST(MatchingWorkerTest, CancelAllCancelsEveryOrderWithoutARequestDone) {
    Rig<TypeParam> rig;
    rig.push(request_of(kAlice, limit(Side::Buy, 1, 10, 90 * kTick)));
    rig.push(request_of(kAlice, limit(Side::Buy, 2, 10, 91 * kTick)));
    rig.push(request_of(kBob, limit(Side::Buy, 1, 10, 92 * kTick)));
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(rig.worker.step());
    }
    (void)drain_all(rig.replies);
    rig.push(make_cancel_all_request(kAlice, 5));
    ASSERT_TRUE(rig.worker.step());
    const auto replies = drain_all(rig.replies);
    ASSERT_EQ(replies.size(), 2U);
    for (const Reply& r : replies) {
        EXPECT_EQ(r.owner, kAlice);
        ASSERT_TRUE(holds<protocol::CanceledMessage>(r));
        EXPECT_EQ(testing::message_of<protocol::CanceledMessage>(r).reason,
                  protocol::WireCancelReason::System);
    }
    EXPECT_EQ(rig.worker.order_entry().open_orders(kAlice), 0U);
    EXPECT_EQ(rig.worker.order_entry().open_orders(kBob), 1U);
    EXPECT_EQ(rig.worker.stats().cancel_alls, 1U);
}

TYPED_TEST(MatchingWorkerTest, AMalformedFrameIsCountedAndStillGetsARequestDone) {
    Rig<TypeParam> rig;
    const std::vector<std::byte> junk{std::byte{'Q'}, std::byte{1}};
    rig.push(make_frame_request(kAlice, junk, 0));
    ASSERT_TRUE(rig.worker.step());
    const auto replies = drain_all(rig.replies);
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].kind, ReplyKind::RequestDone);
    EXPECT_EQ(rig.worker.stats().malformed, 1U);
}

TYPED_TEST(MatchingWorkerTest, StopReachesThePublisherFirstAndThenTheGateway) {
    Rig<TypeParam> rig;
    rig.push(make_stop_request(0));
    ASSERT_TRUE(rig.worker.step());
    EXPECT_TRUE(rig.worker.stopped());
    const auto events = drain_all(rig.events);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, MarketEventKind::Stop);
    const auto replies = drain_all(rig.replies);
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[0].kind, ReplyKind::Stopped);
    rig.worker.run();  // Returns at once, because it has already stopped.
}

// With room for only one reply, the worker has to wait for the gateway on almost every
// message. A second thread plays the gateway and drains slowly. Every reply must still
// arrive, in order, and the worker must have really waited.
TYPED_TEST(MatchingWorkerTest, AFullReplyQueueMakesTheWorkerWaitButNeverDrop) {
    Rig<TypeParam> rig(1);
    constexpr int kOrders = 200;
    constexpr std::size_t kReplies = static_cast<std::size_t>(kOrders) * 2;
    for (int i = 1; i <= kOrders; ++i) {
        rig.push(request_of(kAlice, limit(Side::Buy, static_cast<protocol::UserRefNum>(i), 1,
                                          static_cast<Price>(50 + (i % 20)) * kTick)));
    }
    std::vector<Reply> received;
    std::thread gateway([&] {
        // Start draining only after the queue has been full for a moment, so the worker
        // is sure to have found it full at least once.
        while (rig.replies.size_approx() < rig.replies.capacity()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        while (received.size() < kReplies) {
            Reply r;
            if (rig.replies.try_pop(r)) {
                received.push_back(r);
            } else {
                std::this_thread::yield();
            }
        }
    });
    for (int i = 0; i < kOrders; ++i) {
        ASSERT_TRUE(rig.worker.step());
    }
    gateway.join();
    for (int i = 0; i < kOrders; ++i) {
        const Reply& accepted = received[2U * static_cast<std::size_t>(i)];
        ASSERT_TRUE(holds<protocol::AcceptedMessage>(accepted));
        EXPECT_EQ(testing::message_of<protocol::AcceptedMessage>(accepted).user_ref,
                  static_cast<protocol::UserRefNum>(i + 1));
        EXPECT_EQ(received[2U * static_cast<std::size_t>(i) + 1].kind, ReplyKind::RequestDone);
    }
    EXPECT_GT(rig.worker.stats().reply_waits, 0U);
}

// The same for the event queue, with a slow publisher.
TYPED_TEST(MatchingWorkerTest, AFullEventQueueMakesTheWorkerWaitButNeverDrop) {
    using Events = typename TypeParam::Events;
    Rig<TypeParam> rig;
    // A separate rig would need a different event capacity, so this test builds its own
    // worker around a one-slot event queue.
    Events events(1);
    typename Rig<TypeParam>::Worker worker(entry_config(), rig.requests, rig.replies, events,
                                           WaitMode::Spin, Doorbells{});
    worker.order_entry().add_symbol(
        testing::aapl(), litbook::testing::with_test_limits(BookConfig{.tick_size = kTick}));
    constexpr int kOrders = 100;
    for (int i = 1; i <= kOrders; ++i) {
        rig.push(request_of(kAlice, limit(Side::Buy, static_cast<protocol::UserRefNum>(i), 1,
                                          static_cast<Price>(i) * kTick)));
    }
    rig.push(make_stop_request(0));
    PublisherStub<Events> publisher(events, WaitMode::Spin, nullptr);
    std::thread consumer([&] {
        // As above, the publisher starts late so the worker must wait for it.
        while (events.size_approx() < events.capacity()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        publisher.run();
    });
    std::thread replies([&] {
        // Keep the reply queue from filling up while the worker runs.
        Reply r;
        bool stopped = false;
        while (!stopped) {
            if (rig.replies.try_pop(r)) {
                stopped = r.kind == ReplyKind::Stopped;
            }
        }
    });
    worker.run();
    consumer.join();
    replies.join();
    EXPECT_EQ(publisher.stats().level_updates, static_cast<std::uint64_t>(kOrders));
    EXPECT_TRUE(publisher.stopped());
    EXPECT_GT(worker.stats().event_waits, 0U);
}

// The gateway may be asleep on its doorbell with no timeout while the reply queue is full
// in front of it. The worker must wake it while it waits for room, not only after the
// request is done, or the two threads would wait for each other forever. One request
// makes two replies (Accepted and RequestDone), and the queue holds one.
TYPED_TEST(MatchingWorkerTest, AWorkerWaitingForRoomWakesASleepingGateway) {
    typename TypeParam::Requests requests(256);
    typename TypeParam::Replies replies(1);
    typename TypeParam::Events events(1024);
    Doorbell reply_bell;
    typename Rig<TypeParam>::Worker worker(
        entry_config(), requests, replies, events, WaitMode::Spin,
        Doorbells{.requests = nullptr, .replies = &reply_bell, .events = nullptr});
    worker.order_entry().add_symbol(
        testing::aapl(), litbook::testing::with_test_limits(BookConfig{.tick_size = kTick}));
    constexpr int kOrders = 100;
    for (int i = 1; i <= kOrders; ++i) {
        ASSERT_TRUE(requests.try_push(request_of(
            kAlice, limit(Side::Buy, static_cast<protocol::UserRefNum>(i), 1, 10 * kTick))));
    }
    // This thread plays a blocking gateway. It sleeps with no timeout whenever the
    // queue is empty, so only a ring can wake it.
    std::size_t received = 0;
    std::thread gateway([&] {
        while (received < static_cast<std::size_t>(kOrders) * 2) {
            Reply r;
            if (replies.try_pop(r)) {
                ++received;
            } else if (reply_bell.prepare_to_sleep()) {
                reply_bell.sleep(-1);
            }
        }
    });
    for (int i = 0; i < kOrders; ++i) {
        ASSERT_TRUE(worker.step());
    }
    gateway.join();  // Hangs if the worker waited without ringing.
    EXPECT_GT(worker.stats().reply_waits, 0U);
}

// A blocking worker sleeps on its doorbell and wakes up for each request.
TYPED_TEST(MatchingWorkerTest, ABlockingWorkerSleepsUntilItsDoorbellRings) {
    typename TypeParam::Requests requests(16);
    typename TypeParam::Replies replies(1024);
    typename TypeParam::Events events(1024);
    Doorbell request_bell;
    Doorbell reply_bell;
    typename Rig<TypeParam>::Worker worker(
        entry_config(), requests, replies, events, WaitMode::Block,
        Doorbells{.requests = &request_bell, .replies = &reply_bell, .events = nullptr});
    worker.order_entry().add_symbol(
        testing::aapl(), litbook::testing::with_test_limits(BookConfig{.tick_size = kTick}));
    std::thread matching([&] { worker.run(); });
    for (int i = 1; i <= 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ASSERT_TRUE(requests.try_push(request_of(
            kAlice, limit(Side::Buy, static_cast<protocol::UserRefNum>(i), 1, 10 * kTick))));
        request_bell.ring();
    }
    ASSERT_TRUE(requests.try_push(make_stop_request(0)));
    request_bell.ring();
    matching.join();  // Hangs if a ring was lost.
    EXPECT_EQ(worker.stats().frames, 20U);
    EXPECT_GT(request_bell.wakeups_sent(), 0U);
    EXPECT_EQ(drain_all(replies).size(), 41U);  // 20 Accepted, 20 RequestDone, Stopped.
}

TEST(PublisherStubTest, RestoreOnlyCountsLevelUpdates) {
    concurrency::SpscQueue<MarketEvent> events(16);
    PublisherStub<concurrency::SpscQueue<MarketEvent>> publisher(events, WaitMode::Spin, nullptr);
    MarketEvent level;
    level.kind = MarketEventKind::LevelUpdate;
    MarketEvent trade;
    trade.kind = MarketEventKind::Trade;
    publisher.restore(level);
    publisher.restore(trade);
    publisher.restore(level);
    publisher.begin_new_session(7);
    EXPECT_EQ(publisher.restored(), 2U);
    EXPECT_EQ(publisher.stats().level_updates, 0U);
}

TEST(PublisherStubTest, CountsTradesAndLevelUpdatesUntilStop) {
    concurrency::SpscQueue<MarketEvent> events(16);
    PublisherStub<concurrency::SpscQueue<MarketEvent>> publisher(events, WaitMode::Spin, nullptr);
    EXPECT_FALSE(publisher.step());
    MarketEvent trade;
    trade.kind = MarketEventKind::Trade;
    trade.qty = 30;
    MarketEvent level;
    level.kind = MarketEventKind::LevelUpdate;
    level.qty = 500;
    MarketEvent stop;
    stop.kind = MarketEventKind::Stop;
    ASSERT_TRUE(events.try_push(trade));
    ASSERT_TRUE(events.try_push(level));
    ASSERT_TRUE(events.try_push(trade));
    ASSERT_TRUE(events.try_push(stop));
    ASSERT_TRUE(events.try_push(trade));  // After Stop, so never read.
    publisher.run();
    EXPECT_TRUE(publisher.stopped());
    EXPECT_EQ(publisher.stats().trades, 2U);
    EXPECT_EQ(publisher.stats().level_updates, 1U);
    EXPECT_EQ(publisher.stats().traded_qty, 60U);
    EXPECT_FALSE(publisher.step());
    EXPECT_EQ(events.size_approx(), 1U);
}

TEST(PublisherStubTest, TheChecksumDependsOnTheOrderOfEvents) {
    MarketEvent a;
    a.kind = MarketEventKind::Trade;
    a.price = 10;
    MarketEvent b;
    b.kind = MarketEventKind::LevelUpdate;
    b.price = 20;
    const auto checksum = [](const std::vector<MarketEvent>& stream) {
        concurrency::SpscQueue<MarketEvent> events(16);
        PublisherStub<concurrency::SpscQueue<MarketEvent>> publisher(events, WaitMode::Spin,
                                                                     nullptr);
        for (const MarketEvent& e : stream) {
            EXPECT_TRUE(events.try_push(e));
        }
        while (publisher.step()) {
        }
        return publisher.stats().checksum;
    };
    EXPECT_EQ(checksum({a, b}), checksum({a, b}));
    EXPECT_NE(checksum({a, b}), checksum({b, a}));
    EXPECT_NE(checksum({a}), checksum({a, a}));
}

}  // namespace
}  // namespace litbook::pipeline
