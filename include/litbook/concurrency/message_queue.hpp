#pragma once

#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <type_traits>

namespace litbook::concurrency {

// What a queue may carry. A message is copied into a slot by plain assignment and copied
// out the same way, so it must be trivially copyable (no heap memory of its own, no
// custom copy logic) and default constructible (the slots are built empty at startup).
// This rules out std::string or std::vector members, which would allocate on the hot path.
template <class T>
concept QueueElement = std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>;

// The interface shared by SpscQueue and MutexQueue, so tests, benchmarks and the
// threaded pipeline can be written once and run with either queue.
//
// One thread (the producer) calls try_push. One other thread (the consumer) calls
// front, pop and try_pop. capacity and size_approx may be called from either thread.
template <class Q>
concept MessageQueue = requires(Q& queue, const Q& const_queue, const typename Q::value_type& value,
                                typename Q::value_type& out) {
    { queue.try_push(value) } -> std::same_as<bool>;
    { queue.try_pop(out) } -> std::same_as<bool>;
    { queue.front() } -> std::same_as<const typename Q::value_type*>;
    queue.pop();
    { const_queue.capacity() } -> std::same_as<std::size_t>;
    { const_queue.size_approx() } -> std::same_as<std::size_t>;
};

// The largest capacity either queue accepts. It keeps the slot array well under the
// size where capacity times the message size could overflow.
inline constexpr std::size_t kMaxQueueCapacity = std::size_t{1} << 30;

// Both queues require a capacity that is a power of two between 1 and kMaxQueueCapacity,
// so a slot is found with a bit mask instead of a division. This throws
// std::invalid_argument otherwise and returns the capacity unchanged.
inline std::size_t checked_queue_capacity(std::size_t capacity) {
    const bool power_of_two = capacity != 0 && (capacity & (capacity - 1)) == 0;
    if (!power_of_two || capacity > kMaxQueueCapacity) {
        throw std::invalid_argument("queue capacity must be a power of two in [1, 2^30]");
    }
    return capacity;
}

}  // namespace litbook::concurrency
