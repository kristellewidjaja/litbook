#pragma once

#include "litbook/concurrency/message_queue.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/messages.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace litbook::pipeline {

// Counters of the gateway side of the reply queue.
struct PumpStats {
    std::uint64_t messages = 0;  // Message replies handed to the session layer.
    std::uint64_t done = 0;      // RequestDone replies.
    std::uint64_t drains = 0;    // Calls to drain() that found at least one reply.
};

// The gateway thread's end of the reply queue, used as the EpollGateway's pump. Every
// round of the gateway loop calls drain(), which hands replies to the session layer
// (deliver for a message, request_done at the end of a request) and then lets the session
// layer retry whatever had to wait for room in the request queue. In WaitMode::Block the
// gateway sleeps in epoll_wait, and the reply doorbell's eventfd is in its epoll set, so
// a ring from the matching thread wakes it.
//
// Invariants. Only the gateway thread calls these functions, so it is the single
// consumer of the reply queue. stopped() becomes true once the Stopped reply arrived,
// and the matching thread pushes nothing after it.
template <class Sessions, concurrency::MessageQueue ReplyQueue>
    requires std::same_as<typename ReplyQueue::value_type, Reply>
class ReplyPump {
public:
    // `bell` is nullptr when the gateway spins. `max_batch` bounds the replies handled
    // per drain(), so one busy account cannot keep the loop from reading sockets.
    ReplyPump(Sessions& sessions, ReplyQueue& replies, Doorbell* bell, std::size_t max_batch)
        : sessions_(&sessions),
          replies_(&replies),
          bell_(bell),
          max_batch_(max_batch == 0 ? 1 : max_batch) {}

    // Sleeping is allowed only when no reply is waiting. A reply left over from a
    // drain that hit max_batch would otherwise sit there until the next ring or tick.
    [[nodiscard]] bool prepare_to_sleep() {
        if (replies_->front() != nullptr) {
            return false;
        }
        return bell_ == nullptr || bell_->prepare_to_sleep();
    }

    void woke_up() noexcept {
        if (bell_ != nullptr) {
            bell_->woke_up();
        }
    }

    [[nodiscard]] int wake_fd() const noexcept { return bell_ == nullptr ? -1 : bell_->fd(); }

    void drain(protocol::Timestamp now) {
        std::size_t handled = 0;
        while (handled < max_batch_) {
            const Reply* reply = replies_->front();
            if (reply == nullptr) {
                break;
            }
            switch (reply->kind) {
                case ReplyKind::Message:
                    ++stats_.messages;
                    sessions_->deliver(reply->owner, reply->bytes(), now);
                    break;
                case ReplyKind::RequestDone:
                    ++stats_.done;
                    sessions_->request_done(reply->owner, now);
                    break;
                case ReplyKind::Stopped:
                    stopped_ = true;
                    break;
            }
            // The reply was read in place, so it is popped only after it was handled.
            replies_->pop();
            ++handled;
        }
        if (handled > 0) {
            ++stats_.drains;
        }
        sessions_->retry_waiting(now);
    }

    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] const PumpStats& stats() const noexcept { return stats_; }

private:
    Sessions* sessions_;
    ReplyQueue* replies_;
    Doorbell* bell_;
    std::size_t max_batch_;
    PumpStats stats_;
    bool stopped_ = false;
};

}  // namespace litbook::pipeline
