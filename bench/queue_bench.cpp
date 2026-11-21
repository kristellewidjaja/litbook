// The cost of the queue calls themselves, with no second thread. One thread pushes and
// then pops, so every cache line stays in its own core's cache and the numbers show the
// work each call does (the atomic loads and stores for SpscQueue, the lock and unlock
// and notify for MutexQueue). The cross-thread cost is measured by litbook_queue_latency.

#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>

namespace {

using litbook::concurrency::MutexQueue;
using litbook::concurrency::SpscQueue;

// 64 bytes, the size the Phase 7b pipeline plans to send.
struct BenchMessage {
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, 7> payload{};
};
static_assert(sizeof(BenchMessage) == 64);

// One push followed by one pop per iteration.
template <class Queue>
void BM_QueuePushPop(benchmark::State& state) {
    Queue queue(1024);
    BenchMessage in;
    BenchMessage out;
    for (auto _ : state) {
        ++in.sequence;
        benchmark::DoNotOptimize(queue.try_push(in));
        benchmark::DoNotOptimize(queue.try_pop(out));
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}

// 64 pushes and then 64 pops per iteration, so the counters move through the array the
// way they do when a burst arrives. Items are messages.
template <class Queue>
void BM_QueueBurst64(benchmark::State& state) {
    Queue queue(1024);
    BenchMessage in;
    BenchMessage out;
    for (auto _ : state) {
        for (int i = 0; i < 64; ++i) {
            ++in.sequence;
            benchmark::DoNotOptimize(queue.try_push(in));
        }
        for (int i = 0; i < 64; ++i) {
            benchmark::DoNotOptimize(queue.try_pop(out));
        }
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations() * 64);
}

BENCHMARK_TEMPLATE(BM_QueuePushPop, SpscQueue<BenchMessage>);
BENCHMARK_TEMPLATE(BM_QueuePushPop, MutexQueue<BenchMessage>);
BENCHMARK_TEMPLATE(BM_QueueBurst64, SpscQueue<BenchMessage>);
BENCHMARK_TEMPLATE(BM_QueueBurst64, MutexQueue<BenchMessage>);

}  // namespace
