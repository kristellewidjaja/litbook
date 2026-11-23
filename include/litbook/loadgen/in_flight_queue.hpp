#pragma once

// The requests of one session that were sent and not answered yet, in the order they were
// sent.
//
// A request is answered by its first reply. An Enter Order gets Order Accepted or
// Rejected, a Replace Order gets Order Replaced or Rejected (for its new UserRefNum), and
// a Cancel Order gets Order Canceled with reason U. Other replies (executions, the cancel
// of an IOC remainder, the cancel that follows a failed replace) only follow a first reply
// and answer nothing.
//
// Some requests never get any reply. A Cancel or Replace that names an order which is no
// longer live (it filled while the request was on its way) is ignored by the exchange
// without a word (docs/PROTOCOL.md, session rules 5 and 6). The exchange handles one
// account's requests in the order they were sent and sends their replies in that order. So
// when a reply answers a request, every older request still in the queue was handled
// without a reply, and answer() takes them out and counts them as ignored.
//
// The memory is a ring allocated in the constructor, so nothing allocates after it.
//
// These invariants always hold. size() <= capacity(). The entries are in send order, and
// the oldest is at the front.

#include "litbook/protocol/messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <variant>
#include <vector>

namespace litbook::loadgen {

enum class RequestKind : std::uint8_t {
    Enter,
    Cancel,
    Replace,
    // An Enter Order for a stock that is not listed, so it is always rejected. Its reply
    // proves that every request sent before it was handled (see LoadWorker).
    Fence,
};

inline constexpr std::size_t kRequestKinds = 4;

// The kind of first reply a message is.
enum class ReplyKind : std::uint8_t {
    Accepted,      // Order Accepted, answers an Enter.
    Rejected,      // Rejected, answers an Enter, a Replace or a Fence.
    Replaced,      // Order Replaced, answers a Replace.
    UserCanceled,  // Order Canceled with reason U, answers a Cancel.
};

// One request on its way. `ref` is the UserRefNum the first reply will carry, which is the
// new one for a Replace and the order's own one for a Cancel.
struct InFlight {
    RequestKind kind = RequestKind::Enter;
    protocol::UserRefNum ref = 0;
    // The timed pass the request belongs to, or kWarmupPass.
    std::uint32_t pass = 0;
    std::uint64_t scheduled_ns = 0;  // When it was due.
    std::uint64_t sent_ns = 0;       // When it was really handed to the socket.
};

inline constexpr std::uint32_t kWarmupPass = 0xFFFF'FFFF;

// What answer() found.
struct Answer {
    bool found = false;
    InFlight request;  // The request that was answered, when found.
    // The older requests that were taken out because they got no reply, by kind.
    std::array<std::uint32_t, kRequestKinds> ignored{};
};

// Whether a reply of kind `reply` can be the first reply to a request of kind `request`.
[[nodiscard]] constexpr bool answers(ReplyKind reply, RequestKind request) noexcept {
    switch (reply) {
        case ReplyKind::Accepted:
            return request == RequestKind::Enter;
        case ReplyKind::Rejected:
            return request == RequestKind::Enter || request == RequestKind::Replace ||
                   request == RequestKind::Fence;
        case ReplyKind::Replaced:
            return request == RequestKind::Replace;
        case ReplyKind::UserCanceled:
            return request == RequestKind::Cancel;
    }
    return false;
}

// The kind and UserRefNum of a reply that can be a first reply, or nothing for replies
// that never are (executions, and cancels for any reason other than U).
struct ReplyKey {
    ReplyKind kind;
    protocol::UserRefNum ref;
};

[[nodiscard]] inline std::optional<ReplyKey> reply_key(
    const protocol::OutboundMessage& m) noexcept {
    if (const auto* a = std::get_if<protocol::AcceptedMessage>(&m)) {
        return ReplyKey{.kind = ReplyKind::Accepted, .ref = a->user_ref};
    }
    if (const auto* r = std::get_if<protocol::RejectedMessage>(&m)) {
        return ReplyKey{.kind = ReplyKind::Rejected, .ref = r->user_ref};
    }
    if (const auto* r = std::get_if<protocol::ReplacedMessage>(&m)) {
        return ReplyKey{.kind = ReplyKind::Replaced, .ref = r->user_ref};
    }
    if (const auto* c = std::get_if<protocol::CanceledMessage>(&m);
        c != nullptr && c->reason == protocol::WireCancelReason::UserRequested) {
        return ReplyKey{.kind = ReplyKind::UserCanceled, .ref = c->user_ref};
    }
    return std::nullopt;
}

class InFlightQueue {
public:
    // Throws std::invalid_argument if capacity is 0.
    explicit InFlightQueue(std::size_t capacity) : slots_(checked(capacity)) {}

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool full() const noexcept { return size_ == slots_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    // Adds a request at the back. Returns false and changes nothing if the queue is full.
    bool push(const InFlight& request) noexcept {
        if (full()) {
            return false;
        }
        slots_[index(size_)] = request;
        ++size_;
        return true;
    }

    // The oldest request. The queue must not be empty.
    [[nodiscard]] const InFlight& front() const noexcept { return slots_[front_]; }

    // Looks for the oldest request that a reply of this kind with this UserRefNum answers.
    // If there is one, it and every older request are taken out, and the older ones are
    // counted as ignored. If there is none, the reply only followed an earlier first
    // reply, and nothing changes.
    Answer answer(ReplyKind reply, protocol::UserRefNum ref) noexcept {
        Answer result;
        std::size_t position = 0;
        while (position < size_) {
            const InFlight& r = slots_[index(position)];
            if (r.ref == ref && answers(reply, r.kind)) {
                break;
            }
            ++position;
        }
        if (position == size_) {
            return result;
        }
        for (std::size_t i = 0; i < position; ++i) {
            ++result.ignored[static_cast<std::size_t>(slots_[front_].kind)];
            pop_front();
        }
        result.found = true;
        result.request = slots_[front_];
        pop_front();
        return result;
    }

    // Takes everything out, for a session that ended, and returns how many there were.
    std::size_t clear() noexcept {
        const std::size_t n = size_;
        size_ = 0;
        front_ = 0;
        return n;
    }

private:
    static std::size_t checked(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("InFlightQueue capacity must be at least 1");
        }
        return capacity;
    }

    // The slot of the entry `position` places behind the front.
    [[nodiscard]] std::size_t index(std::size_t position) const noexcept {
        const std::size_t i = front_ + position;
        return i < slots_.size() ? i : i - slots_.size();
    }

    void pop_front() noexcept {
        front_ = index(1);
        --size_;
    }

    std::vector<InFlight> slots_;
    std::size_t front_ = 0;
    std::size_t size_ = 0;
};

}  // namespace litbook::loadgen
