#pragma once

#include "litbook/concurrency/cache_line.hpp"
#include "litbook/gateway/socket.hpp"

#include <atomic>
#include <cstdint>

namespace litbook::pipeline {

// How the threads of the pipeline wait when their input queue is empty.
enum class WaitMode : std::uint8_t {
    Spin,   // Keep checking the queue. Lowest latency, but each thread uses a whole core.
    Block,  // Sleep in the kernel until a Doorbell rings. Frees the core, costs a wakeup.
};

// Wakes up one sleeping consumer thread when its producer has pushed something. It is
// used in WaitMode::Block, next to a queue, and works with any queue.
//
// The point is to make no system call while the consumer is awake. The state has three
// values. Awake means the consumer is running and will look at its queue again anyway.
// Sleeping means it decided to sleep. Rung means a producer rang since the consumer last
// woke up.
//
// The consumer drains its queue until it is empty and then calls prepare_to_sleep(),
// which swaps the state to Sleeping. If the old state was Rung, a producer pushed
// something after the consumer's last look, so the consumer must not sleep and checks
// its queue again. Otherwise it sleeps (sleep(), or the eventfd in its own epoll set)
// and then calls woke_up().
//
// The producer pushes first and then calls ring(), which swaps the state to Rung. Only if
// the old state was Sleeping does it write to the eventfd, which wakes the consumer.
//
// Why no wakeup can be lost. Both sides change the state with an atomic exchange, and all
// exchanges of one variable happen in a single order. Take a push that the consumer's
// last look at the queue missed. If the producer's ring() comes first in that order, the
// consumer's prepare_to_sleep() sees Rung and does not sleep. If prepare_to_sleep()
// comes first, ring() sees Sleeping and writes the eventfd. The exchanges are acq_rel, so
// a consumer that sees Rung also sees the pushed message when it looks again. A stale
// Rung from an earlier push only costs one extra look at an empty queue.
//
// This needs no fences (GCC's ThreadSanitizer does not support
// std::atomic_thread_fence). The mutex and condition variable alternative cannot be used
// by the gateway thread, which already sleeps in epoll_wait.
//
// Invariants. Exactly one consumer thread calls prepare_to_sleep, sleep and woke_up. Any
// number of producer threads may call ring. The eventfd is non-blocking.
class Doorbell {
public:
    // Throws std::system_error if the eventfd cannot be created.
    Doorbell();

    Doorbell(const Doorbell&) = delete;
    Doorbell& operator=(const Doorbell&) = delete;
    Doorbell(Doorbell&&) = delete;
    Doorbell& operator=(Doorbell&&) = delete;
    ~Doorbell() = default;

    // Producer side. Call after pushing one or more messages.
    void ring() noexcept;

    // Consumer side. Returns true if the consumer may sleep now, and false if a ring came
    // in since it last woke up, in which case it should look at its queue again.
    [[nodiscard]] bool prepare_to_sleep() noexcept;

    // Consumer side, after prepare_to_sleep() returned true. Waits until a ring wakes it
    // or timeout_ms passes (-1 waits forever), then calls woke_up().
    void sleep(int timeout_ms) noexcept;

    // Consumer side. Marks the consumer awake and empties the eventfd. Call it after any
    // sleep that prepare_to_sleep() allowed.
    void woke_up() noexcept;

    // The eventfd, readable while a wakeup is waiting. A consumer that sleeps somewhere
    // else (the gateway in epoll_wait) watches it there.
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    // How many times ring() wrote to the eventfd, for tests and statistics.
    [[nodiscard]] std::uint64_t wakeups_sent() const noexcept {
        return wakeups_sent_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::uint32_t kAwake = 0;
    static constexpr std::uint32_t kSleeping = 1;
    static constexpr std::uint32_t kRung = 2;

    // Producers and the consumer both write this, so it sits alone on its line.
    alignas(concurrency::kCacheLineSize) std::atomic<std::uint32_t> state_{kAwake};
    alignas(concurrency::kCacheLineSize) std::atomic<std::uint64_t> wakeups_sent_{0};
    gateway::Fd fd_;
};

}  // namespace litbook::pipeline
