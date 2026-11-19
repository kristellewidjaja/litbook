// Harness check: proves Google Benchmark builds, links and runs.

#include <benchmark/benchmark.h>

#include <bit>
#include <cstdint>

namespace {

void BM_StdBitCeil(benchmark::State& state) {
    std::uint64_t x = 1;
    for (auto _ : state) {
        benchmark::DoNotOptimize(std::bit_ceil(x));
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;  // LCG: varying input
        x >>= 2;                                                  // keep bit_ceil defined
    }
}
BENCHMARK(BM_StdBitCeil);

}  // namespace
