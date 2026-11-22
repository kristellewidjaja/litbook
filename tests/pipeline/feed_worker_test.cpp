#include "litbook/pipeline/feed_worker.hpp"

#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/pipeline/doorbell.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "feed/feed_test_support.hpp"

// FeedWorker driven by hand with a fake clock, plus two runs on a real thread. The
// FeedPublisher itself is tested in tests/feed. These tests are about when the worker
// flushes, ticks, sleeps and stops.

namespace litbook::pipeline {
namespace {

using Events = concurrency::SpscQueue<MarketEvent>;
using Worker = FeedWorker<Events>;
using feed::FeedStream;
using feed::testing::decode_packet;
using feed::testing::level_event;
using feed::testing::RecordingSink;
using feed::testing::ticker;

constexpr Timestamp kMillisecond = 1'000'000;
constexpr Timestamp kSecond = 1'000 * kMillisecond;

// The fake clock. Only the test's own thread moves it, but the worker thread of the
// threaded tests reads it, so it is atomic.
std::atomic<Timestamp> g_now{0};
Timestamp fake_clock() {
    return g_now.load(std::memory_order_relaxed);
}

FeedWorkerOptions options_for(feed::FeedSink& sink) {
    FeedWorkerOptions o;
    o.feed.session = "WORKER";
    o.feed.snapshot_spacing = 10 * kMillisecond;
    o.feed.heartbeat_interval = kSecond;
    o.sink = &sink;
    o.max_levels_per_side = 64;
    o.max_events_per_batch = 4;
    return o;
}

const BookConfig kBook{.tick_size = 100, .lot_size = 1};

MarketEvent stop_event() {
    MarketEvent e;
    e.kind = MarketEventKind::Stop;
    return e;
}

class FeedWorkerTest : public ::testing::Test {
protected:
    FeedWorkerTest()
        : events_(64), worker_(events_, WaitMode::Spin, nullptr, &fake_clock, options_for(sink_)) {
        g_now = 1'000;
        worker_.add_symbol(0, ticker("AAPL"), kBook);
        worker_.add_symbol(1, ticker("MSFT"), kBook);
    }

    void push(const MarketEvent& e) { ASSERT_TRUE(events_.try_push(e)); }

    RecordingSink sink_;
    Events events_;
    Worker worker_;
};

TEST_F(FeedWorkerTest, StartSendsTheDirectory) {
    worker_.start();
    ASSERT_EQ(sink_.packets.size(), 1U);
    EXPECT_EQ(decode_packet(sink_.packets[0].bytes).messages.size(), 3U);
    EXPECT_TRUE(worker_.feed().started());
}

TEST_F(FeedWorkerTest, EventsWaitUntilTheQueueRunsEmpty) {
    worker_.start();
    worker_.service(false, 1'000);  // The first snapshot is due at the start.
    sink_.packets.clear();
    push(level_event(0, Side::Buy, 10'000, 5, 1, 1'000));
    push(level_event(1, Side::Sell, 20'000, 7, 2, 1'000));
    EXPECT_TRUE(worker_.step());
    EXPECT_TRUE(worker_.step());
    EXPECT_FALSE(worker_.step());  // Empty now.
    EXPECT_TRUE(sink_.packets.empty());
    EXPECT_EQ(worker_.stats().events, 2U);

    // A batch that did not empty the queue sends nothing, because nothing is due yet.
    worker_.service(false, 2'000);
    EXPECT_TRUE(sink_.packets.empty());
    // An empty queue sends both events in one packet at once.
    worker_.service(true, 2'000);
    ASSERT_EQ(sink_.packets.size(), 1U);
    EXPECT_EQ(sink_.packets[0].stream, FeedStream::Incremental);
    EXPECT_EQ(decode_packet(sink_.packets[0].bytes).messages.size(), 2U);
    EXPECT_EQ(worker_.stats().flushes, 1U);
}

TEST_F(FeedWorkerTest, SnapshotsAndHeartbeatsGoOutOnlyWhenDue) {
    worker_.start();
    worker_.service(false, 1'000);
    EXPECT_EQ(worker_.feed().stats().snapshots, 1U);
    EXPECT_EQ(worker_.stats().ticks, 1U);
    worker_.service(false, 1'000 + 10 * kMillisecond - 1);
    EXPECT_EQ(worker_.stats().ticks, 1U);  // One nanosecond early.
    worker_.service(false, 1'000 + 10 * kMillisecond);
    EXPECT_EQ(worker_.feed().stats().snapshots, 2U);
    EXPECT_EQ(worker_.stats().ticks, 2U);
}

TEST_F(FeedWorkerTest, StopEndsTheFeedsSession) {
    worker_.start();
    push(level_event(0, Side::Buy, 10'000, 5, 1));
    push(stop_event());
    push(level_event(0, Side::Buy, 10'100, 5, 1));  // After Stop, never popped.
    EXPECT_TRUE(worker_.step());
    EXPECT_TRUE(worker_.step());
    EXPECT_TRUE(worker_.stopped());
    EXPECT_TRUE(worker_.feed().ended());
    EXPECT_FALSE(worker_.step());
    EXPECT_EQ(worker_.stats().events, 1U);
    EXPECT_EQ(events_.size_approx(), 1U);
    // The level update went out with End of Messages, before the end of session packets.
    const auto incremental = sink_.of(FeedStream::Incremental);
    ASSERT_GE(incremental.size(), 2U);
    EXPECT_TRUE(decode_packet(incremental.back()).header.end_of_session());
}

TEST_F(FeedWorkerTest, TheSleepTimeoutRoundsUpAndHasALimit) {
    worker_.start();
    EXPECT_EQ(worker_.sleep_timeout_ms(1'000), 0);  // The first snapshot is due now.
    worker_.service(false, 1'000);
    // The next snapshot is due at 1'000 + 10 ms.
    EXPECT_EQ(worker_.sleep_timeout_ms(1'000), 10);
    EXPECT_EQ(worker_.sleep_timeout_ms(1'001), 10);  // 9.999999 ms rounds up.
    EXPECT_EQ(worker_.sleep_timeout_ms(1'000 + 9 * kMillisecond), 1);
    EXPECT_EQ(worker_.sleep_timeout_ms(1'000 + 10 * kMillisecond), 0);
    EXPECT_EQ(worker_.sleep_timeout_ms(1'000 + 20 * kMillisecond), 0);  // Overdue.

    RecordingSink sink;
    Events events(4);
    FeedWorkerOptions slow = options_for(sink);
    slow.feed.snapshot_spacing = 100 * kSecond;
    slow.feed.heartbeat_interval = 100 * kSecond;
    slow.longest_sleep_ms = 250;
    Worker worker(events, WaitMode::Block, nullptr, &fake_clock, slow);
    worker.add_symbol(0, ticker("AAPL"), kBook);
    worker.start();
    worker.service(false, fake_clock());
    EXPECT_EQ(worker.sleep_timeout_ms(fake_clock()), 250);
}

TEST_F(FeedWorkerTest, LocatesMustMatchTheEnginesSymbolIds) {
    EXPECT_THROW(worker_.add_symbol(3, ticker("IBM"), kBook), std::logic_error);
    EXPECT_THROW(worker_.add_symbol(1, ticker("IBM"), kBook), std::logic_error);
    EXPECT_EQ(worker_.feed().symbol_count(), 2U);
    worker_.add_symbol(2, ticker("IBM"), kBook);
    EXPECT_EQ(worker_.feed().symbol_count(), 3U);
}

TEST(FeedWorkerSetupTest, BadOptionsThrow) {
    RecordingSink sink;
    Events events(4);
    FeedWorkerOptions o = options_for(sink);
    o.sink = nullptr;
    EXPECT_THROW(Worker(events, WaitMode::Spin, nullptr, &fake_clock, o), std::invalid_argument);
    o = options_for(sink);
    o.max_events_per_batch = 0;
    EXPECT_THROW(Worker(events, WaitMode::Spin, nullptr, &fake_clock, o), std::invalid_argument);
    o = options_for(sink);
    o.longest_sleep_ms = 0;
    EXPECT_THROW(Worker(events, WaitMode::Spin, nullptr, &fake_clock, o), std::invalid_argument);
    o = options_for(sink);
    o.feed.session = "no spaces";
    EXPECT_THROW(Worker(events, WaitMode::Spin, nullptr, &fake_clock, o), std::invalid_argument);
}

// A sink the worker thread writes while the test thread reads how many packets arrived.
class LockedSink final : public feed::FeedSink {
public:
    void send(FeedStream stream, std::span<const std::byte> packet) override {
        const std::scoped_lock lock(mutex_);
        inner_.send(stream, packet);
    }
    [[nodiscard]] std::size_t messages_on(FeedStream stream) const {
        const std::scoped_lock lock(mutex_);
        std::size_t n = 0;
        for (const auto& bytes : inner_.of(stream)) {
            n += decode_packet(bytes).messages.size();
        }
        return n;
    }

private:
    mutable std::mutex mutex_;
    RecordingSink inner_;
};

// Waits up to ten seconds for `done`. The limit only matters when something is broken.
template <class Done>
bool wait_until(Done done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

// The worker on its own thread, in both wait modes. The clock stands still and nothing
// is due for an hour, and the blocking worker may sleep that long. So each event reaches
// the sink only because a ring woke the worker, and a lost wakeup makes this test hang.
void run_on_a_thread(WaitMode mode) {
    LockedSink sink;
    Events events(16);
    Doorbell bell;
    FeedWorkerOptions o = options_for(sink);
    o.feed.snapshot_spacing = 3'600 * kSecond;
    o.feed.heartbeat_interval = 3'600 * kSecond;
    o.longest_sleep_ms = 3'600'000;
    g_now = 5'000;
    Worker worker(events, mode, mode == WaitMode::Block ? &bell : nullptr, &fake_clock, o);
    worker.add_symbol(0, ticker("AAPL"), kBook);
    std::thread thread([&] { worker.run(); });

    // Start of Messages and the directory, then one snapshot of three messages.
    ASSERT_TRUE(wait_until([&] { return sink.messages_on(FeedStream::Snapshot) == 3; }));
    for (unsigned i = 1; i <= 50; ++i) {
        while (!events.try_push(
            level_event(0, Side::Buy, 10'000 + (static_cast<Price>(i) * 100), i, 1))) {
            std::this_thread::yield();
        }
        bell.ring();
        // Each event is flushed on its own because the queue runs empty after it.
        ASSERT_TRUE(wait_until([&] { return sink.messages_on(FeedStream::Incremental) == 2 + i; }));
    }
    while (!events.try_push(stop_event())) {
        std::this_thread::yield();
    }
    bell.ring();
    thread.join();
    EXPECT_TRUE(worker.stopped());
    EXPECT_EQ(worker.stats().events, 50U);
    EXPECT_EQ(worker.feed().stats().snapshots, 1U);
    EXPECT_EQ(worker.feed().stats().heartbeats[0], 0U);
    if (mode == WaitMode::Block) {
        EXPECT_GE(worker.stats().sleeps, 50U);
    } else {
        EXPECT_EQ(worker.stats().sleeps, 0U);
    }
    // The 50 levels plus End of Messages.
    EXPECT_EQ(sink.messages_on(FeedStream::Incremental), 2U + 50U + 1U);
}

// With no events at all, a sleeping worker must still wake for each snapshot. Nothing
// rings its doorbell here, so only the sleep timeout can wake it.
TEST(FeedWorkerThreadTest, ASleepingWorkerWakesForItsDeadlines) {
    LockedSink sink;
    Events events(16);
    Doorbell bell;
    const FeedWorkerOptions o = options_for(sink);  // A snapshot every 10 ms.
    g_now = 0;
    Worker worker(events, WaitMode::Block, &bell, &fake_clock, o);
    worker.add_symbol(0, ticker("AAPL"), kBook);
    std::thread thread([&] { worker.run(); });
    // Each snapshot of a stock with no levels is three messages. The test moves the clock
    // forward and waits for the next snapshot each time.
    for (std::size_t n = 1; n <= 5; ++n) {
        ASSERT_TRUE(wait_until([&] { return sink.messages_on(FeedStream::Snapshot) == 3 * n; }));
        g_now += 10 * kMillisecond;
    }
    while (!events.try_push(stop_event())) {
        std::this_thread::yield();
    }
    bell.ring();
    thread.join();
    EXPECT_GE(worker.stats().sleeps, 5U);
}

TEST(FeedWorkerThreadTest, ASpinningWorkerPublishesEveryEventUntilStop) {
    run_on_a_thread(WaitMode::Spin);
}

TEST(FeedWorkerThreadTest, ASleepingWorkerIsWokenForEveryEvent) {
    run_on_a_thread(WaitMode::Block);
}

// Under a steady stream of events the queue never runs empty, but snapshots must still go
// out when due. The batch limit in run_once is what lets the worker look at the clock.
TEST(FeedWorkerRoundTest, AQueueThatNeverEmptiesDoesNotStarveSnapshots) {
    RecordingSink sink;
    Events events(64);
    const FeedWorkerOptions o = options_for(sink);  // A batch is 4 events.
    g_now = 0;
    Worker worker(events, WaitMode::Spin, nullptr, &fake_clock, o);
    worker.add_symbol(0, ticker("AAPL"), kBook);
    worker.start();
    std::uint64_t pushed = 0;
    for (int round = 0; round < 200; ++round) {
        // Refill first, so the round finds more events than one batch.
        while (events.try_push(
            level_event(0, Side::Buy, static_cast<Price>(10'000 + (pushed % 40) * 100), 1, 1))) {
            ++pushed;
        }
        g_now += kMillisecond;
        EXPECT_FALSE(worker.run_once());
        EXPECT_EQ(worker.stats().events, 4U * static_cast<std::uint64_t>(round + 1));
    }
    EXPECT_EQ(worker.stats().flushes, 0U);
    // 200 ms passed with a snapshot due every 10 ms, the first one at the start.
    EXPECT_EQ(worker.feed().stats().snapshots, 20U);
}

TEST(FeedWorkerRoundTest, ARoundThatEmptiesTheQueueFlushesAndSaysSo) {
    RecordingSink sink;
    Events events(64);
    g_now = 0;
    Worker worker(events, WaitMode::Spin, nullptr, &fake_clock, options_for(sink));
    worker.add_symbol(0, ticker("AAPL"), kBook);
    worker.start();
    EXPECT_TRUE(worker.run_once());  // Nothing to do, but the first snapshot is due.
    EXPECT_EQ(worker.feed().stats().snapshots, 1U);
    sink.packets.clear();
    ASSERT_TRUE(events.try_push(level_event(0, Side::Buy, 10'000, 1, 1)));
    ASSERT_TRUE(events.try_push(level_event(0, Side::Buy, 10'100, 1, 1)));
    EXPECT_TRUE(worker.run_once());
    ASSERT_EQ(sink.packets.size(), 1U);
    EXPECT_EQ(decode_packet(sink.packets[0].bytes).messages.size(), 2U);
    ASSERT_TRUE(events.try_push(stop_event()));
    EXPECT_FALSE(worker.run_once());  // Stopped, so there is nothing to wait for.
    EXPECT_TRUE(worker.stopped());
}

}  // namespace
}  // namespace litbook::pipeline
