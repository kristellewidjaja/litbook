#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/concurrency/cpu_relax.hpp"
#include "litbook/concurrency/message_queue.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/feed/feed_publisher.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/messages.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace litbook::pipeline {

struct FeedWorkerOptions {
    feed::FeedPublisherConfig feed;
    // Where the packets go, usually a feed::UdpFeedSink. It must not be nullptr, and the
    // caller keeps it alive until the worker is destroyed.
    feed::FeedSink* sink = nullptr;
    // The room each stock's level copy reserves on each side.
    std::size_t max_levels_per_side = 1024;
    // The most events handled before the worker checks the clock for a due snapshot or
    // heartbeat. Without a limit a steady stream of events would starve the snapshots.
    std::size_t max_events_per_batch = 64;
    // The longest a blocking worker sleeps, even when nothing is due for longer. One
    // second, the default heartbeat interval. Tests raise it, so that a lost wakeup makes
    // the test hang instead of only costing a second.
    int longest_sleep_ms = 1'000;
};

struct FeedWorkerStats {
    std::uint64_t events = 0;   // Trades and level updates handed to the publisher.
    std::uint64_t flushes = 0;  // Times the queue ran empty and the open packet was sent.
    std::uint64_t ticks = 0;    // Times a snapshot or heartbeat was due.
    std::uint64_t sleeps = 0;   // Times the thread slept on its doorbell (Block mode).
};

// The publisher thread with the real market data feed. It pops the matching thread's
// market events, hands them to a FeedPublisher, and sends what is waiting whenever the
// event queue runs empty. So an order that changes several levels usually leaves in one
// packet, and no event waits for a timer. Between events it sends the snapshot stream and
// heartbeats when they are due. In Block mode it sleeps on its doorbell, but never past
// the next snapshot or heartbeat.
//
// It has the same step, run and stopped interface as PublisherStub, so ThreadedGateway
// can run either one.
//
// Invariants. Exactly one thread calls start, step, service, run_once or run, and it is the only
// consumer of the event queue. Each stock's locate on the feed equals its SymbolId in the
// engine, which add_symbol checks. After Stop the feed has sent End of Session and
// nothing more is popped.
template <concurrency::MessageQueue EventQueue>
class FeedWorker {
    // A static_assert and not a requires clause, so the template has exactly the shape
    // BasicThreadedGateway's Publisher parameter asks for.
    static_assert(std::same_as<typename EventQueue::value_type, MarketEvent>,
                  "the publisher consumes MarketEvents");

public:
    using Options = FeedWorkerOptions;
    using Clock = Timestamp (*)();

    // `bell` wakes this thread in WaitMode::Block, and is nullptr when it spins. Throws
    // std::invalid_argument if options.sink is nullptr, the batch size or longest sleep is
    // 0 or less, or the feed configuration is bad.
    FeedWorker(EventQueue& events, WaitMode mode, Doorbell* bell, Clock clock,
               const Options& options)
        : events_(&events),
          mode_(mode),
          bell_(bell),
          clock_(clock),
          publisher_(checked_sink(options), options.feed),
          max_levels_per_side_(options.max_levels_per_side),
          max_events_per_batch_(options.max_events_per_batch),
          longest_sleep_ms_(options.longest_sleep_ms) {
        if (max_events_per_batch_ == 0) {
            throw std::invalid_argument("FeedWorker max_events_per_batch must be above 0");
        }
        if (longest_sleep_ms_ <= 0) {
            throw std::invalid_argument("FeedWorker longest_sleep_ms must be above 0");
        }
    }

    // Lists a stock on the feed, before start. ThreadedGateway calls it right after the
    // engine listed the stock as `id`. Throws std::logic_error if the feed would give the
    // stock a different locate, plus whatever FeedPublisher::add_symbol throws.
    void add_symbol(SymbolId id, Ticker ticker, const BookConfig& config) {
        if (id != publisher_.symbol_count()) {
            throw std::logic_error("feed locates must match the engine's symbol ids");
        }
        publisher_.add_symbol(feed::FeedSymbol{.symbol = ticker,
                                               .tick_size = config.tick_size,
                                               .lot_size = config.lot_size,
                                               .max_levels_per_side = max_levels_per_side_});
    }

    // Recovery calls, before start. restore() gives the feed's level copy one level update
    // that recovery produced while it rebuilt the books from the journal, so nothing is
    // published for it. begin_new_session() names the feed's session after the journal's
    // Restart record, so every start gets a session name of its own and receivers fill
    // their books again from the snapshot stream. Both throw std::logic_error after start.
    void restore(const MarketEvent& event) { publisher_.restore(event); }
    void begin_new_session(std::uint64_t restart_sequence) {
        publisher_.rename_session(session_name_for(restart_sequence));
    }

    // A session name is at most 10 digits, so the sequence number is cut to its last 10.
    [[nodiscard]] static std::string session_name_for(std::uint64_t restart_sequence) {
        constexpr std::uint64_t kTenDigits = 10'000'000'000ULL;
        return std::to_string(restart_sequence % kTenDigits);
    }

    // Sends Start of Messages and the directory. run() calls it first.
    void start() { publisher_.start(clock_()); }

    // Pops and handles one event. Returns false if the queue was empty or the worker has
    // already stopped. Stop ends the feed's session.
    bool step() {
        if (stopped_) {
            return false;
        }
        MarketEvent e;
        if (!events_->try_pop(e)) {
            return false;
        }
        if (e.kind == MarketEventKind::Stop) {
            publisher_.end_session(clock_());
            stopped_ = true;
            return true;
        }
        publisher_.on_event(e);
        ++stats_.events;
        return true;
    }

    // What the worker does between batches of events. If the queue ran empty, the open
    // packet is sent at once. If a snapshot or heartbeat is due, tick sends it.
    void service(bool queue_ran_empty, Timestamp now) {
        if (queue_ran_empty) {
            publisher_.flush(now);
            ++stats_.flushes;
        }
        if (now >= publisher_.next_deadline()) {
            publisher_.tick(now);
            ++stats_.ticks;
        }
    }

    // One round of run(). Handles up to max_events_per_batch events, then calls service.
    // Returns true if the queue ran empty, which means the caller may wait for more.
    bool run_once() {
        std::size_t handled = 0;
        bool ran_empty = false;
        while (handled < max_events_per_batch_) {
            if (!step()) {
                ran_empty = true;
                break;
            }
            ++handled;
        }
        if (stopped_) {
            return false;
        }
        last_service_time_ = clock_();
        service(ran_empty, last_service_time_);
        return ran_empty;
    }

    // Starts the feed and handles events until Stop.
    void run() {
        start();
        while (!stopped_) {
            if (run_once()) {
                wait(last_service_time_);
            }
        }
    }

    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] const feed::FeedPublisher& feed() const noexcept { return publisher_; }
    [[nodiscard]] const FeedWorkerStats& stats() const noexcept { return stats_; }

    // How long a blocking worker may sleep at `now`, in whole milliseconds, rounded up so
    // it does not wake just before the deadline and go around again. It is never more
    // than longest_sleep_ms.
    [[nodiscard]] int sleep_timeout_ms(Timestamp now) const noexcept {
        constexpr Timestamp kMillisecond = 1'000'000;
        const Timestamp deadline = publisher_.next_deadline();
        if (deadline <= now) {
            return 0;
        }
        const Timestamp left = deadline - now;
        const Timestamp rounded_up = (left / kMillisecond) + (left % kMillisecond != 0 ? 1 : 0);
        const auto longest = static_cast<Timestamp>(longest_sleep_ms_);
        return static_cast<int>(std::min(rounded_up, longest));
    }

private:
    static feed::FeedSink& checked_sink(const Options& options) {
        if (options.sink == nullptr) {
            throw std::invalid_argument("FeedWorker needs a sink");
        }
        return *options.sink;
    }

    // Called when the queue is empty. Spinning just pauses the core for a moment. Blocking
    // sleeps until the doorbell rings or the next snapshot or heartbeat is due.
    void wait(Timestamp now) {
        if (mode_ == WaitMode::Spin || bell_ == nullptr) {
            concurrency::cpu_relax();
            return;
        }
        const int timeout_ms = sleep_timeout_ms(now);
        if (timeout_ms == 0) {
            return;  // Something is due already, so go around again instead of sleeping.
        }
        if (bell_->prepare_to_sleep()) {
            ++stats_.sleeps;
            bell_->sleep(timeout_ms);
        }
    }

    EventQueue* events_;
    WaitMode mode_;
    Doorbell* bell_;
    Clock clock_;
    feed::FeedPublisher publisher_;
    std::size_t max_levels_per_side_;
    std::size_t max_events_per_batch_;
    int longest_sleep_ms_;
    FeedWorkerStats stats_;
    Timestamp last_service_time_ = 0;
    bool stopped_ = false;
};

}  // namespace litbook::pipeline
