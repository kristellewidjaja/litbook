#pragma once

#include "litbook/concurrency/cache_line.hpp"
#include "litbook/concurrency/message_queue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace litbook::concurrency {

// A bounded lock-free queue for exactly one producer thread and one consumer thread.
//
// How it works. The messages live in a fixed array of slots whose size is a power of
// two, allocated once in the constructor. The producer owns a counter tail_ (how many
// messages were ever pushed) and the consumer owns a counter head_ (how many were ever
// popped). The queue holds tail_ - head_ messages, and message number i lives in slot
// i & mask_. The counters are 64 bits and only grow, so they never wrap in practice
// (at a billion messages a second that would take over 500 years). This is why no slot
// has to be left empty to tell a full queue from an empty one.
//
// Memory ordering. A push first writes the slot and then stores tail_ with release.
// The consumer loads tail_ with acquire, so once it sees the new count it is guaranteed
// to also see the finished slot. A pop works the same way in the other direction. It
// reads the slot and then stores head_ with release, and the producer loads head_ with
// acquire before it reuses that slot, so it never overwrites a message that is still
// being read. Each side reads its own counter with relaxed ordering, because no other
// thread writes it. Nothing stronger than acquire and release is needed.
//
// Avoiding cache traffic. Every variable that changes sits alone on its own cache line
// (see kCacheLineSize), so the producer writing tail_ does not slow down the consumer
// writing head_. On top of that, each side keeps a private copy of the other side's
// counter (cached_head_ and cached_tail_). The producer only reads the real head_ when
// its copy says the queue is full, and the consumer only reads the real tail_ when its
// copy says the queue is empty. When the queue is neither, a push or pop touches no
// cache line the other thread is writing. The slot array also has a cache line of
// unused slots at each end, so other heap data next to it cannot share a line with the
// first or last slot. This is the design of Erik Rigtorp's SPSCQueue.
//
// Invariants: 0 <= tail_ - head_ <= capacity(). Only the producer writes tail_ and
// cached_head_, and only the consumer writes head_ and cached_tail_. Nothing allocates
// after the constructor. See D-049.
//
// Calling try_push from two threads at once, or the consumer calls from two threads at
// once, is a data race. This is not checked.
template <QueueElement T>
class SpscQueue {
public:
    using value_type = T;

    // Throws std::invalid_argument unless capacity is a power of two in [1, 2^30].
    explicit SpscQueue(std::size_t capacity)
        : capacity_(checked_queue_capacity(capacity)),
          mask_(capacity - 1),
          slots_(capacity + 2 * kPaddingSlots) {}

    // The two threads hold references to one queue, so it must never move.
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;
    ~SpscQueue() = default;

    // Producer only. Copies value into the queue and returns true, or returns false and
    // changes nothing if the queue is full.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - cached_head_ == capacity_) {
            // Our copy says the queue is full. The consumer may have popped since we
            // last looked, so read the real head before giving up.
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - cached_head_ == capacity_) {
                return false;
            }
        }
        slot(tail) = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Returns the oldest message without removing it, or nullptr if the
    // queue is empty. The pointer stays valid until pop() is called, because the
    // producer never writes a slot that has not been popped.
    [[nodiscard]] const T* front() noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        if (head == cached_tail_) {
            // Our copy says the queue is empty. Read the real tail in case the producer
            // pushed since we last looked.
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) {
                return nullptr;
            }
        }
        return &slot(head);
    }

    // Consumer only. Removes the oldest message. It must only be called after front()
    // returned a message, and the caller must be done reading that message. For speed
    // this precondition is not checked.
    void pop() noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        head_.store(head + 1, std::memory_order_release);
    }

    // Consumer only. Copies the oldest message into out and removes it, or returns false
    // and leaves out alone if the queue is empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const T* message = front();
        if (message == nullptr) {
            return false;
        }
        out = *message;
        pop();
        return true;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // How many messages are queued. Either thread may call it, but the other thread can
    // change the answer at any moment, so it is only good for statistics and tests.
    // head_ is read first. tail_ can only grow after that, so the difference is never
    // negative, but it can briefly look larger than the capacity, so it is capped.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        const std::uint64_t size = tail - head;
        return size > capacity_ ? capacity_ : static_cast<std::size_t>(size);
    }

private:
    // Enough unused slots to fill one cache line, placed before and after the real
    // slots. Rounded up, so at least one.
    static constexpr std::size_t kPaddingSlots = (kCacheLineSize - 1) / sizeof(T) + 1;

    T& slot(std::uint64_t index) noexcept {
        return slots_[kPaddingSlots + static_cast<std::size_t>(index & mask_)];
    }

    // This line is only read after the constructor, so both threads can keep a copy of
    // it in their caches at the same time.
    alignas(kCacheLineSize) const std::size_t capacity_;
    const std::uint64_t mask_;
    std::vector<T> slots_;

    // Written by the producer on every push and read by the consumer when it thinks the
    // queue is empty.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_{0};

    // The producer's private copy of head_.
    alignas(kCacheLineSize) std::uint64_t cached_head_ = 0;

    // Written by the consumer on every pop and read by the producer when it thinks the
    // queue is full.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0};

    // The consumer's private copy of tail_.
    alignas(kCacheLineSize) std::uint64_t cached_tail_ = 0;
};

}  // namespace litbook::concurrency
