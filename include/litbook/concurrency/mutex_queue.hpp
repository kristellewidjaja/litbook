#pragma once

#include "litbook/concurrency/message_queue.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace litbook::concurrency {

// A bounded queue protected by one mutex, with condition variables for waiting. It is
// the textbook way to pass messages between threads, and litbook keeps it as the
// baseline that SpscQueue is measured against.
//
// It has the same interface as SpscQueue (try_push, front, pop, try_pop, capacity,
// size_approx), so the same tests and benchmarks run on both. It also has push_wait and
// pop_wait, which sleep on a condition variable instead of returning false, because
// that is how a mutex queue is normally used. Like SpscQueue it is meant for one
// producer and one consumer, although the lock would also make more of either safe.
//
// Every call takes the lock, so the two threads always touch the same mutex cache line,
// and a thread that finds the lock taken may be put to sleep by the kernel. Those two
// costs are what the comparison in BENCHMARKS.md measures.
//
// Invariants: 0 <= tail_ - head_ <= capacity(), and both counters are only read or
// written while mutex_ is held. Nothing allocates after the constructor.
//
// None of the functions are noexcept, because std::mutex::lock may throw
// std::system_error if the operating system refuses the lock. That does not happen in
// normal use.
template <QueueElement T>
class MutexQueue {
public:
    using value_type = T;

    // Throws std::invalid_argument unless capacity is a power of two in [1, 2^30]. The
    // rule is the same as SpscQueue's so the two can be swapped.
    explicit MutexQueue(std::size_t capacity)
        : capacity_(checked_queue_capacity(capacity)), mask_(capacity - 1), slots_(capacity) {}

    MutexQueue(const MutexQueue&) = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;
    MutexQueue(MutexQueue&&) = delete;
    MutexQueue& operator=(MutexQueue&&) = delete;
    ~MutexQueue() = default;

    // Copies value into the queue and returns true, or returns false if it is full.
    [[nodiscard]] bool try_push(const T& value) {
        {
            const std::scoped_lock lock(mutex_);
            if (tail_ - head_ == capacity_) {
                return false;
            }
            slot(tail_) = value;
            ++tail_;
        }
        // We notify after unlocking, so the woken consumer does not wake up only to
        // find the lock still held.
        not_empty_.notify_one();
        return true;
    }

    // Waits until there is room, then copies value into the queue.
    void push_wait(const T& value) {
        {
            std::unique_lock lock(mutex_);
            not_full_.wait(lock, [this] { return tail_ - head_ < capacity_; });
            slot(tail_) = value;
            ++tail_;
        }
        not_empty_.notify_one();
    }

    // Returns the oldest message without removing it, or nullptr if the queue is empty.
    // Reading the message after the lock is released is safe, because the producer only
    // writes slots that are free, and this one stays taken until pop().
    [[nodiscard]] const T* front() {
        const std::scoped_lock lock(mutex_);
        if (tail_ == head_) {
            return nullptr;
        }
        return &slot(head_);
    }

    // Removes the oldest message. It must only be called after front() returned a
    // message. For speed this precondition is not checked.
    void pop() {
        {
            const std::scoped_lock lock(mutex_);
            ++head_;
        }
        not_full_.notify_one();
    }

    // Copies the oldest message into out and removes it, or returns false if the queue
    // is empty. This takes the lock once, not twice like front() then pop().
    [[nodiscard]] bool try_pop(T& out) {
        {
            const std::scoped_lock lock(mutex_);
            if (tail_ == head_) {
                return false;
            }
            out = slot(head_);
            ++head_;
        }
        not_full_.notify_one();
        return true;
    }

    // Waits until a message is queued, then copies it into out and removes it.
    void pop_wait(T& out) {
        {
            std::unique_lock lock(mutex_);
            not_empty_.wait(lock, [this] { return tail_ != head_; });
            out = slot(head_);
            ++head_;
        }
        not_full_.notify_one();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // How many messages are queued right now. The other thread may change it at once.
    [[nodiscard]] std::size_t size_approx() const {
        const std::scoped_lock lock(mutex_);
        return static_cast<std::size_t>(tail_ - head_);
    }

private:
    T& slot(std::uint64_t index) noexcept {
        return slots_[static_cast<std::size_t>(index & mask_)];
    }

    const std::size_t capacity_;
    const std::uint64_t mask_;
    std::vector<T> slots_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;  // Signaled after a push.
    std::condition_variable not_full_;   // Signaled after a pop.
    std::uint64_t head_ = 0;             // Messages ever popped. Guarded by mutex_.
    std::uint64_t tail_ = 0;             // Messages ever pushed. Guarded by mutex_.
};

}  // namespace litbook::concurrency
