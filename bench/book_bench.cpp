// These are the first benchmarks of both order books, with two per-operation
// microbenchmarks and three mixed operation streams. Every benchmark runs the same code
// against MapOrderBook and FastOrderBook with the same config, so the pairs are directly
// comparable.
//
// The listener only counts events (a virtual call per event, as in real use). Recording
// events into a vector would measure the recorder, not the book. Operation streams are
// generated before timing starts. Books are built and destroyed outside the timed region,
// because construction is startup cost, not hot-path cost. Microbenchmarks leave the book
// in the state they found it, so every iteration does the same work against a
// 2,000-order background book. The `time/op` counter is time per book operation (submit,
// cancel or modify).
//
// Run with the release preset only. The logged results are in docs/BENCHMARKS.md.

#include "litbook/book/book_config.hpp"
#include "litbook/book/events.hpp"
#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/types.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "book/workload.hpp"

namespace litbook {
namespace {

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

constexpr Price kMid = 10'000;
constexpr int kBackgroundLevels = 100;  // levels on each side
constexpr int kOrdersPerLevel = 10;
constexpr OrderId kFirstBenchId = 1'000'000;
constexpr BookConfig kConfig{
    .min_price = kMid - 5'000, .max_price = kMid + 5'000, .max_orders = 1U << 17};

// Bids at kMid-1 .. kMid-100 and asks at kMid+1 .. kMid+100, 10 orders of 100 each.
// kMid itself stays empty (a two-tick spread).
template <class Book>
void fill_background(Book& book) {
    OrderId id = 1;
    for (Price level = 1; level <= kBackgroundLevels; ++level) {
        for (int k = 0; k < kOrdersPerLevel; ++k) {
            book.submit({.id = id++, .side = Side::Buy, .price = kMid - level, .qty = 100});
            book.submit({.id = id++, .side = Side::Sell, .price = kMid + level, .qty = 100});
        }
    }
}

void set_time_per_op(benchmark::State& state, double ops_per_iteration) {
    // An inverted rate gives seconds per operation, printed with an SI prefix such as
    // "25.2ns".
    state.counters["time/op"] =
        benchmark::Counter(ops_per_iteration, benchmark::Counter::kIsIterationInvariantRate |
                                                  benchmark::Counter::kInvert);
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(ops_per_iteration));
}

// A passive order joins the back of an existing level, then is canceled. Cycles
// through all 100 bid levels, so it is not always the best one.
template <class Book>
void BM_RestThenCancel_ExistingLevel(benchmark::State& state) {
    CountingListener listener;
    Book book(listener, kConfig);
    fill_background(book);
    OrderId id = kFirstBenchId;
    Price level = 0;
    for (auto _ : state) {
        book.submit({.id = id, .side = Side::Buy, .price = kMid - 1 - level, .qty = 100});
        book.cancel(id);
        ++id;
        level = (level + 1) % kBackgroundLevels;
    }
    benchmark::DoNotOptimize(listener.events());
    set_time_per_op(state, 2);
}

// A passive order opens a new best level inside the spread, then is canceled (the
// level is created and destroyed each time).
template <class Book>
void BM_RestThenCancel_NewLevel(benchmark::State& state) {
    CountingListener listener;
    Book book(listener, kConfig);
    fill_background(book);
    OrderId id = kFirstBenchId;
    for (auto _ : state) {
        book.submit({.id = id, .side = Side::Buy, .price = kMid, .qty = 100});
        book.cancel(id);
        ++id;
    }
    benchmark::DoNotOptimize(listener.events());
    set_time_per_op(state, 2);
}

// A sell rests at kMid (a new best ask), then a buy at kMid fills it completely.
// That makes one trade, empties the level, and moves the best ask back to kMid+1.
template <class Book>
void BM_RestThenMatch(benchmark::State& state) {
    CountingListener listener;
    Book book(listener, kConfig);
    fill_background(book);
    OrderId id = kFirstBenchId;
    for (auto _ : state) {
        book.submit({.id = id, .side = Side::Sell, .price = kMid, .qty = 100});
        book.submit({.id = id + 1, .side = Side::Buy, .price = kMid, .qty = 100});
        id += 2;
    }
    benchmark::DoNotOptimize(listener.events());
    set_time_per_op(state, 2);
}

// Replays a pre-generated stream from the randomized-test generator into a fresh book.
// These streams mix every order type and include invalid requests on purpose (zero
// quantities, duplicate IDs, bad post-only combinations), so they are a stress mix,
// not a model of a real market.
template <class Book>
void BM_Stream(benchmark::State& state, const testing::WorkloadParams& p) {
    const std::vector<testing::Operation> ops = testing::generate(p);
    CountingListener listener;
    std::unique_ptr<Book> book;
    for (auto _ : state) {
        state.PauseTiming();
        book.reset();  // destroy the previous iteration's book while the clock is paused
        book = std::make_unique<Book>(listener, kConfig);
        state.ResumeTiming();
        for (const testing::Operation& op : ops) {
            testing::apply(*book, op);
        }
    }
    benchmark::DoNotOptimize(listener.events());
    set_time_per_op(state, static_cast<double>(ops.size()));
}

// The workload config's band and capacity are ignored, because every benchmark uses kConfig.
const testing::WorkloadParams kBalanced{.name = "Balanced",
                                        .seed = 101,
                                        .operations = 100'000,
                                        .min_price = kMid - 50,
                                        .max_price = kMid + 50,
                                        .max_lots = 20,
                                        .cancel_percent = 30,
                                        .modify_percent = 10,
                                        .config = BookConfig{},
                                        .owners = 0};
const testing::WorkloadParams kCancelHeavy{.name = "CancelHeavy",
                                           .seed = 102,
                                           .operations = 100'000,
                                           .min_price = kMid - 50,
                                           .max_price = kMid + 50,
                                           .max_lots = 20,
                                           .cancel_percent = 60,
                                           .modify_percent = 20,
                                           .config = BookConfig{},
                                           .owners = 0};
const testing::WorkloadParams kWidePrices{.name = "WidePrices",
                                          .seed = 103,
                                          .operations = 100'000,
                                          .min_price = kMid - 2'000,
                                          .max_price = kMid + 2'000,
                                          .max_lots = 20,
                                          .cancel_percent = 15,
                                          .modify_percent = 10,
                                          .config = BookConfig{},
                                          .owners = 0};

BENCHMARK_TEMPLATE(BM_RestThenCancel_ExistingLevel, MapOrderBook);
BENCHMARK_TEMPLATE(BM_RestThenCancel_ExistingLevel, FastOrderBook);
BENCHMARK_TEMPLATE(BM_RestThenCancel_NewLevel, MapOrderBook);
BENCHMARK_TEMPLATE(BM_RestThenCancel_NewLevel, FastOrderBook);
BENCHMARK_TEMPLATE(BM_RestThenMatch, MapOrderBook);
BENCHMARK_TEMPLATE(BM_RestThenMatch, FastOrderBook);
BENCHMARK_CAPTURE(BM_Stream<MapOrderBook>, Balanced, kBalanced)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Stream<FastOrderBook>, Balanced, kBalanced)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Stream<MapOrderBook>, CancelHeavy, kCancelHeavy)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Stream<FastOrderBook>, CancelHeavy, kCancelHeavy)
    ->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Stream<MapOrderBook>, WidePrices, kWidePrices)->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(BM_Stream<FastOrderBook>, WidePrices, kWidePrices)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace litbook
