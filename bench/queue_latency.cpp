// litbook_queue_latency measures passing messages between two threads.
//
//   litbook_queue_latency [ROUND_TRIPS_PER_PASS] [--pin CPU_A CPU_B] [--only SETUP]
//                         [--detail] [--idle-spinner CPU]
//
// It runs three setups and prints one row for each.
//   spsc          SpscQueue, both threads spin on try_push and try_pop.
//   mutex spin    MutexQueue, both threads spin on try_push and try_pop, so the only
//                 difference from spsc is the lock.
//   mutex wait    MutexQueue with push_wait and pop_wait, so a thread that finds the
//                 queue empty or full sleeps on a condition variable. This is the usual
//                 way a mutex queue is written. It runs a tenth of the round trips,
//                 because every hop may need the kernel to wake a thread.
//
// Ping-pong. The main thread pushes a message into one queue and spins (or waits) for
// the reply on a second queue. A helper thread pops each message and pushes it straight
// back. Each round trip is two hops between cores. A single hop is near the timer's
// resolution (one TickClock tick is 41.67 ns on the M1 VM), so the mean is computed from
// the total time of the pass with steady_clock, and the per round trip percentiles come
// from TickClock and are only meaningful well above one tick.
//
// Throughput. A helper thread pushes 20 times as many messages as there are round
// trips, as fast as it can, and the main thread pops them all. The time runs from the
// moment the producer is released until the last message has been popped.
//
// After one untimed warm up pass, 5 timed passes run. Each printed value is the median
// over the passes, except max, which is the largest of any pass (the same method as
// litbook_latency, D-034). With --pin the main thread runs on CPU_A and the helper on
// CPU_B. Inside the Docker VM those are virtual CPUs (see thread_affinity.hpp). Both may
// be the same CPU, which only makes sense for mutex wait, because a spinning thread
// would hold the CPU its partner needs.
//
// The other options help explain a result rather than produce one.
//   --only SETUP        runs only spsc, mutex-spin or mutex-wait.
//   --detail            prints one line per timed ping-pong pass, with the largest round
//                       trip and where in the pass it happened, and the CPU each thread
//                       started and ended on and how often it moved. Asking for the CPU
//                       on every round trip costs a little time, so these passes are
//                       slightly slower than normal ones.
//   --idle-spinner CPU  runs one more thread on CPU with the SCHED_IDLE policy, which
//                       spins and gives way to any other thread at once. It keeps that
//                       virtual CPU from going idle, which shows whether waking an idle
//                       virtual CPU is what makes some passes slow.

#include "litbook/concurrency/cpu_relax.hpp"
#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/concurrency/thread_affinity.hpp"
#include "litbook/metrics/latency_histogram.hpp"
#include "litbook/metrics/tick_clock.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace litbook;  // NOLINT(google-build-using-namespace): a small program.
using concurrency::MutexQueue;
using concurrency::SpscQueue;
using metrics::LatencyHistogram;
using metrics::TickClock;
using Clock = std::chrono::steady_clock;

constexpr int kPasses = 5;
constexpr std::size_t kCapacity = 1024;
constexpr std::uint64_t kThroughputFactor = 20;

// 64 bytes, the size the Phase 7b pipeline plans to send.
struct Message {
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, 7> payload{};
};
static_assert(sizeof(Message) == 64);

// Which CPUs to pin the two threads to, or -1 for no pinning.
struct Pinning {
    int main_cpu = -1;
    int helper_cpu = -1;
};

// What one thread's CPU did during a pass.
struct CpuTrace {
    int first = -1;
    int last = -1;
    std::uint64_t moves = 0;

    void note() noexcept {
        const int cpu = concurrency::current_cpu();
        if (first < 0) {
            first = cpu;
        } else if (cpu != last) {
            ++moves;
        }
        last = cpu;
    }
};

// Pins the calling thread if a CPU was asked for. main() has already checked that both
// CPUs are allowed for this process, so the call cannot fail here.
void pin_if_requested(int cpu) noexcept {
    if (cpu >= 0) {
        (void)concurrency::pin_current_thread(cpu);
    }
}

// The two ways a thread can wait for a queue. Spin keeps calling the try_ function.
// Wait uses the MutexQueue calls that sleep.
enum class Waiting : std::uint8_t { Spin, Sleep };

template <class Queue, Waiting Mode>
void push(Queue& queue, const Message& message) {
    if constexpr (Mode == Waiting::Sleep) {
        queue.push_wait(message);
    } else {
        while (!queue.try_push(message)) {
        }
    }
}

template <class Queue, Waiting Mode>
void pop(Queue& queue, Message& message) {
    if constexpr (Mode == Waiting::Sleep) {
        queue.pop_wait(message);
    } else {
        while (!queue.try_pop(message)) {
        }
    }
}

struct PingPongPass {
    LatencyHistogram round_trip;  // Nanoseconds per round trip, from TickClock.
    double mean_ns = 0;           // Total pass time divided by round trips.
    std::uint64_t max_index = 0;  // Which round trip was the largest.
    CpuTrace main_cpu;            // Only filled in with --detail.
    CpuTrace helper_cpu;
};

template <class Queue, Waiting Mode>
PingPongPass ping_pong_pass(std::uint64_t round_trips, const Pinning& pins, bool detail) {
    Queue requests(kCapacity);
    Queue replies(kCapacity);
    PingPongPass pass;
    // The helper writes its trace and main reads it only after the join.
    std::thread echo([&requests, &replies, round_trips, &pins, detail, &pass] {
        pin_if_requested(pins.helper_cpu);
        Message message;
        for (std::uint64_t i = 0; i < round_trips; ++i) {
            pop<Queue, Mode>(requests, message);
            push<Queue, Mode>(replies, message);
            if (detail) {
                pass.helper_cpu.note();
            }
        }
    });

    Message request;
    Message reply;
    std::uint64_t largest = 0;
    const Clock::time_point start = Clock::now();
    for (std::uint64_t i = 0; i < round_trips; ++i) {
        request.sequence = i;
        const std::uint64_t t0 = TickClock::now();
        push<Queue, Mode>(requests, request);
        pop<Queue, Mode>(replies, reply);
        const std::uint64_t ns = TickClock::to_ns(TickClock::now() - t0);
        pass.round_trip.record(ns);
        if (ns > largest) {
            largest = ns;
            pass.max_index = i;
        }
        if (reply.sequence != i) {
            throw std::runtime_error("ping-pong reply out of order");
        }
        if (detail) {
            pass.main_cpu.note();
        }
    }
    const Clock::duration elapsed = Clock::now() - start;
    echo.join();
    pass.mean_ns = static_cast<double>(std::chrono::nanoseconds(elapsed).count()) /
                   static_cast<double>(round_trips);
    return pass;
}

// Returns millions of messages per second.
template <class Queue, Waiting Mode>
double throughput_pass(std::uint64_t messages, const Pinning& pins) {
    Queue queue(kCapacity);
    std::atomic<bool> go{false};
    std::thread producer([&queue, &go, messages, &pins] {
        pin_if_requested(pins.helper_cpu);
        while (!go.load(std::memory_order_acquire)) {
        }
        Message message;
        for (std::uint64_t i = 0; i < messages; ++i) {
            message.sequence = i;
            push<Queue, Mode>(queue, message);
        }
    });

    Message message;
    const Clock::time_point start = Clock::now();
    go.store(true, std::memory_order_release);
    for (std::uint64_t i = 0; i < messages; ++i) {
        pop<Queue, Mode>(queue, message);
        if (message.sequence != i) {
            throw std::runtime_error("throughput message out of order");
        }
    }
    const Clock::duration elapsed = Clock::now() - start;
    producer.join();
    const double seconds = std::chrono::duration<double>(elapsed).count();
    return static_cast<double>(messages) / seconds / 1e6;
}

double median(std::vector<double> values) {
    std::ranges::sort(values);
    return values[values.size() / 2];
}

struct Options {
    std::uint64_t round_trips = 200'000;
    Pinning pins;
    std::string only;  // Empty means every setup.
    bool detail = false;
    int idle_spinner_cpu = -1;
};

void print_pass(int index, const PingPongPass& pass) {
    std::printf(
        "  pass %d: p50 %llu ns, p99 %llu ns, max %llu ns at round trip %llu, "
        "main CPU %d to %d (%llu moves), helper CPU %d to %d (%llu moves)\n",
        index + 1, static_cast<unsigned long long>(pass.round_trip.percentile(50)),
        static_cast<unsigned long long>(pass.round_trip.percentile(99)),
        static_cast<unsigned long long>(pass.round_trip.max()),
        static_cast<unsigned long long>(pass.max_index), pass.main_cpu.first, pass.main_cpu.last,
        static_cast<unsigned long long>(pass.main_cpu.moves), pass.helper_cpu.first,
        pass.helper_cpu.last, static_cast<unsigned long long>(pass.helper_cpu.moves));
}

template <class Queue, Waiting Mode>
void run_setup(std::string_view name, std::uint64_t round_trips, const Options& options) {
    const Pinning& pins = options.pins;
    (void)ping_pong_pass<Queue, Mode>(round_trips, pins, options.detail);  // Warm up.
    std::vector<double> mean;
    std::vector<double> p50;
    std::vector<double> p99;
    std::vector<double> p999;
    std::uint64_t max = 0;
    std::vector<PingPongPass> details;
    for (int i = 0; i < kPasses; ++i) {
        const PingPongPass pass = ping_pong_pass<Queue, Mode>(round_trips, pins, options.detail);
        mean.push_back(pass.mean_ns);
        p50.push_back(static_cast<double>(pass.round_trip.percentile(50)));
        p99.push_back(static_cast<double>(pass.round_trip.percentile(99)));
        p999.push_back(static_cast<double>(pass.round_trip.percentile(99.9)));
        max = std::max(max, pass.round_trip.max());
        if (options.detail) {
            details.push_back(pass);
        }
    }

    const std::uint64_t messages = round_trips * kThroughputFactor;
    (void)throughput_pass<Queue, Mode>(messages, pins);  // Warm up.
    std::vector<double> rate;
    rate.reserve(kPasses);
    for (int i = 0; i < kPasses; ++i) {
        rate.push_back(throughput_pass<Queue, Mode>(messages, pins));
    }

    std::printf("%-12.*s %10llu %9.1f %9.0f %9.0f %9.0f %11llu   %10.2f %8.2f %8.2f\n",
                static_cast<int>(name.size()), name.data(),
                static_cast<unsigned long long>(round_trips), median(mean), median(p50),
                median(p99), median(p999), static_cast<unsigned long long>(max), median(rate),
                *std::ranges::min_element(rate), *std::ranges::max_element(rate));
    for (std::size_t i = 0; i < details.size(); ++i) {
        print_pass(static_cast<int>(i), details[i]);
    }
}

// Reads a whole number that must be at least `low`. Returns nothing for anything else.
std::optional<long long> parse_number(std::string_view text, long long low) {
    if (text.empty()) {
        return std::nullopt;
    }
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9' || value > 1'000'000'000'000LL) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    if (value < low) {
        return std::nullopt;
    }
    return value;
}

// Returns nothing if the arguments are wrong.
std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    int i = 1;
    if (i < argc && !std::string_view(argv[i]).starts_with("--")) {
        const auto n = parse_number(argv[i], 10);
        if (!n) {
            return std::nullopt;
        }
        options.round_trips = static_cast<std::uint64_t>(*n);
        ++i;
    }
    while (i < argc) {
        const std::string_view flag = argv[i];
        const int left = argc - i - 1;  // Arguments after the flag.
        if (flag == "--pin" && left >= 2) {
            const auto a = parse_number(argv[i + 1], 0);
            const auto b = parse_number(argv[i + 2], 0);
            if (!a || !b) {
                return std::nullopt;
            }
            options.pins.main_cpu = static_cast<int>(*a);
            options.pins.helper_cpu = static_cast<int>(*b);
            i += 3;
        } else if (flag == "--only" && left >= 1) {
            options.only = argv[i + 1];
            if (options.only != "spsc" && options.only != "mutex-spin" &&
                options.only != "mutex-wait") {
                return std::nullopt;
            }
            i += 2;
        } else if (flag == "--detail") {
            options.detail = true;
            i += 1;
        } else if (flag == "--idle-spinner" && left >= 1) {
            const auto cpu = parse_number(argv[i + 1], 0);
            if (!cpu) {
                return std::nullopt;
            }
            options.idle_spinner_cpu = static_cast<int>(*cpu);
            i += 2;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

bool cpu_allowed(const std::vector<int>& allowed, int cpu) {
    return std::ranges::find(allowed, cpu) != allowed.end();
}

// The --idle-spinner thread. It lowers itself to SCHED_IDLE and spins until told to stop.
class IdleSpinner {
public:
    explicit IdleSpinner(int cpu) {
        thread_ = std::thread([this, cpu] {
            pin_if_requested(cpu);
            sched_param param{};
            param.sched_priority = 0;
            result_.store(pthread_setschedparam(pthread_self(), SCHED_IDLE, &param),
                          std::memory_order_release);
            started_.store(true, std::memory_order_release);
            while (!stop_.load(std::memory_order_acquire)) {
                concurrency::cpu_relax();
            }
        });
        while (!started_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    IdleSpinner(const IdleSpinner&) = delete;
    IdleSpinner& operator=(const IdleSpinner&) = delete;
    IdleSpinner(IdleSpinner&&) = delete;
    IdleSpinner& operator=(IdleSpinner&&) = delete;

    ~IdleSpinner() {
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }

    // 0 if the thread runs under SCHED_IDLE, otherwise the error number.
    [[nodiscard]] int result() const noexcept { return result_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int> result_{0};
    std::thread thread_;
};

}  // namespace

int main(int argc, char** argv) try {
    const std::optional<Options> parsed = parse_options(argc, argv);
    if (!parsed) {
        std::fprintf(stderr,
                     "usage: litbook_queue_latency [ROUND_TRIPS_PER_PASS] [--pin CPU_A CPU_B]\n"
                     "       [--only spsc|mutex-spin|mutex-wait] [--detail] "
                     "[--idle-spinner CPU]\n");
        return 2;
    }
    const Options& options = *parsed;
    const Pinning& pins = options.pins;
    const std::vector<int> allowed = concurrency::allowed_cpus();
    std::vector<int> wanted;
    if (pins.main_cpu >= 0) {
        wanted.push_back(pins.main_cpu);
        wanted.push_back(pins.helper_cpu);
    }
    if (options.idle_spinner_cpu >= 0) {
        wanted.push_back(options.idle_spinner_cpu);
    }
    for (const int cpu : wanted) {
        if (!cpu_allowed(allowed, cpu)) {
            std::fprintf(stderr, "litbook_queue_latency: CPU %d is not available\n", cpu);
            return 2;
        }
    }
    pin_if_requested(pins.main_cpu);

    std::optional<IdleSpinner> spinner;
    if (options.idle_spinner_cpu >= 0) {
        spinner.emplace(options.idle_spinner_cpu);
        if (spinner->result() != 0) {
            std::fprintf(stderr, "litbook_queue_latency: could not use SCHED_IDLE (error %d)\n",
                         spinner->result());
            return 1;
        }
    }

    std::printf("queue latency: %d passes, capacity %zu, %zu byte messages, timer tick %.2f ns, ",
                kPasses, kCapacity, sizeof(Message), TickClock::tick_ns());
    if (pins.main_cpu >= 0) {
        std::printf("main thread on CPU %d, helper on CPU %d", pins.main_cpu, pins.helper_cpu);
    } else {
        std::printf("threads not pinned");
    }
    if (options.idle_spinner_cpu >= 0) {
        std::printf(", idle spinner on CPU %d", options.idle_spinner_cpu);
    }
    std::printf("%s\n", options.detail ? ", detail on (slightly slower passes)" : "");
    std::printf("%-12s %10s %9s %9s %9s %9s %11s   %10s %8s %8s\n", "", "round", "mean", "p50",
                "p99", "p99.9", "max", "M msg/s", "min", "max");
    std::printf("%-12s %10s %9s %9s %9s %9s %11s   %10s %8s %8s\n", "setup", "trips", "(ns)",
                "(ns)", "(ns)", "(ns)", "(ns)", "median", "pass", "pass");

    const std::uint64_t round_trips = options.round_trips;
    const bool all = options.only.empty();
    if (all || options.only == "spsc") {
        run_setup<SpscQueue<Message>, Waiting::Spin>("spsc", round_trips, options);
    }
    if (all || options.only == "mutex-spin") {
        run_setup<MutexQueue<Message>, Waiting::Spin>("mutex spin", round_trips, options);
    }
    if (all || options.only == "mutex-wait") {
        run_setup<MutexQueue<Message>, Waiting::Sleep>(
            "mutex wait", std::max<std::uint64_t>(round_trips / 10, 10), options);
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_queue_latency: %s\n", e.what());
    return 1;
}
