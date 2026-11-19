# Benchmarks

Append-only. **Never delete or edit old rows**, because the history of improvement is the point.
Every number here must come from an actual run on a recorded machine. No estimates, no numbers copied from elsewhere.

**Machine key** (add a row when a new machine is used):

| ID | Description |
|----|-------------|
| M1 | Apple M3 Pro (11 cores, 18 GiB), macOS 15.0.1 host, Docker Desktop 28.3.3 VM (11 vCPU, 7.7 GiB, kernel 6.10.14-linuxkit), Ubuntu 26.04.1 linux/arm64 container. VM, so expect noise. Google Benchmark reports the CPU as "48 MHz" here: it cannot read the real clock inside the VM, so ignore that field. |

**Results**

| Date | Commit | Machine | Compiler + flags | Benchmark | Workload | Result | Notes |
|------|--------|---------|------------------|-----------|----------|--------|-------|
| 2026-11-18 | 81fa412 + uncommitted Phase 0 scaffold | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_StdBitCeil` | `std::bit_ceil` on LCG-varying 62-bit inputs, 5 repetitions | mean 1.14 ns, median 1.13 ns, CV 2.5% | Harness check only; baseline for the hand-written `next_power_of_two` |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_RestThenCancel_ExistingLevel<MapOrderBook>` | Rest a bid at one of 100 existing levels, then cancel it; 2,000-order background book; 5 repetitions | median 48.4 ns per pair (24.2 ns/op), CV 2.9% | First book benchmarks (D-031). Counting listener |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_RestThenCancel_ExistingLevel<FastOrderBook>` | same | median 25.2 ns per pair (12.6 ns/op), CV 1.6% | |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_RestThenCancel_NewLevel<MapOrderBook>` | Rest a bid inside the spread (creates a level), then cancel it; same background; 5 repetitions | median 62.9 ns per pair (31.5 ns/op), CV 10.1% | Noisy (high CV) |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_RestThenCancel_NewLevel<FastOrderBook>` | same | median 28.3 ns per pair (14.1 ns/op), CV 0.5% | |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_RestThenMatch<MapOrderBook>` | Rest a sell inside the spread, then a buy fills it (1 trade); same background; 5 repetitions | median 63.3 ns per pair (31.7 ns/op), CV 3.0% | |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_RestThenMatch<FastOrderBook>` | same | median 35.2 ns per pair (17.6 ns/op), CV 0.7% | |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_Stream<MapOrderBook>/Balanced` | 100,000 generated ops (seed 101), prices mid±50, cancel 30%, modify 10%, all order types incl. invalid requests; fresh book per iteration; 5 repetitions | median 3.88 ms per stream (38.8 ns/op), CV 0.5% | Test-generator stress mix, not a market model |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_Stream<FastOrderBook>/Balanced` | same | median 1.92 ms per stream (19.1 ns/op), CV 1.0% | |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_Stream<MapOrderBook>/CancelHeavy` | 100,000 ops (seed 102), mid±50, cancel 60%, modify 20%; 5 repetitions | median 1.99 ms per stream (19.9 ns/op), CV 0.7% | |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_Stream<FastOrderBook>/CancelHeavy` | same | median 1.01 ms per stream (10.1 ns/op), CV 1.3% | |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_Stream<MapOrderBook>/WidePrices` | 100,000 ops (seed 103), mid±2,000, cancel 15%, modify 10%; 5 repetitions | median 4.97 ms per stream (49.7 ns/op), CV 0.1% | |
| 2026-11-19 | 85b5454 + uncommitted Phase 3b | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `BM_Stream<FastOrderBook>/WidePrices` | same | median 2.47 ms per stream (24.7 ns/op), CV 0.4% | |
| 2026-11-19 | e2f78f2 + uncommitted Phase 4a | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `litbook_latency` MapOrderBook / Market | `MarketGenerator` seed 201, 1,000,000 ops per pass, book steers to 2,000 orders (peak 1,666), 4% aggressive, distance p 0.25, modify 5% | 42< / 83< / 125 / 208 / 292 / 19,542 | First per-operation latency run (D-032 to D-034). The result columns are p50 / p90 / p99 / p99.9 / p99.99 / max in ns, each the median over 5 passes, except max, which is the largest of any pass. `<` marks values of at most 2 timer ticks (41.67 ns each), which are below the timer's resolution. p99.9 over 3 invocations: 208, 208, 208 |
| 2026-11-19 | e2f78f2 + uncommitted Phase 4a | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `litbook_latency` FastOrderBook / Market | same | 42< / 42< / 83< / 125 / 208 / 53,375 | p99.9 over 3 invocations: 125, 208, 125 |
| 2026-11-19 | e2f78f2 + uncommitted Phase 4a | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `litbook_latency` MapOrderBook / MarketDeep | seed 202, 1,000,000 ops, target 20,000 orders (peak 17,893), distance p 0.02 (hundreds of levels) | 83< / 125 / 167 / 292 / 4,625 / 870,917 | p99.9 over 3 invocations: 292, 250, 625; p99.99 4,625 / 4,542 / 4,792 (repeatable) |
| 2026-11-19 | e2f78f2 + uncommitted Phase 4a | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `litbook_latency` FastOrderBook / MarketDeep | same | 42< / 42< / 83< / 125 / 208 / 202,000 | p99.9 over 3 invocations: 125, 125, 167; p99.99 208 / 250 / 1,625 |
| 2026-11-19 | e2f78f2 + uncommitted Phase 4a | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `litbook_latency` MapOrderBook / MarketAggressive | seed 203, 1,000,000 ops, target 2,000 (peak 1,179), 20% aggressive, drift 2% | 42< / 83< / 125 / 208 / 458 / 6,314,625 | p99.9 over 3 invocations: 208, 167, 167. Max varies widely between invocations (0.15 to 6.3 ms), which is VM noise |
| 2026-11-19 | e2f78f2 + uncommitted Phase 4a | M1 | GCC 15.2, `release` preset (`-O3 -DNDEBUG`) | `litbook_latency` FastOrderBook / MarketAggressive | same | 42< / 42< / 83< / 125 / 250 / 60,167 | p99.9 over 3 invocations: 125, 167, 167 |
