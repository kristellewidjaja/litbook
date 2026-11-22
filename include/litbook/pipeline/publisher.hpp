#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/concurrency/cpu_relax.hpp"
#include "litbook/concurrency/message_queue.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/messages.hpp"

#include <concepts>
#include <cstdint>

namespace litbook::pipeline {

// What the publisher has seen. The checksum mixes every field of every event in order,
// so two runs that produced the same market events in the same order have the same sum.
struct PublisherStats {
    std::uint64_t trades = 0;
    std::uint64_t level_updates = 0;
    std::uint64_t traded_qty = 0;
    std::uint64_t checksum = 0;
};

// A publisher thread that only counts what it receives. It is the default publisher of
// ThreadedGateway, for runs that need no market data feed. FeedWorker (feed_worker.hpp)
// is the one that publishes the real feed. Both have the same constructor, add_symbol,
// step, run and stopped, so ThreadedGateway can run either one.
//
// Invariants. Exactly one thread calls step or run, and it is the only consumer of the
// event queue. After Stop, stopped() is true and nothing more is popped.
template <concurrency::MessageQueue EventQueue>
class PublisherStub {
    // A static_assert and not a requires clause, so the template has exactly the shape
    // BasicThreadedGateway's Publisher parameter asks for.
    static_assert(std::same_as<typename EventQueue::value_type, MarketEvent>,
                  "the publisher consumes MarketEvents");

public:
    // The stub needs no options. The type exists so ThreadedGateway can build any publisher
    // the same way.
    struct Options {};

    // `bell` wakes this thread in WaitMode::Block, and is nullptr when it spins.
    PublisherStub(EventQueue& events, WaitMode mode, Doorbell* bell)
        : events_(&events), mode_(mode), bell_(bell) {}

    // The constructor ThreadedGateway uses. The stub reads no clock and has no options.
    PublisherStub(EventQueue& events, WaitMode mode, Doorbell* bell, Timestamp (* /*clock*/)(),
                  const Options& /*options*/)
        : PublisherStub(events, mode, bell) {}

    // The stub publishes nothing per stock, so there is nothing to list.
    void add_symbol(SymbolId /*id*/, Ticker /*ticker*/, const BookConfig& /*config*/) noexcept {}

    // The recovery calls (see FeedWorker). The stub keeps no levels and has no session, so
    // it only counts the level updates it was given.
    void restore(const MarketEvent& event) noexcept {
        if (event.kind == MarketEventKind::LevelUpdate) {
            ++restored_;
        }
    }
    void begin_new_session(std::uint64_t /*restart_sequence*/) noexcept {}
    [[nodiscard]] std::uint64_t restored() const noexcept { return restored_; }

    // Pops and handles one event. Returns false if the queue was empty or the publisher
    // has already stopped.
    bool step() {
        if (stopped_) {
            return false;
        }
        MarketEvent e;
        if (!events_->try_pop(e)) {
            return false;
        }
        switch (e.kind) {
            case MarketEventKind::Trade:
                ++stats_.trades;
                stats_.traded_qty += e.qty;
                break;
            case MarketEventKind::LevelUpdate:
                ++stats_.level_updates;
                break;
            case MarketEventKind::Stop:
                stopped_ = true;
                return true;
        }
        mix(e);
        return true;
    }

    // Handles events until Stop.
    void run() {
        while (!stopped_) {
            if (step()) {
                continue;
            }
            if (mode_ == WaitMode::Spin || bell_ == nullptr) {
                concurrency::cpu_relax();
            } else if (bell_->prepare_to_sleep()) {
                bell_->sleep(-1);
            }
        }
    }

    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] const PublisherStats& stats() const noexcept { return stats_; }

private:
    // Folds one event into the checksum. Multiplying by an odd constant before adding
    // the next field makes the result depend on the order of the fields and events.
    void mix(const MarketEvent& e) noexcept {
        constexpr std::uint64_t kOdd = 0x100000001B3ULL;
        std::uint64_t h = stats_.checksum;
        h = h * kOdd + static_cast<std::uint64_t>(e.kind);
        h = h * kOdd + static_cast<std::uint64_t>(e.side);
        h = h * kOdd + e.symbol;
        h = h * kOdd + e.order_count;
        h = h * kOdd + static_cast<std::uint64_t>(e.price);
        h = h * kOdd + e.qty;
        h = h * kOdd + e.match_id;
        stats_.checksum = h;
    }

    EventQueue* events_;
    WaitMode mode_;
    Doorbell* bell_;
    PublisherStats stats_;
    std::uint64_t restored_ = 0;
    bool stopped_ = false;
};

}  // namespace litbook::pipeline
