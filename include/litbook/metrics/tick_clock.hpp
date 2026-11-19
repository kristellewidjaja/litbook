#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace litbook::metrics {

// A low-overhead timer for measuring single operations. It reads the CPU's own counter
// instead of asking the operating system for the time.
//
// On 64-bit ARM it reads cntvct_el0, the generic timer. Its frequency is fixed and
// known (cntfrq_el0), but it is slow. On Apple silicon it ticks at 24 MHz, so one tick
// is about 41.7 ns, which is longer than a typical book operation. Single operations
// then measure as 0 or 1 tick, so only percentiles well above one tick mean anything.
// tick_ns() reports the tick length so results can say so.
//
// On x86-64 it reads the time stamp counter (rdtsc), which ticks at a fixed rate of
// roughly the CPU's base clock, so well under 1 ns. That rate is not exposed directly,
// so the first call to tick_ns() measures it against steady_clock for about 20 ms.
//
// On anything else it falls back to std::chrono::steady_clock.
//
// now() never goes backwards on one core. Neither counter stops the CPU from running
// instructions out of order, so a measured region may be slightly blurred at both ends.
// The ARM read uses an isb barrier to reduce that.
class TickClock {
public:
    [[nodiscard]] static std::uint64_t now() noexcept {
#if defined(__aarch64__)
        std::uint64_t ticks = 0;  // NOLINT(misc-const-correctness): written by the asm
        asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks) : : "memory");
        return ticks;
#elif defined(__x86_64__)
        return __rdtsc();
#else
        return static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#endif
    }

    // Length of one tick in nanoseconds.
    [[nodiscard]] static double tick_ns() noexcept {
        static const double ns = measure_tick_ns();
        return ns;
    }

    [[nodiscard]] static std::uint64_t to_ns(std::uint64_t ticks) noexcept {
        return static_cast<std::uint64_t>(std::llround(static_cast<double>(ticks) * tick_ns()));
    }

private:
    static double measure_tick_ns() noexcept {
#if defined(__aarch64__)
        std::uint64_t hz = 0;  // NOLINT(misc-const-correctness): written by the asm
        asm volatile("mrs %0, cntfrq_el0" : "=r"(hz));
        return 1e9 / static_cast<double>(hz);
#elif defined(__x86_64__)
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        const std::uint64_t c0 = now();
        while (Clock::now() - t0 < std::chrono::milliseconds(20)) {
        }
        const std::uint64_t c1 = now();
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
        return elapsed / static_cast<double>(c1 - c0);
#else
        return 1e9 * std::chrono::steady_clock::period::num /
               std::chrono::steady_clock::period::den;
#endif
    }
};

}  // namespace litbook::metrics
