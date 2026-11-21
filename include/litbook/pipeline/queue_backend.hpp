#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/concurrency/message_queue.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/order_backend.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/types.hpp"

#include <concepts>
#include <cstdint>
#include <span>
#include <type_traits>

namespace litbook::pipeline {

static_assert(std::is_same_v<Timestamp, protocol::Timestamp>,
              "the pipeline and the order entry layer must use the same time type");
static_assert(kMaxFrame == protocol::kMaxMessageLength,
              "a queue message must hold the longest OUCH message");

// The order backend the session layer uses on the gateway thread of the pipeline. It
// sends each order to the matching thread through the request queue instead of handling
// it, so the session layer's forward() returns at once.
//
// It still decodes each frame once, only to check it. A malformed frame must close the
// connection before any later packet of that client is forwarded, and only the gateway
// thread can know that in time. The frame then travels as raw bytes, because the decoded
// message holds a Ticker, which has no default value, and because the Phase 9 journal
// will want the raw bytes. The matching thread decodes it again (about 20 ns).
//
// A full queue is reported as ForwardResult::Busy and cancel_all() returns false. The
// gateway never waits for the matching thread.
//
// add_symbol and set_risk_limits change the order entry layer that the matching thread
// owns. They are startup calls and must happen before the matching thread starts.
//
// Invariants. Only the gateway thread calls forward and cancel_all, so it is the single
// producer of the request queue. Every Frame request it pushes decoded successfully.
template <OrderBook Book, concurrency::MessageQueue RequestQueue>
    requires std::same_as<typename RequestQueue::value_type, Request>
class QueueBackend {
public:
    static constexpr bool kAsynchronous = true;

    // `matching` is the order entry layer of the matching thread, used only for startup
    // calls. `bell` wakes the matching thread in WaitMode::Block, and is nullptr when it
    // spins.
    QueueBackend(protocol::OrderEntry<Book>& matching, RequestQueue& requests, Doorbell* bell)
        : matching_(&matching), requests_(&requests), bell_(bell) {}

    QueueBackend(const QueueBackend&) = delete;
    QueueBackend& operator=(const QueueBackend&) = delete;
    QueueBackend(QueueBackend&&) = delete;
    QueueBackend& operator=(QueueBackend&&) = delete;
    ~QueueBackend() = default;

    // Replies reach the session layer through the reply queue (see reply_pump.hpp), so
    // the target is not needed here.
    void attach(gateway::ReplyTarget& /*target*/) noexcept {}

    gateway::ForwardResult forward(OwnerId owner, std::span<const std::byte> frame,
                                   protocol::Timestamp now) {
        if (!protocol::decode_inbound(frame).message) {
            ++malformed_;
            return gateway::ForwardResult::Malformed;
        }
        // A frame that decodes is never longer than kMaxFrame.
        if (!requests_->try_push(make_frame_request(owner, frame, now))) {
            ++busy_;
            return gateway::ForwardResult::Busy;
        }
        ring();
        return gateway::ForwardResult::Taken;
    }

    bool cancel_all(OwnerId owner, protocol::Timestamp now) {
        if (!requests_->try_push(make_cancel_all_request(owner, now))) {
            ++busy_;
            return false;
        }
        ring();
        return true;
    }

    void set_risk_limits(OwnerId owner, const protocol::RiskLimits& limits) {
        matching_->set_risk_limits(owner, limits);
    }

    SymbolId add_symbol(Ticker ticker, const BookConfig& config) {
        return matching_->add_symbol(ticker, config);
    }

    [[nodiscard]] OwnerId max_owners() const noexcept { return matching_->config().max_owners; }

    // Frames that did not decode, and pushes refused because the queue was full.
    [[nodiscard]] std::uint64_t malformed() const noexcept { return malformed_; }
    [[nodiscard]] std::uint64_t busy() const noexcept { return busy_; }

private:
    void ring() noexcept {
        if (bell_ != nullptr) {
            bell_->ring();
        }
    }

    protocol::OrderEntry<Book>* matching_;
    RequestQueue* requests_;
    Doorbell* bell_;
    std::uint64_t malformed_ = 0;
    std::uint64_t busy_ = 0;
};

}  // namespace litbook::pipeline
