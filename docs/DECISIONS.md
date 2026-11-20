# Design Decisions Log

Each entry: date, context, options considered, choice, reason. Newest at the bottom. To reverse a decision, add a new entry that references the old one. Never edit history.

---

## D-001 (2026-11-18): Development environment
**Context:** The host is macOS 15 on an Apple M3 Pro (arm64). The project needs epoll (Phase 6), perf (Phase 4) and Linux-specific socket options, none of which exist on macOS.
**Options:**
1. Build natively on macOS and swap epoll for kqueue. Rejected: trading firms run Linux, and two code paths would double the gateway work.
2. Docker dev container, linux/arm64 (native architecture, runs in Docker Desktop's VM).
3. Docker container, linux/amd64 under emulation. Rejected: emulated x86 makes every benchmark meaningless.
4. A remote Linux box (e.g. UIUC EWS, a cloud VM). Kept as an *additional* benchmark machine later, not the daily environment.

**Choice:** Option 2, an Ubuntu linux/arm64 container with GCC and Clang, entered via `scripts/dev.sh` or VS Code Dev Containers.
**Reason:** Real Linux APIs at native speed with no extra hardware needed.
**Consequences to remember:**
- `rdtsc` is x86-only. On arm64 the equivalent is the generic timer `cntvct_el0` (fixed frequency, lower resolution than TSC). The Phase 4 latency recorder must choose deliberately; that choice will get its own entry.
- Docker Desktop runs Linux in a VM, so benchmarks carry VM noise and every BENCHMARKS.md row must say so.
- perf inside Docker Desktop's VM may not have access to hardware PMU counters. **Not yet verified**; check in Phase 4 before relying on it. Software-event sampling (`-e cpu-clock`) is the fallback.

## D-002 (2026-11-18): Project documentation structure
**Context:** The project spans many work sessions, and anyone (including future me) should be able to resume it cold.
**Choice:** docs/PROGRESS.md (current state and next step), docs/DECISIONS.md (why), docs/ROADMAP.md (what), docs/BENCHMARKS.md (append-only numbers), docs/learn/ (study guides per phase).
**Reason:** Separating state, rationale, plan and measurements keeps each file short and easy to keep accurate.

## D-003 (2026-11-18): Phase 4 pre-split
**Context:** Phase 4 bundles a latency recorder, benchmark workloads, an ITCH parser with replay, and a profiling pass. That is too much for one session.
**Choice:** Planned sub-parts 4a (recorder + benchmarks), 4b (ITCH), 4c (perf hotspot fix). Other phases get split only when a split proves necessary.

## D-004 (2026-11-18): Project name
**Context:** The project needed a name for the repo, the C++ namespace and the binaries.
**Options:** ringmatch (from the ring buffers used between threads), litbook.
**Choice:** litbook. Namespace `litbook`, headers under `include/litbook/`.
**Reason:** A "lit" market is one with a publicly visible order book, as opposed to a dark pool where resting orders are hidden. That is exactly what this project builds: a lit venue that publishes its book over a market data feed.

## D-005 (2026-11-18): Build configurations via CMake presets
**Context:** Four build types are needed (debug, release, ASan+UBSan, TSan) on two compilers.
**Options:** (1) Document long `cmake -D...` command lines. (2) Shell scripts per configuration. (3) `CMakePresets.json`.
**Choice:** Presets: `debug`, `release`, `asan`, `tsan` (GCC) plus `-clang` variants, each building into `build/<preset>/` with Ninja.
**Reason:** Presets are standard CMake, understood by IDEs and CI, and keep every configuration reproducible from one name. ASan and TSan are separate presets because they cannot share a binary (both replace the memory allocator and shadow memory layout).
Sanitizer flags are applied globally (including GoogleTest), because TSan misses races and reports false positives when only part of the program is instrumented.

## D-006 (2026-11-18): Dependencies via FetchContent, pinned by hash
**Options:** (1) System packages. (2) vcpkg or Conan. (3) Git submodules. (4) CMake FetchContent.
**Choice:** FetchContent from release tarballs with SHA-256: GoogleTest v1.17.0, Google Benchmark v1.9.1, declared `SYSTEM` so their headers do not trigger project warnings.
**Reason:** No extra tool to install or explain; exact versions are pinned; the hash rejects a tampered or changed download. A package manager pays off only with many dependencies. Submodules add a manual `git submodule update` step that is easy to forget.

## D-007 (2026-11-18): Stricter warnings than the minimum
**Choice:** Beyond `-Wall -Wextra -Wpedantic -Werror`, also `-Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual`, applied per target through an interface library (`litbook::warnings`) so third-party code is unaffected.
**Reason:** Quantities are unsigned. A silent signed/unsigned or 64-to-32-bit conversion in matching code is exactly the bug class `-Wsign-conversion`/`-Wconversion` catch at compile time.

## D-008 (2026-11-18): CI runs on the native GitHub runner, not the dev container
**Options:** (1) Build the dev container image in CI and run inside it. (2) Use `ubuntu-24.04` runners with their preinstalled GCC 14 and Clang 18.
**Choice:** Option 2, a matrix of {debug, release, asan, tsan} by {gcc-14, clang-18}.
**Reason:** Much faster (no image build) and simpler. It also adds coverage: CI is x86-64 while development is arm64, so architecture-specific assumptions get caught. TSan on these runners needs `vm.mmap_rnd_bits=28` (a known ASLR incompatibility).

## D-009 (2026-11-18): One test executable
**Choice:** A single `litbook_tests` binary with all test files, registered through `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST)`.
**Reason:** It links fast and every test is still visible to ctest individually. Discovery at test time (not build time) avoids running sanitizer-instrumented binaries during the build. Split into several binaries later only if link times grow.

## D-010 (2026-11-18): Dev image is Ubuntu 26.04 LTS with distro-default compilers
**Options:** (1) Ubuntu 24.04 (GCC 13/14, Clang 18). (2) Ubuntu 26.04 LTS (GCC 15.2, Clang 21). (3) Install newer compilers from the upstream LLVM apt repository.
**Choice:** Ubuntu 26.04 LTS with unversioned packages (`g++`, `clang`), installed versions GCC 15.2 and Clang 21.1.8.
**Reason:** Newest C++20/23 support from the standard archive with no extra package sources. Being newer than CI (GCC 14 and Clang 18 on ubuntu-24.04) is useful, because code must compile on both old and new compilers.
Verified 2026-11-18: ASan (heap-buffer-overflow), UBSan (signed overflow) and TSan (data race) all detect deliberate bugs inside the Docker Desktop VM with both compilers, and TSan needed no ASLR workaround there.

## D-011 (2026-11-18): `#pragma once` over include guards
**Choice:** `#pragma once`; clang-tidy's `portability-avoid-pragma-once` is disabled.
**Reason:** It is supported by every compiler targeted (GCC, Clang) and cannot go wrong by copy-pasting a guard name. Include guards are only needed for exotic compilers or headers reachable through multiple filesystem paths, neither of which applies here.

## D-012 (2026-11-18): Planning notes stay out of the repository
**Context:** D-002 set up docs for state, plan, rationale and measurements.
**Choice:** The roadmap, progress notes and study notes are kept locally and gitignored. Only this decisions log and the benchmark log are published.
**Reason:** The repository should present the engineering record (why things were built this way, and measured results), not personal to-do lists.

## D-013 (2026-11-18): Core numeric types
**Choice:** `Price = int64_t` (ticks), `Qty = uint32_t`, `AggregateQty = uint64_t` for sums, `OrderId = MatchId = uint64_t`. Plain type aliases.
**Options rejected:** a single 64-bit quantity type (bigger order objects for no benefit, since OUCH and ITCH carry 4-byte share counts); strong wrapper types such as `struct Price { int64_t v; }` (they prevent mixing a price with a quantity, but add a lot of boilerplate at this stage; `-Wconversion`/`-Wsign-conversion` catch the most dangerous mixes).
**Reason:** Order quantity matches the wire protocols. Totals over many orders cannot overflow (a test rests four `UINT32_MAX` orders at one price).

## D-014 (2026-11-18): Reference book from standard containers
**Choice:** `std::map<Price, Level>` per side (bids with `std::greater<>`, so `begin()` is best on both sides), a `std::list` FIFO per level, and an `std::unordered_map<OrderId, Location>` index storing side, price and list iterator.
**Options rejected:** a sorted `std::vector` of levels (cache-friendly, but O(L) insertion of a new price; a performance concern for Phase 3); `std::deque` per level (no O(1) erase in the middle, so cancels would shift elements).
**Reason:** Each container's guarantees map directly to an exchange rule: sorted prices give price priority, list order gives time priority, and stable list iterators give O(1) cancel without disturbing queue position.

## D-015 (2026-11-18): All outcomes are events with a fixed ordering contract
**Choice:** The book reports everything (accept, reject, trade, cancel, cancel-reject, level update) through `OrderBookListener` virtual callbacks, in the order documented in `events.hpp`. Methods return `void`.
**Options rejected:** return codes plus separate callbacks (two sources of truth); a templated listener (faster, but premature before measuring in Phase 4).
**Reason:** One stream feeds the gateway, the market data feed and the journal, and lets the Phase 3 fast book be verified by exact stream comparison. The listener must not re-enter the book.

## D-016 (2026-11-18): Order ID uniqueness is per resting order
**Choice:** The book rejects an ID only while an order with that ID rests; IDs of filled or canceled orders may be reused. A cancel for a filled order is `CancelRejected(UnknownOrderId)`.
**Reason:** Remembering every ID ever seen grows memory without bound. Day-long uniqueness per session belongs in the gateway (Phase 6).

## D-017 (2026-11-18): Model-based testing against a naive oracle
**Choice:** Besides scenario tests, randomized workloads run against both `MapOrderBook` and `tests/book/naive_book.hpp` (one flat vector, linear scans, no shared code), asserting identical event streams and invariants after every operation.
**Reason:** Random operation sequences reach combinations nobody writes by hand. Verified non-vacuous by mutation: a planted LIFO bug and a planted `<` vs `<=` crossing bug were each caught (8 and 17 failing tests).

## D-018 (2026-11-18): Hand-written exercises live outside the repository
**Choice:** Each phase's practice function lives in `exercises/phase-N/` (gitignored). Its tests carry the ctest label `exercise`, are excluded from every normal test preset, and run via `ctest --preset exercises`. Project code never depends on exercise code (for example, the project uses `std::bit_ceil`, not the exercise's `next_power_of_two`).
**Reason:** Unfinished exercises must never break CI or block the next phase, and exercises can be done at any time.

## D-019 (2026-11-18): Order entry through an `OrderRequest` struct
**Options:** (1) Keep growing `add_limit(id, owner, side, price, qty, type, tif, post_only)`. (2) One struct with named fields.
**Choice:** `submit(const OrderRequest&)` with designated initializers at call sites; `add_limit` removed.
**Reason:** Eight positional integer-like arguments make silent swaps (price for quantity) easy. Named fields are self-documenting, adding a field does not break callers, and the struct mirrors the Phase 5 "enter order" message.

## D-020 (2026-11-18): Order type semantics
- **Market:** no price limit (implemented as an extreme limit so every price crosses); never rests. `GoodTillCancel` on a market order behaves like IOC. `OrderAccepted.price` is 0.
- **IOC / FOK:** an unfillable remainder or an unfillable FOK is `OrderAccepted` then `OrderCanceled(Unfilled)`. A reject is reserved for invalid requests.
- **Post-only:** rejected with `PostOnlyWouldCross` if it would trade on arrival, checked against any resting order including the same owner's. Rejected over "sliding" to a non-crossing price for simplicity and explicitness. Only valid with Limit + GTC (`InvalidOrderType` otherwise).
- **Validation order** (first failure wins, identical for every book): zero qty, invalid type combination, off-tick price (limit only), odd lot, duplicate ID, post-only cross.

## D-021 (2026-11-18): Modify semantics
**Choice:** `modify(id, new_price, new_qty)`, where `new_qty` is the new *remaining* quantity. Priority is kept only if the price is unchanged and the quantity does not increase (shrink in place). Otherwise the order is removed (event `OrderModified{kept_priority=false}` plus the old level's update) and re-entered as a GTC limit order, so it may trade immediately. Post-only orders cannot be modified into a cross. Invalid modifies emit `ModifyRejected` and change nothing.
**Reason:** Keeping priority on a shrink harms nobody behind the order; anything else is equivalent to a new order. "Remaining" is unambiguous for partially filled orders.

## D-022 (2026-11-18): Self-trade prevention
**Choice:** An `owner` field on every order (`kNoOwner` = 0 is exempt) and one book-wide `SelfTradePolicy`: CancelNewest (default), CancelOldest, CancelBoth, Decrement. STP actions are reported as `OrderCanceled(SelfTradePrevention)`; Decrement reports partial cancels via `remaining_qty > 0`. STP also applies when a modify re-enters an order.
**FOK interaction:** a FOK is killed if a same-owner order lies within the quantity it would need, so a FOK never partially executes and never triggers STP.
**Reason:** Covers the policies real venues offer. Exempting owner 0 keeps anonymous flow (and the Phase 1 tests) unaffected.

## D-023 (2026-11-18): Tick and lot size live in `BookConfig`
**Choice:** Prices remain integers in the system's smallest unit; `BookConfig::tick_size` and `lot_size` are per-instrument multiples checked with `%`. Invalid configs (tick or lot < 1) throw `std::invalid_argument` from the constructor, at startup and never on the hot path.
**Reason:** Separates "the unit prices are expressed in" from "which prices this instrument allows", as real venues do. `x % m == 0` is correct for negative `x` in C++.

## D-024 (2026-11-18): Coverage assertions in the model test
**Choice:** Each random workload counts the events and paths it produced (trades, STP actions, priority-keeping vs priority-losing modifies, each reject reason) and fails if a path it should cover never occurred.
**Reason:** A random test that never reaches a rule passes vacuously. The first run proved the point: priority-keeping modifies occurred 0 to 7 times per 20,000 operations because random prices rarely matched the order's own price. The generator now targets each order's last known price half of the time.

## D-025 (2026-11-18): Common book interface as a C++20 concept (static polymorphism)
**Options:** (1) An abstract base class `OrderBook` with virtual `submit`/`cancel`/`modify`. (2) A concept `OrderBook<Book>` that both books satisfy; generic code is a template.
**Choice:** (2), in `order_book.hpp`, checked with `static_assert` next to each book. The shared tests are GoogleTest typed tests over `BookTypes = {MapOrderBook, FastOrderBook}`.
**Reason:** Calls are resolved at compile time and can be inlined; a virtual call is an indirect branch per operation and blocks inlining. Nothing needs to pick a book at run time. The listener stays virtual for now (one indirect call per event); Phase 4 measures whether that matters.

## D-026 (2026-11-18): Price band and capacity are book rules, enforced by every book
**Choice:** `BookConfig` gains `min_price`/`max_price` (limit prices outside: `PriceOutOfRange`, checked after the tick check and before the lot check, for submit and modify) and `max_orders` (a remainder that would rest in a full book: `OrderCanceled(BookFull)` after any trading). Defaults are unbounded, so existing users of `MapOrderBook` see no change. All three books (naive, map, fast) implement both, so the differential tests cover them.
**Rejected:** limits known only to `FastOrderBook` (the books would then disagree on those inputs and the differential test could not cover the paths); rejecting new orders up front when the book is full (a full book could then not even accept orders that would trade and free space).
**Note:** an order that trades never ends up `BookFull`: having a remainder means every order it crossed was used up, which freed at least as many slots as it needs.

## D-027 (2026-11-18): `FastOrderBook` layout
**Choice:** per side, a flat array with one level per tick in the band plus an occupancy bitmap; an index-based pool of 32-byte order nodes with intrusive doubly linked FIFO lists per level; a fixed-capacity open-addressing hash map (`FlatIdMap`) from order ID to node. All sized in the constructor from `BookConfig`; the constructor throws for an unbounded or too-wide band (more than 2^22 levels), a band not on tick, or `max_orders` above 2^24.
**Rejected:** a tree of levels (O(log n), node per level); `std::list` per level (allocation per order, scattered memory); `std::unordered_map` (a node allocation per insert); a direct array indexed by order ID (client IDs are sparse and reused).
**Details:** 32-bit indexes instead of pointers (half the size, survive moves, checkable). LIFO free list (reuses cache-warm slots). Best price moves to the next occupied level 64 levels per step via `countr_zero`/`countl_zero`; a two-level bitmap would bound that further and is not needed yet. `FlatIdMap` uses Fibonacci hashing (replaced by a fold in D-039), at most 50% load, and backward-shift deletion (no tombstones).

## D-028 (2026-11-18): Proving "no allocation after startup"
**Choice:** a separate test executable replaces the global `operator new`/`delete` with counting versions, arms the counter around replaying every standard workload into a `FastOrderBook`, and requires zero. Positive controls: a test that the counter sees an allocation, and a test that `MapOrderBook` does allocate. Not built under TSan (Clang's static TSan runtime defines `operator new`, so it cannot be replaced; the test is single-threaded anyway).
**Reason:** A rule that is only reviewed, not tested, regresses silently.

## D-029 (2026-11-18): Differential testing in two layers
**Choice:** (1) The model test replays each workload into the naive oracle, `MapOrderBook` and `FastOrderBook`, comparing events and queries after every operation (20,000 operations per workload; each workload's band is its generated price range plus a 10-tick margin so the per-operation invariant check stays cheap). (2) A long test compares only `FastOrderBook` with `MapOrderBook` on streamed operations (default 100,000 per workload, `LITBOOK_DIFF_OPS` for more), checking invariants every 4,096 operations.
**Reason:** The naive oracle is O(n) per operation, so it cannot run millions of operations; the map book is trusted because it matches the oracle.

## D-030 (2026-11-19): Differential fuzzing with fixed-size records, replayed in every build
**Choice:** one libFuzzer target (`fuzz/book_fuzzer.cpp`) that decodes its input as a 6-byte config header (tick, lot, band, capacity, self-trade policy) followed by 5-byte operation records, runs them through the naive oracle, `MapOrderBook` and `FastOrderBook`, and aborts on any difference in events, best prices, depth, counts or invariants. Value ranges are deliberately narrow (IDs 1..32, at most 32 orders, prices up to 4 ticks outside the band). Built only by the Clang `fuzz` preset (ASan + UBSan, `-O1`). The same `run_one` function is linked into `litbook_fuzz_replay`, a ctest test that replays `fuzz/seeds/` and `fuzz/regressions/` in every preset. CI runs the fuzzer for 60 seconds per push.
**Rejected:** `FuzzedDataProvider` (reading a variable number of bytes per value means one mutated byte shifts every later value, so mutations rarely keep a useful prefix); fuzzing each book alone for crashes only (it would miss wrong-but-not-crashing results, the more likely bug); committing the generated corpus (large and churns; `fuzz/corpus/` stays local).
**Reason:** random tests explore what the generator was written to produce; a coverage-guided fuzzer keeps inputs that reach new code and so finds paths the generator's probabilities make rare.

## D-031 (2026-11-19): First benchmark method
**Choice:** Google Benchmark (`bench/book_bench.cpp`), every benchmark instantiated for both books with the same config. A counting listener (one virtual call per event, no storage). Microbenchmarks run against a 2,000-order background book and restore its state each iteration; stream benchmarks replay a pre-generated test-generator stream into a fresh book, built and destroyed while the timer is paused. Results are recorded with 5 repetitions (median and coefficient of variation), release preset, in `docs/BENCHMARKS.md`.
**Reason:** these are relative, regression-tracking numbers. Tail latency (percentiles per operation) and the real-data headline benchmark (a full Nasdaq TotalView-ITCH 5.0 day) come in Phase 4, which needs a latency recorder and an ITCH parser first.

## D-032 (2026-11-19): The latency timer reads the CPU counter directly
**Choice:** `TickClock` (`include/litbook/metrics/tick_clock.hpp`) reads `cntvct_el0` on arm64 (with an `isb` barrier) and `rdtsc` on x86-64, and falls back to `steady_clock` elsewhere. On arm64 the tick length comes from `cntfrq_el0`. On x86-64 it is measured against `steady_clock` for 20 ms on first use. Histograms record raw ticks, which are converted to nanoseconds only when printed.
**Measured on M1:** the counter runs at 24 MHz, so one tick is 41.67 ns. Two back-to-back reads differ by 0 ticks 64% of the time. Book operations take about 12 to 35 ns (BENCHMARKS, Phase 3b), so a single operation usually measures as 0 or 1 tick.
**Consequence:** on this machine, percentiles up to about p99 cannot tell the books apart. The tool marks every value of at most 2 ticks. The tail (p99.9 and above) is where the books differ, and it is well above the tick. An x86 Linux machine (where `rdtsc` ticks well under 1 ns) is the planned second benchmark machine for finer numbers.
**Rejected:** `steady_clock` everywhere (on arm64 Linux it reads the same counter through the vDSO, with more overhead and no better resolution); the ARM cycle counter `PMCCNTR_EL0` (not readable from user space unless the kernel enables it, which Docker Desktop's VM does not).

## D-033 (2026-11-19): A log-linear latency histogram written in-house
**Choice:** `LatencyHistogram` (`include/litbook/metrics/latency_histogram.hpp`) works like HdrHistogram. Values 0..63 are counted exactly, and every power-of-two range above that is split into 32 buckets, so the error is at most 1/32 (about 3%). It is a fixed array of about 1,200 counters, recording costs a few instructions and never allocates, and two histograms can be merged. Percentiles round up to the top of the bucket (never below the true value) and are capped at the exact max. `record_corrected` adds the samples that coordinated omission hides, the same way HdrHistogram does.
**Bug found while writing the exercise tests:** the percentile rank was computed as `ceil(p / 100 * count)` in floating point. With 100 samples, `7 / 100.0 * 100` is 7.000000000000001, so p7 returned the 8th value; with 999 fast samples and one slow one, p99.9 returned the slow one. The rank is now computed with integers (percent rounded to parts per billion). Regression test `DecimalPercentilesUseTheExactRank`.
**Rejected:** storing every sample and sorting (8 bytes per sample is too much for a full ITCH day of about 270 million messages, and it allocates); the HdrHistogram C library (a new dependency for about 150 lines of code).

## D-034 (2026-11-19): How per-operation latency is measured and reported
**Choice:** `litbook_latency` (`bench/latency_main.cpp`) generates each stream up front, runs one untimed warm-up pass and 5 timed passes per book and workload, each into a fresh book, and times every operation. Each printed percentile is the **median over the 5 passes**, and the max is the largest of any pass. It also prints the timer's own cost and the p99.9 spread over passes. Workloads come from `MarketGenerator` (`bench/market_workload.hpp`), which only sends valid orders, places them near the best price with geometrically decreasing distance, lets the price drift, and steers the cancel rate so the book settles at a target size (2,000 or 20,000 orders).
**Reason:** merged histograms were dominated by single passes the VM had interrupted (p99.99 moved by 10x between runs); the median of passes was stable across invocations. An earlier version of the generator only canceled the last 4,096 orders, so the book grew for the whole run, which is unlike a real book; the target size fixes that.
**Note:** these are service times (one call to the book, listener calls included), measured back to back with no target rate, so coordinated omission does not apply. End-to-end latency under an offered load comes with the gateway in Phase 7.

## D-035 (2026-11-19): Read ITCH as a stream, decode with a compile-time handler
**Choice:** `BinaryFileReader` splits Nasdaq's BinaryFILE format (2-byte big-endian length, then the payload, and a zero-length message at the end of the session) from any `FILE*`, through one buffer allocated in the constructor. Real days are replayed as `gunzip -c day.gz | tool`, so the 8.25 GB uncompressed file never touches the disk. `parse_message` decodes one payload into a struct and calls a handler that is a template parameter (checked by the `ItchHandler` concept). Every known type must have exactly the length in the specification, or it is reported as `BadLength` and not passed on. Types that do not change a book are skipped.
**Rejected:** decompressing in-process with zlib (a new dependency), decompressing to disk first (the development disk had about 16 GB free), virtual handler methods (one indirect call per message for no benefit), reading fields by casting the bytes to packed structs (alignment and endianness problems, undefined behavior).
**Checked against the real file:** 268,744,780 messages on 2019-12-30 with zero wrong lengths, zero references to unknown orders and zero duplicate references.

## D-036 (2026-11-19): A third book, SortedOrderBook, because real data breaks the ladder
**Context:** `FastOrderBook` (D-027) needs a price band with at most 2^22 ticks. On the real Nasdaq day, most stocks have market-maker stub quotes at $0.0001 and $199,999.99, so 6,932 of 8,892 stocks span more than 2^22 levels even at a 1-cent tick, and only 1,611 fit a 16,384-level ladder. Real orders still cluster near the market (98.97% of new orders land within $20.47 of the stock's last trade).
**Choice:** keep `FastOrderBook` unchanged and add `SortedOrderBook`, which has the same rules and the same event stream. Per side it keeps a sorted array of {price, level slot} from the worst price to the best, so the busy end is the back of the array. Finding a price checks the last 8 entries and then binary searches. Levels live in a pool, so orders point at a stable slot and shifting the array never breaks them. Orders, the FIFO lists and the ID index are the same as in `FastOrderBook`. Both arrays reserve `max_orders` entries, so nothing allocates after construction. It needs no band.
**Rejected:** replacing the ladder inside `FastOrderBook` (loses a design that is right for venues that enforce price collars, and the comparison); a dense window around the market plus an overflow structure (fastest lookups but the most complex, with re-centering and about 14 GB for all stocks unless sized per stock; a candidate for Phase 4c); replaying with `MapOrderBook` only.
**Testing:** joins every typed scenario, the four-way model test, the differential test, the fuzzer and the no-allocation test, plus its own tests. Of 5 planted bugs, 4 were caught and the fifth was an equivalent mutant (searching `[0, i - 1)` instead of `[0, i)` when entry `i - 1` is already known to be better).

## D-037 (2026-11-19): How a real ITCH day is replayed and measured
**Choice:** `ItchBookBuilder` turns ITCH into book calls: an add is a resting limit order, an execution or partial cancel is a modify to the smaller size (which keeps priority) or a cancel when nothing is left, a delete is a cancel, and a replace is a cancel followed by a new order on the same side. It keeps its own record per resting order in a fixed `IndexPool` plus `FlatIdMap`. Books are sized per stock from a first pass (`litbook_itch_stats` writes each stock's peak resting orders and price range), with 50% slack. `litbook_itch_replay` times only the calls into the book, one sample per ITCH message, and reports a health check: because Nasdaq already matched everything, a faithful replay has zero trades and zero rejects, and every book replayed all day ends empty.
**Halts:** the first verify run found 1,577 trades in 15 stocks, every one while the stock was halted, paused or in a quotation-only period. Nasdaq accepts orders then without matching and clears the crossed book with a reopening auction; our books always match and have no auctions. The builder therefore stops replaying a stock at its first trading action that is not 'T', for the rest of the day (24 of 8,892 stocks on 2019-12-30). Modeling auctions would be its own phase.
**Correctness on real data:** `verify` mode runs `MapOrderBook` and `SortedOrderBook` side by side; on 2019-12-30 their events matched for all 263,116,612 messages that reached a book, with zero trades, rejects, drops or invariant failures.
**Note:** this measures maintaining a book from a feed (adds, cancels, reductions), as CppTrader's published ITCH benchmark does, not matching. Matching cost is covered by the synthetic benchmarks (Phase 3b, 4a).

## D-038 (2026-11-19): Profiling with perf inside the Docker VM
**Choice:** the dev image installs `linux-perf` (Debian/Ubuntu's single perf binary, not tied to a kernel version; `linux-tools-generic` ships no `/usr/bin/perf` for the linuxkit kernel), and the container runs with `--cap-add PERFMON`. A `profile` preset builds Release with `-g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`, and `scripts/profile.sh` samples with `perf record -e cpu-clock --call-graph fp`.
**Why the workarounds:** Docker Desktop's VM exposes no hardware counters (no cycles, cache misses or branch misses), so only software events such as `cpu-clock` work. perf also cannot name functions on its own: the kernel reports the binary under the VM's internal path (`/run/host_virtiofs/...`) and a build ID with extra note-header bytes in front, so perf decides the file does not match. The script records raw addresses and resolves them with `addr2line`, which reads the DWARF information straight from the binary, then groups samples per function and caller.
**Rejected:** running perf on the macOS host (no perf on macOS; Instruments would profile a different OS and compiler), or a bare-metal Linux machine (not available; noted as future work for hardware counters).

## D-039 (2026-11-19): FlatIdMap folds the key instead of Fibonacci hashing
**Choice:** `home(key)` XORs the key's table-index-wide chunks together (`h = key; h ^= key >> b; h ^= key >> 2b; ...; h & mask`) instead of multiplying by 2^64/φ and keeping the top bits.
**Why:** the untimed profile of the full-day sorted replay put about 28% of samples in `FlatIdMap` operations (the book's cancel and validate lookups, and the builder's own table). Nasdaq hands out order reference numbers in increasing order, and most orders are canceled soon after they are added, so the IDs touched at any moment are close together. Fibonacci hashing scatters close IDs across the whole table (every lookup a likely cache miss); the fold keeps them in nearby slots that stay in cache. Every key bit still reaches the index, so keys that differ only in high bits do not all collide (unit tests check the longest probe for strides 2^10 to 2^32 and for sequential keys).
**Measured:** five A/B pairs on the same build, alternating the two binaries, full day, untimed, CPU time of the message loop. Fibonacci 63.7, 61.1, 78.2, 67.9, 64.6 s; fold 58.7, 57.7, 64.2, 60.8, 69.2 s. The fold won 4 of 5 pairs; medians 64.6 vs 60.8 s (about 6% of the loop, about 8% of the book's own share after the roughly 16.5 s null-book pipeline). Run-to-run noise on this VM is about as large as the gain (the same binary ranged from 57.7 to 75.4 s within one session), so this is a likely improvement, not a proven one; hardware counters on bare metal (cache misses per lookup) would settle it. Kept because it is no worse on any run set, has a clear mechanism, and passes the pile-up tests.
**Still the top hotspot:** a profile after the change (sampled timing, see D-040) puts about 44% of samples in `FlatIdMap` (find 16%, home 12%, slot reads 7%, erase 6%). Part of `home` is the fold loop itself, which cannot be unrolled because the table width is only known at run time. Follow-ups: a fold with a fixed number of steps, and a smaller per-stock table.
**Rejected:** keeping Fibonacci hashing (measurably slower); identity hashing (`key & mask`, which ignores high bits, so strided keys pile up); a stronger mixer such as murmur's finalizer (spreads keys like Fibonacci, which is what we want to avoid here). Not hardened against adversarial keys, which is fine for exchange-assigned IDs; revisit if client-chosen IDs ever reach this table.


## D-040 (2026-11-19): The replay times one message in 64, and reports the timer floor
**Problem:** `TickClock::now()` on ARM is `isb` followed by reading `cntvct_el0`. The barrier makes the CPU finish everything in flight first, so timing every message disturbed the very thing being measured. The profile of the every-message replay put about 54% of samples in `TickClock::now()`, and in back-to-back runs the message loop used 10 to 22 s more CPU timed than untimed (sorted 86.5 / 75.9 vs 64.7 / 65.4 s; map 132.6 / 135.5 vs 109.9 / 115.7 s), roughly 40 to 100 ns per message. The Phase 4b per-message numbers are therefore inflated by the measurement; those rows stay in BENCHMARKS.md with a note.
**Choice:** `litbook_itch_replay` times one message in N (default 64, `--sample-every N`; N = 1 is the Phase 4b method). It still gets about 4.1 million samples on the full day, enough for p99.99. Each sample still contains one timer read, so the tool also prints the timer floor (an empty call timed the same way; p50 0, p99 42 ns on M1). `--untimed` and a `null` book (the pipeline alone) give a second, timer-free view: an untimed real book minus the null book is the book's own CPU cost, an upper bound because cache sharing with the decoder is charged to the book.
**Measured:** with 1-in-64 sampling the profile shows 0.81% of samples in `TickClock::now()` (was about 54%).
**Rejected:** subtracting a fixed per-read overhead from every sample (the barrier's cost depends on what the book left in flight, so it is not a constant); timing batches of messages (gives an average, loses the tail, which is the point of percentiles); random sampling instead of every Nth (ITCH has no structure with period 64, so a fixed stride is simpler and reproducible).
**Caveat:** run-to-run CPU-time noise in this VM is large (the same binary ranged from 57.7 to 75.4 s in one session). Only compare CPU times from alternated A/B pairs.
## D-041 (2026-11-19): One book per stock, routed by a dense SymbolId
**Options:** (1) A hash map from ticker string to book, looked up on every call. (2) Dense 16-bit SymbolIds handed out at startup, with the books in an array and one flat hash table from ticker to SymbolId. (3) One book holding every stock.
**Choice:** (2). `Engine<Book>` is a template over the `OrderBook` concept. Each stock's book reports into a small forwarder that adds the SymbolId, and the forwarder replaces `Trade::match_id` with an engine-wide counter so match numbers are unique for the day across all stocks. Order IDs stay per book; making them unique is the order entry layer's job. Unknown SymbolIds return `EngineStatus::UnknownSymbol` and send no event, since no book is involved.
**Reason:** A ticker string hash and compare on every call is wasted work, and ITCH already uses a 16-bit locate code for the same idea, so the ticker is turned into an integer once at the edge. Option (3) would make every stock share one ladder and one capacity, and a bug in one stock could corrupt another. Each book is its own allocation (a `unique_ptr` to a slot holding the forwarder and the book) because the book keeps a reference to its forwarder, so the slot must never move.

## D-042 (2026-11-19): Wire codec with explicit big-endian fields at fixed offsets, modeled on OUCH 5.0
**Options:** (1) `#pragma pack` structs laid out like the wire, filled by `reinterpret_cast` or `memcpy` of the frame. (2) Plain C++ structs for decoded values, and a codec that reads and writes each field at a documented offset with byte-by-byte big-endian loads and stores. (3) A byte-exact OUCH 5.0 implementation including the TagValue appendage.
**Choice:** (2). Messages keep OUCH 5.0's type letters, field encodings (big-endian integers, prices with 4 implied decimals up to $199,999.9900, space-padded symbols, UserRefNum, cancel to an intended size) and session rules, but have fixed lengths and only the fields litbook supports. Every difference is listed in `docs/PROTOCOL.md`. The decoder returns a `DecodeStatus` and never throws or allocates. Inbound enumerated fields are checked strictly, outbound reason codes and liquidity flags are open sets, and every frame that decodes re-encodes to the same bytes. Malformed frames (bad bytes) are separate from rejected orders (well formed but breaking a trading rule).
**Reason:** Packed structs depend on the host's byte order, invite misaligned and type-punned access (undefined behavior that only shows up on some CPUs or compilers), and tie the in-memory type to the wire. The shift loop compiles to one load plus a byte reverse, so explicit fields cost nothing. Option (3) would mean parsing variable-length option lists for features the books do not have; it can be added later without changing the engine.
**Checked by:** golden frames written out field by field in the tests, every truncation and every unknown type byte, bad values in every enumerated field, and a `protocol_fuzzer` libFuzzer target (130 million inputs in a 2 minute local run, no failures; a planted bug that accepted a lowercase `y` flag was caught within 60 s).

## D-043 (2026-11-19): The order entry layer owns the engine and keeps one record per live order
**Options:** (1) Give the client's UserRefNum straight to the book as the order ID. (2) The exchange assigns its own day-unique Order Reference Number, uses it as the book order ID, and keeps a record per live order reachable from both (account, UserRefNum) and the reference number.
**Choice:** (2). `OrderEntry<Book>` owns the `Engine` and is its listener. Records live in an `IndexPool`, and two `FlatIdMap`s (key `owner << 32 | UserRefNum`, and reference number) point at them, so nothing allocates after startup. A record stores the shares executed so far, which OUCH's "intended size" cancels and replaces need. It is released as soon as the order has no open quantity. Because a listener may not call back into the engine, a cancel or replace sets a small `pending_` state before calling `modify`, the event handlers read it, and follow-up work (canceling the original after a failed replace) runs after the call returns.
**Rejected:** option (1), because UserRefNums are only unique per account, so two accounts would collide in one book, and the exchange could not give out day-unique reference numbers. Tracking executed shares in the book, because it is session state the book does not need.
**Details:** an invalid replace sends Rejected for the new UserRefNum and then cancels the original, as OUCH does with an invalid replace. A reducing cancel whose new size is not a multiple of the lot size is ignored and counted, because the book refuses it and the order does not change.

## D-044 (2026-11-19): The gateway speaks SoupBinTCP 3.0, and its rules live apart from the sockets
**Options:** (1) A bare 2-byte length prefix in front of each OUCH message. (2) SoupBinTCP 3.0, Nasdaq's session protocol for OUCH, with login, heartbeats, sequenced and unsequenced envelopes, logout and end of session. For where the rules live: (a) inside the epoll loop, or (b) in a `SessionManager` that never touches a socket and is driven by calls such as "this connection opened", "these bytes arrived", "the clock is now t", "this much output was written" and "the connection is gone".
**Choice:** (2) and (b). `include/litbook/gateway/soup.hpp` parses and builds packets without copying or allocating. `Session` holds one connection's protocol state and two fixed `ByteBuffer`s. `SessionManager<Book>` owns the `OrderEntry`, the accounts and a pool of sessions made at startup, and it is the order entry layer's `OutboundSink`. The Phase 6b epoll code only moves bytes between sockets and these calls.
**Reason:** SoupBinTCP is how OUCH actually travels, it is only ten small packet types, and it gives accounts, logins and heartbeats for free. Keeping the rules away from system calls means every rule is tested with plain byte arrays and a made up clock, including every two-way split of a byte stream and one byte at a time, and the whole layer can be fuzzed (`session_fuzzer`).
**Limits chosen where the specification is silent:** client packets longer than 47 bytes after the length field close the connection (checked as soon as the two length bytes arrive), one connection per account, and a requested sequence number ahead of the next one is accepted with the real next number. Replaying old sequence numbers is refused with Login Rejected `S` until the Phase 9 journal can regenerate them.
**Rejected:** a ring buffer per connection (it would split packets across the end of its memory; with packets of at most 50 bytes, moving the leftover bytes to the front is cheap and keeps every packet contiguous).
**Checked by:** a mutation check of 9 planted bugs in the session layer, buffers, codec and order entry changes (all caught, one as a hang by the test timeout), and a fuzzer run that caught a planted sequence-number bug within 60 seconds when started from the seeds.

## D-045 (2026-11-19): Backpressure first, disconnect only when the output buffer is full
**Problem:** the first version processed every byte it was handed. A client that sent 200 orders in one read produced 9,600 bytes of replies, overflowed its 4 KiB test output buffer, and was disconnected as a slow consumer although it was reading fine.
**Choice:** a connection's input is only processed while fewer than `output_high_water` bytes (32 KiB by default, out of 64 KiB) wait in its output. Past that, new bytes stay in the input buffer, `receive()` takes no more, and `receive_room()` is 0, so the network code stops reading that socket and TCP flow control slows the client down. `sent()` resumes processing. The space above the high water mark is for replies that cannot be held back (executions caused by other accounts, heartbeats). Only when those fill the whole buffer is the client disconnected with `OutputFull`.
**Rejected:** an output queue that grows without limit (a client that stops reading would use up the exchange's memory), and disconnecting at the first full buffer (punishes a client for sending a burst).
**Consequence:** a paused client that keeps sending but never reads stops being read, so it eventually hits the idle timeout. That is the behavior wanted for a client that does not read its replies.

## D-046 (2026-11-19): Risk checks and cancel on disconnect live in the order entry layer
**Choice:** `RiskLimits` per account (largest order quantity, most live orders) are checked in `OrderEntry` before an order reaches its book, and the quantity limit also applies to the new size in a Replace. An order over the size limit gets OUCH's own code 0x0031 (Risk: Max Quantity Exceeded). OUCH has no code for an open order limit, so that one is the litbook extension 0x0103. `OrderEntry::cancel_all(owner, now)` cancels every live order of one account, reported with cancel reason `Z` (system), because OUCH's `U` means "sent in response to a Cancel Request". To find one account's orders without scanning everyone's, each record is on a doubly linked list per account (two 32-bit indexes in the record), so `cancel_all` costs time proportional to that account's live orders and allocates nothing.
**Rejected:** checking risk in the gateway (it would have to rebuild the live order count per account by watching the replies, a second copy of state `OrderEntry` already has), and scanning the whole record pool on disconnect (time proportional to every live order on the exchange).

## D-047 (2026-11-19): A single-threaded, level-triggered epoll gateway
**Options:** (1) One blocking thread per connection. (2) One thread with epoll, edge triggered. (3) One thread with epoll, level triggered. (4) io_uring.
**Choice:** (3), `EpollGateway<Book>` in `include/litbook/gateway/epoll_gateway.hpp`. Each `poll_once()` accepts waiting connections (`accept4` with `SOCK_NONBLOCK`, `TCP_NODELAY` on), reads at most `receive_room()` bytes from each readable socket, runs `tick()` every 100 ms, then writes out every connection's output with `send(MSG_NOSIGNAL)` and closes the ones the session layer gave up on. A socket's `EPOLLIN` interest is off while its input is paused, and `EPOLLOUT` is on only while output is left over. `stop()` writes to an eventfd, so it is safe from another thread or a signal handler. The clock is a function pointer, so tests move time by hand.
**Reason:** a thread per connection needs a lock around the engine, and the lock then decides the order of orders. Edge triggering wakes the loop once per change, so every handler must read until `EAGAIN` or lose the wakeup, and pausing a client would need extra bookkeeping. Level triggering keeps telling the loop a socket is ready until it acts, and turning `EPOLLIN` off is the whole backpressure mechanism. io_uring support inside the Docker Desktop VM is uncertain and it is much harder to reason about. Writing every connection's output after each round is needed because one client's order can produce replies for many others. It costs O(connections) per round, which is fine for the 64 connection default; a list of connections with new output would bound it if that ever measures as a cost.
**Rejected details:** waiting for `EPOLLOUT` before every write (one extra system call and wakeup per reply; the gateway writes straight away and only falls back to `EPOLLOUT` when the kernel buffer is full), and letting a closing session linger until its output drains (a client that stops reading would keep the slot; one write attempt is made, then the socket closes).
**Checked by:** 16 integration tests over loopback sockets (many clients, one byte at a time, malformed input, rejected login delivered before the close, cancel on disconnect, connection limit, heartbeats and idle timeout on a fake clock, end of session, a burst that pauses a client without dropping it, a slow reader dropped with small kernel buffers, a threaded run for TSan), a no-allocation test over real sockets (20,000 frames), end-to-end tests of the `litbook_gateway` program, and 4 planted bugs (all caught, one as a hang).

