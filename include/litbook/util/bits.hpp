#pragma once

#include <cstdint>

namespace litbook {

// Returns the smallest power of two that is >= x.
//
//   next_power_of_two(0)    == 1
//   next_power_of_two(1)    == 1
//   next_power_of_two(5)    == 8
//   next_power_of_two(1024) == 1024
//
// Precondition: x <= 2^63 (otherwise the answer does not fit in 64 bits).
//
// Rules: implement it with bit operations; no loops that test one bit at a time,
// no floating point, and no std::bit_ceil (the tests compare your answer against it).
//
// Later use: the order-ID hash map (Phase 3) and the ring buffer (Phase 7) round
// their capacity up to a power of two so `i % capacity` becomes `i & (capacity - 1)`.
constexpr std::uint64_t next_power_of_two(std::uint64_t x) noexcept {
    // TODO(phase-0 exercise): replace this stub with a real implementation.
    (void)x;
    return 0;
}

}  // namespace litbook
