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
