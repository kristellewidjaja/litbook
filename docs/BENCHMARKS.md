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
