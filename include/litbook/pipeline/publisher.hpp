#pragma once

#include "litbook/concurrency/cpu_relax.hpp"
#include "litbook/concurrency/message_queue.hpp"
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

// The publisher thread's side of the pipeline, for now only a consumer that counts what
// it receives. In Phase 8 it turns the events into the UDP market data feed, and in
// Phase 9 it also writes the journal. It exists already so the matching thread has a
// real third consumer to keep fed, and so the event stream can be tested.
//
// Invariants. Exactly one thread calls step or run, and it is the only consumer of the
// event queue. After Stop, stopped() is true and nothing more is popped.
template <concurrency::MessageQueue EventQueue>
    requires std::same_as<typename EventQueue::value_type, MarketEvent>
class PublisherStub {
public:
    // `bell` wakes this thread in WaitMode::Block, and is nullptr when it spins.
    PublisherStub(EventQueue& events, WaitMode mode, Doorbell* bell)
        : events_(&events), mode_(mode), bell_(bell) {}

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
    bool stopped_ = false;
};

}  // namespace litbook::pipeline
