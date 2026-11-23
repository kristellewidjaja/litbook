#pragma once

// The send schedule of the load generator. The generator is open loop, so every request
// has a time at which it should be sent, fixed before the run starts. That time does not
// move when the exchange is slow. Latency is then measured from the scheduled time, so a
// stall that holds requests back is counted for every request it delayed (the problem a
// closed loop client has is called coordinated omission).
//
// The whole run is one schedule of `rate` requests per second, numbered 0, 1, 2 and so on.
// Request i is due at floor(i * 1e9 / rate) nanoseconds after the start. Session s of n
// sends requests s, s + n, s + 2n and so on, which spreads the sessions evenly in time.
//
// Everything is integer math, so the schedule never drifts the way adding a rounded
// interval again and again would. Both functions are exact for every index and time the
// generator can reach, and neither overflows for rate up to kMaxRate.

#include <cstdint>

namespace litbook::loadgen {

inline constexpr std::uint64_t kNanosPerSecond = 1'000'000'000;

// The highest rate the formulas accept, one request per nanosecond.
inline constexpr std::uint64_t kMaxRate = kNanosPerSecond;

// The time at which request `index` is due, in nanoseconds after the start, for a rate of
// `rate` requests per second. rate must be between 1 and kMaxRate.
//
// index * 1e9 would overflow 64 bits after about 1.8e10 requests, so the index is split
// into whole seconds of requests (q) and the rest (r). Then index = q * rate + r and the
// result is q seconds plus floor(r * 1e9 / rate), where r * 1e9 stays below 1e18.
[[nodiscard]] constexpr std::uint64_t scheduled_offset_ns(std::uint64_t index,
                                                          std::uint64_t rate) noexcept {
    const std::uint64_t whole_seconds = index / rate;
    const std::uint64_t rest = index % rate;
    return whole_seconds * kNanosPerSecond + rest * kNanosPerSecond / rate;
}

// How many requests are due once `elapsed_ns` nanoseconds have passed, which is the
// number of indexes i with scheduled_offset_ns(i, rate) <= elapsed_ns. Request 0 is due
// at once, so the answer is never 0.
//
// floor(i * 1e9 / rate) <= e holds exactly when i * 1e9 < (e + 1) * rate, so the count is
// the ceiling of (e + 1) * rate / 1e9. The time is split into whole seconds the same way
// as above so that nothing overflows.
[[nodiscard]] constexpr std::uint64_t sends_due(std::uint64_t elapsed_ns,
                                                std::uint64_t rate) noexcept {
    const std::uint64_t time = elapsed_ns + 1;
    const std::uint64_t whole_seconds = time / kNanosPerSecond;
    const std::uint64_t rest = time % kNanosPerSecond;
    return whole_seconds * rate + (rest * rate + kNanosPerSecond - 1) / kNanosPerSecond;
}

}  // namespace litbook::loadgen
