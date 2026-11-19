// Measures how long each single book operation takes and prints latency percentiles
// (p50 up to p99.99 and the maximum) for both books on several workloads.
//
// Google Benchmark reports averages, which hide the rare slow operations that matter
// most in trading. This tool times every operation separately with TickClock and
// records it in a LatencyHistogram.
//
// How a run works. The operation stream is generated before any timing. For every
// book and workload there is one untimed warm-up pass, then kRuns timed passes, each
// into a fresh book that is built before the clock starts. Each percentile printed is
// the median of that percentile over the timed passes. The development machine is a
// VM, and one pass that the host interrupts can move p99.99 of all passes together by
// a lot, while the median ignores a single bad pass. The max is different. It is the
// largest value of any pass, because the worst case should never be hidden. The tool
// also prints the spread of p99.9 across the passes, so an unstable row is easy to see.
//
// What the numbers mean. Each value is service time, the time one call to submit,
// cancel or modify takes, including the listener calls it makes. Requests are applied
// back to back with no target rate, so coordinated omission does not apply here.
// Measuring an operation also costs two timer reads. The tool measures that cost and
// prints it as "timer", so it can be compared with the book numbers. When the timer
// ticks slower than the operations (one tick is about 41.7 ns on Apple silicon), low
// percentiles come out as 0 or 1 tick. Only percentiles of several ticks or more are
// meaningful, and the tool marks the ones that are not.
//
// Usage: litbook_latency [operations per pass]   (default 1,000,000)

#include "litbook/book/book_config.hpp"
#include "litbook/book/events.hpp"
#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/metrics/latency_histogram.hpp"
#include "litbook/metrics/tick_clock.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

#include "book/workload.hpp"
#include "market_workload.hpp"

namespace litbook::bench {
namespace {

using metrics::LatencyHistogram;
using metrics::TickClock;

constexpr int kRuns = 5;
constexpr std::array<double, 6> kPercentiles = {50, 90, 99, 99.9, 99.99, 100};

// Counts events through the same virtual calls a real listener would get, and stores
// nothing, so the timing measures the book and not an event recorder.
class CountingListener final : public OrderBookListener {
public:
    void on_accepted(const OrderAccepted& /*e*/) override { ++events_; }
    void on_rejected(const OrderRejected& /*e*/) override { ++events_; }
    void on_trade(const Trade& /*e*/) override { ++events_; }
    void on_canceled(const OrderCanceled& /*e*/) override { ++events_; }
    void on_cancel_rejected(const CancelRejected& /*e*/) override { ++events_; }
    void on_modified(const OrderModified& /*e*/) override { ++events_; }
    void on_modify_rejected(const ModifyRejected& /*e*/) override { ++events_; }
    void on_level_update(const LevelUpdate& /*e*/) override { ++events_; }
    [[nodiscard]] std::uint64_t events() const noexcept { return events_; }

private:
    std::uint64_t events_ = 0;
};

// The band is wide enough that the drifting middle price never leaves it, and the
// capacity is well above the largest book these workloads build (see "peak orders").
constexpr BookConfig kConfig{.min_price = 50'000, .max_price = 150'000, .max_orders = 1U << 17};

// A workload around a middle price of 100,000 where 5% of operations are modifies.
// target is the book size the cancel rate steers toward, aggressive is the percent of new
// orders that trade on arrival, distance_p sets how close to the best new orders land
// (larger is closer), and drift is the percent chance per operation of a one-tick move.
constexpr MarketParams market(const char* name, std::uint64_t seed, int operations,
                              std::size_t target, int aggressive, double distance_p, int drift) {
    return {.name = name,
            .seed = seed,
            .operations = operations,
            .start_mid = 100'000,
            .target_orders = target,
            .modify_percent = 5,
            .aggressive_percent = aggressive,
            .distance_p = distance_p,
            .drift_percent = drift};
}

// The histograms hold raw ticks, not nanoseconds. That keeps the per-operation cost to
// one subtraction, and percentiles are converted to nanoseconds only when printed.
template <class Book>
LatencyHistogram time_pass(const std::vector<testing::Operation>& ops, std::uint64_t& events,
                           std::size_t& peak_orders) {
    CountingListener listener;
    auto book = std::make_unique<Book>(listener, kConfig);
    LatencyHistogram ticks;
    for (const testing::Operation& op : ops) {
        const std::uint64_t start = TickClock::now();
        testing::apply(*book, op);
        const std::uint64_t end = TickClock::now();
        ticks.record(end - start);
        peak_orders = std::max(peak_orders, book->order_count());  // outside the timed region
    }
    events += listener.events();
    return ticks;
}

// The cost of measuring nothing, i.e. two timer reads back to back.
LatencyHistogram time_empty(std::size_t samples) {
    LatencyHistogram ticks;
    for (std::size_t i = 0; i < samples; ++i) {
        const std::uint64_t start = TickClock::now();
        const std::uint64_t end = TickClock::now();
        ticks.record(end - start);
    }
    return ticks;
}

void print_header() {
    std::printf("%-34s", "book / workload");
    for (const double p : kPercentiles) {
        if (p == 100) {
            std::printf("%10s", "max");
        } else {
            std::array<char, 16> name{};
            std::snprintf(name.data(), name.size(), "p%g", p);
            std::printf("%10s", name.data());
        }
    }
    std::printf("%16s%12s\n", "p99.9 range", "peak orders");
}

// The value of percentile p in each pass, sorted.
std::vector<std::uint64_t> per_pass(const std::vector<LatencyHistogram>& passes, double p) {
    std::vector<std::uint64_t> values;
    values.reserve(passes.size());
    for (const LatencyHistogram& h : passes) {
        values.push_back(h.percentile(p));
    }
    std::ranges::sort(values);
    return values;
}

// Prints one row in nanoseconds. Each percentile is the median over the passes, the
// max is the largest of any pass, and the last column is the p99.9 spread. A value of
// at most 2 ticks is below what the clock can resolve, and is marked with '<'.
void print_row(std::string_view label, const std::vector<LatencyHistogram>& passes) {
    const double tick = TickClock::tick_ns();
    std::printf("%-34.*s", static_cast<int>(label.size()), label.data());
    for (const double p : kPercentiles) {
        const std::vector<std::uint64_t> values = per_pass(passes, p);
        const std::uint64_t t = p == 100 ? values.back() : values[values.size() / 2];
        std::printf("%9.0f%c", static_cast<double>(t) * tick, t <= 2 ? '<' : ' ');
    }
    const std::vector<std::uint64_t> p999 = per_pass(passes, 99.9);
    std::printf("%7.0f..%-7.0f", static_cast<double>(p999.front()) * tick,
                static_cast<double>(p999.back()) * tick);
}

// Returns false if the book emitted no events at all, which would mean nothing real
// was measured.
template <class Book>
bool measure(std::string_view book_name, std::string_view workload,
             const std::vector<testing::Operation>& ops) {
    std::uint64_t events = 0;
    std::size_t peak = 0;
    (void)time_pass<Book>(ops, events, peak);  // warm-up pass for caches and branch predictors
    std::vector<LatencyHistogram> passes;
    passes.reserve(kRuns);
    for (int run = 0; run < kRuns; ++run) {
        passes.push_back(time_pass<Book>(ops, events, peak));
    }
    std::array<char, 64> label{};
    std::snprintf(label.data(), label.size(), "%.*s / %.*s", static_cast<int>(book_name.size()),
                  book_name.data(), static_cast<int>(workload.size()), workload.data());
    print_row(label.data(), passes);
    std::printf("%12zu\n", peak);
    return events > 0;
}

}  // namespace
}  // namespace litbook::bench

int main(int argc, char** argv) {
    using namespace litbook;
    using namespace litbook::bench;
    const int operations = argc > 1 ? std::atoi(argv[1]) : 1'000'000;
    if (operations <= 0) {
        std::fprintf(stderr, "usage: litbook_latency [operations per pass]\n");
        return 1;
    }

    // Market is a typical liquid stock.
    // MarketDeep spreads 20,000 orders over hundreds of price levels. MarketAggressive
    // trades much more often.
    const std::array<MarketParams, 3> workloads = {{
        market("Market", 201, operations, 2'000, 4, 0.25, 1),
        market("MarketDeep", 202, operations, 20'000, 4, 0.02, 1),
        market("MarketAggressive", 203, operations, 2'000, 20, 0.25, 2),
    }};

    std::printf(
        "timer: TickClock, 1 tick = %.2f ns. Values in ns; '<' = at most 2 ticks, below "
        "timer resolution.\n",
        metrics::TickClock::tick_ns());
    std::printf(
        "%d operations per pass, 1 warm-up pass + %d timed passes. Median over "
        "passes; max is the largest of any pass.\n\n",
        operations, kRuns);
    print_header();
    const std::vector<metrics::LatencyHistogram> empty = {
        time_empty(static_cast<std::size_t>(operations))};
    print_row("timer (two reads, nothing between)", empty);
    std::printf("\n");
    for (const MarketParams& w : workloads) {
        const std::vector<testing::Operation> ops = MarketGenerator(w).generate();
        const bool map_ok = measure<MapOrderBook>("MapOrderBook", w.name, ops);
        const bool fast_ok = measure<FastOrderBook>("FastOrderBook", w.name, ops);
        if (!map_ok || !fast_ok) {
            std::fprintf(stderr, "a book emitted no events, so nothing was measured\n");
            return 1;
        }
    }
    return 0;
}
