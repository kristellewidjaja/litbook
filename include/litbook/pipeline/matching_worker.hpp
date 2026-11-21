#pragma once

#include "litbook/book/events.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/concurrency/cpu_relax.hpp"
#include "litbook/concurrency/message_queue.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/types.hpp"

#include <concepts>
#include <cstdint>
#include <span>

namespace litbook::pipeline {

// The doorbells of the pipeline, or nullptr for a thread that spins instead of sleeping.
struct Doorbells {
    Doorbell* requests = nullptr;  // Wakes the matching thread.
    Doorbell* replies = nullptr;   // Wakes the gateway thread.
    Doorbell* events = nullptr;    // Wakes the publisher thread.
};

// Counters of the matching thread.
struct MatchingStats {
    std::uint64_t requests = 0;     // Every request popped, Stop included.
    std::uint64_t frames = 0;       // Frame requests.
    std::uint64_t cancel_alls = 0;  // CancelAll requests.
    std::uint64_t malformed = 0;    // Frames that did not decode. Should stay 0.
    std::uint64_t replies = 0;      // Message replies pushed.
    std::uint64_t events = 0;       // Market events pushed, Stop included.
    // How often a push found the reply or event queue full and had to wait.
    std::uint64_t reply_waits = 0;
    std::uint64_t event_waits = 0;
};

// The matching thread's side of the pipeline. It owns the order entry layer (and so the
// engine and every book), pops requests, handles them, and pushes the replies for the
// gateway thread and the trades and level updates for the publisher thread.
//
// Every Frame request ends with exactly one RequestDone reply after its other replies,
// which is how the gateway counts requests in flight. CancelAll gets no RequestDone,
// because the gateway does not count it as in flight.
//
// A full reply or event queue is never a reason to drop a message. The worker rings the
// consumer's doorbell and spins until there is room. This cannot deadlock, because the
// gateway and the publisher never wait for the matching thread, so they keep draining.
//
// step() handles one request and can be called by hand, which is how the single-threaded
// tests drive it. run() loops until it has handled Stop.
//
// Invariants. Exactly one thread calls step or run. It is the only consumer of the
// request queue and the only producer of the reply and event queues. Nothing allocates
// after the constructor and add_symbol.
template <OrderBook Book, concurrency::MessageQueue RequestQueue,
          concurrency::MessageQueue ReplyQueue, concurrency::MessageQueue EventQueue>
    requires std::same_as<typename RequestQueue::value_type, Request> &&
                 std::same_as<typename ReplyQueue::value_type, Reply> &&
                 std::same_as<typename EventQueue::value_type, MarketEvent>
class MatchingWorker final : private protocol::OutboundSink, private protocol::MarketDataSink {
public:
    // Throws whatever OrderEntry throws for a bad configuration.
    MatchingWorker(const protocol::OrderEntryConfig& config, RequestQueue& requests,
                   ReplyQueue& replies, EventQueue& events, WaitMode mode, Doorbells bells)
        : order_entry_(*this, config),
          requests_(&requests),
          replies_(&replies),
          events_(&events),
          mode_(mode),
          bells_(bells) {
        order_entry_.set_market_data_sink(this);
    }

    // The order entry layer holds a reference to this object, so it must never move.
    MatchingWorker(const MatchingWorker&) = delete;
    MatchingWorker& operator=(const MatchingWorker&) = delete;
    MatchingWorker(MatchingWorker&&) = delete;
    MatchingWorker& operator=(MatchingWorker&&) = delete;
    ~MatchingWorker() override = default;

    // Pops and handles one request. Returns false if the queue was empty.
    bool step() {
        const Request* request = requests_->front();
        if (request == nullptr) {
            return false;
        }
        handle(*request);
        // The request is only popped after it was handled, because it was read in place.
        requests_->pop();
        ++stats_.requests;
        // One ring per request instead of one per message keeps a busy pipeline to one
        // atomic exchange per request on each doorbell.
        ring(bells_.replies);
        ring(bells_.events);
        return true;
    }

    // Handles requests until Stop, spinning or sleeping on the request doorbell when the
    // queue is empty.
    void run() {
        while (!stopped_) {
            if (step()) {
                continue;
            }
            wait_for_requests();
        }
    }

    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] const MatchingStats& stats() const noexcept { return stats_; }

    // The order entry layer. Only touch it before the thread starts (startup calls) or
    // after it has stopped.
    [[nodiscard]] protocol::OrderEntry<Book>& order_entry() noexcept { return order_entry_; }
    [[nodiscard]] const protocol::OrderEntry<Book>& order_entry() const noexcept {
        return order_entry_;
    }

private:
    void handle(const Request& request) {
        switch (request.kind) {
            case RequestKind::Frame:
                ++stats_.frames;
                if (order_entry_.on_frame(request.owner, request.bytes(), request.now) !=
                    protocol::DecodeStatus::Ok) {
                    ++stats_.malformed;
                }
                push_reply(make_done_reply(request.owner));
                return;
            case RequestKind::CancelAll:
                ++stats_.cancel_alls;
                order_entry_.cancel_all(request.owner, request.now);
                return;
            case RequestKind::Stop: {
                // The publisher gets its Stop first, so by the time the gateway sees
                // Stopped, nothing more will be pushed to either queue.
                MarketEvent stop;
                stop.kind = MarketEventKind::Stop;
                push_event(stop);
                push_reply(make_stopped_reply());
                stopped_ = true;
                return;
            }
        }
    }

    void wait_for_requests() {
        if (mode_ == WaitMode::Spin || bells_.requests == nullptr) {
            concurrency::cpu_relax();
            return;
        }
        if (bells_.requests->prepare_to_sleep()) {
            bells_.requests->sleep(-1);
        }
    }

    // The order entry layer calls this for every reply.
    void send(OwnerId owner, std::span<const std::byte> frame) override {
        ++stats_.replies;
        push_reply(make_message_reply(owner, frame));
    }

    void on_trade(SymbolId symbol, const Trade& trade) override {
        MarketEvent e;
        e.kind = MarketEventKind::Trade;
        e.side = trade.aggressor_side;
        e.symbol = symbol;
        e.price = trade.price;
        e.qty = trade.qty;
        e.match_id = trade.match_id;
        push_event(e);
    }

    void on_level_update(SymbolId symbol, const LevelUpdate& update) override {
        MarketEvent e;
        e.kind = MarketEventKind::LevelUpdate;
        e.side = update.side;
        e.symbol = symbol;
        e.order_count = update.order_count;
        e.price = update.price;
        e.qty = update.total_qty;
        push_event(e);
    }

    void push_reply(const Reply& reply) {
        if (replies_->try_push(reply)) {
            return;
        }
        ++stats_.reply_waits;
        while (!replies_->try_push(reply)) {
            // The gateway may be asleep with a full queue in front of it, so it is woken
            // before we wait for it to make room.
            ring(bells_.replies);
            concurrency::cpu_relax();
        }
    }

    void push_event(const MarketEvent& event) {
        ++stats_.events;
        if (events_->try_push(event)) {
            return;
        }
        ++stats_.event_waits;
        while (!events_->try_push(event)) {
            ring(bells_.events);
            concurrency::cpu_relax();
        }
    }

    static void ring(Doorbell* bell) noexcept {
        if (bell != nullptr) {
            bell->ring();
        }
    }

    protocol::OrderEntry<Book> order_entry_;
    RequestQueue* requests_;
    ReplyQueue* replies_;
    EventQueue* events_;
    WaitMode mode_;
    Doorbells bells_;
    MatchingStats stats_;
    bool stopped_ = false;
};

}  // namespace litbook::pipeline
