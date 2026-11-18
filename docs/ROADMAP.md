# litbook Roadmap

Rules: one phase at a time. A phase is done only when all of its boxes are checked, all tests pass (including the sanitizer presets), and its study guide is written.
Every phase has a **student function**: one self-contained function written by hand from a given signature, description and failing tests, then reviewed.
Every phase has a **unit tests** item: each new component ships with GoogleTest unit tests covering normal cases, edge cases and error paths.
If a phase is split across sessions, record the split here as sub-parts (e.g. 4a / 4b).

---

## Phase 0: Scaffold
- [x] Project docs: docs/ROADMAP.md, PROGRESS.md, DECISIONS.md, BENCHMARKS.md, docs/learn/
- [ ] Dev container (Ubuntu linux/arm64, recent GCC + Clang, CMake, Ninja, clang-format, clang-tidy, gdb, perf) + `scripts/dev.sh`
- [ ] CMake project, C++20, `-Wall -Wextra -Wpedantic -Werror`
- [ ] CMakePresets.json: debug, release, asan (ASan+UBSan), tsan
- [ ] GoogleTest + Google Benchmark via FetchContent (pinned versions)
- [ ] `.clang-format` and `.clang-tidy`
- [ ] GitHub Actions: build + test (GCC and Clang) + ASan/UBSan + TSan on every push
- [ ] Hello-world test passes in every preset; hello-world benchmark runs
- [ ] Student function (Phase 0): _small warm-up, chosen at phase start_
- [ ] Study guide `docs/learn/phase-0.md` (toolchain, CMake, sanitizers, why Linux) + 8 questions

## Phase 1: Baseline order book (correctness reference)
- [ ] Core types: `Price` (int64 ticks), `Qty` (unsigned), `OrderId`, `Side`, events
- [ ] Listener interface for trade events and book update events
- [ ] `MapOrderBook`: `std::map` price levels, `std::list` orders per level, `std::unordered_map` ID → location
- [ ] Limit orders with price-then-time-priority matching (partial fills, sweeps across multiple levels)
- [ ] Cancel (by ID; unknown-ID handling)
- [ ] Best bid/ask and depth queries
- [ ] Heavy unit tests: crossing, partial fills, multi-level sweeps, FIFO within a level, cancel edge cases, empty book
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 1)
- [ ] Study guide `docs/learn/phase-1.md` + 8 questions

## Phase 2: Order types and rules
- [ ] Market orders
- [ ] Immediate-or-cancel (IOC)
- [ ] Fill-or-kill (FOK)
- [ ] Post-only (reject if it would cross)
- [ ] Modify: quantity decrease keeps priority; price change or quantity increase loses priority
- [ ] Self-trade prevention with a configurable policy (cancel newest / cancel oldest / cancel both / decrement)
- [ ] Tick size and lot size validation with reject reasons
- [ ] Tests for every order type, rule and reject reason
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 2)
- [ ] Study guide `docs/learn/phase-2.md` + 8 questions

## Phase 3: Fast order book
- [ ] Common `OrderBook` interface shared by both books (choose static vs dynamic polymorphism and log it in DECISIONS.md)
- [ ] Flat-array price ladder indexed by tick
- [ ] Intrusive doubly linked list per price level
- [ ] Preallocated object pool for orders
- [ ] Flat open-addressing hash map or direct array for ID lookup
- [ ] Incremental best bid/ask tracking
- [ ] No hot-path heap allocation (verified by a test, e.g. a counting allocator hook)
- [ ] Differential test: millions of random operations through both books, asserting identical event streams
- [ ] libFuzzer target running the same differential check (Clang)
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 3)
- [ ] Study guide `docs/learn/phase-3.md` + 8 questions

## Phase 4: Measurement (likely split 4a / 4b / 4c)
- [ ] 4a: Latency recorder (cycle counter or steady_clock; arm64 note in DECISIONS) with a histogram reporting p50/p90/p99/p99.9/max
- [ ] 4a: Throughput and latency benchmarks of both books under several workloads (balanced, cancel-heavy, deep-book, ...)
- [ ] 4b: NASDAQ TotalView-ITCH 5.0 parser (verified against the official spec)
- [ ] 4b: Replay a real ITCH sample file through the fast book and rebuild one stock's book
- [ ] 4c: Profile with perf, find the top hotspot, fix it, and log before/after numbers in BENCHMARKS.md
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 4)
- [ ] Study guide `docs/learn/phase-4.md` + 8 questions

## Phase 5: Multi-symbol engine and wire protocol
- [ ] Engine owning many books keyed by symbol ID
- [ ] Fixed-width binary order-entry protocol modeled on NASDAQ OUCH: packed structs, explicit endianness
- [ ] Inbound messages: new, cancel, modify
- [ ] Outbound messages: accepted, rejected, executed, canceled
- [ ] Encoder, decoder and round-trip tests (including malformed input)
- [ ] `docs/PROTOCOL.md`
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 5)
- [ ] Study guide `docs/learn/phase-5.md` + 8 questions

## Phase 6: TCP gateway
- [ ] Non-blocking TCP server on epoll with many client connections
- [ ] Per-connection read/write buffers that handle partial reads and writes
- [ ] Message framing
- [ ] Session IDs
- [ ] Input validation
- [ ] Risk checks: max order size, max open orders per session
- [ ] Disconnect handling with optional cancel-on-disconnect
- [ ] Integration tests with real sockets
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 6)
- [ ] Study guide `docs/learn/phase-6.md` + 8 questions

## Phase 7: Threading
- [ ] Lock-free SPSC ring buffer with cache-line padding (no false sharing)
- [ ] SPSC tests under TSan
- [ ] Split into a gateway thread, a single matching thread and a publisher thread, connected by SPSC queues
- [ ] Optional thread pinning
- [ ] End-to-end latency (packet in → ack out) measurement
- [ ] Comparison against a mutex-based queue, logged in BENCHMARKS.md
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 7)
- [ ] Study guide `docs/learn/phase-7.md` + 8 questions

## Phase 8: Market data feed
- [ ] Incremental binary feed over UDP multicast with sequence numbers
- [ ] Snapshot channel for late joiners
- [ ] Feed handler client: gap detection, snapshot recovery, local copy of the book
- [ ] Tests that drop packets on purpose
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 8)
- [ ] Study guide `docs/learn/phase-8.md` + 8 questions

## Phase 9: Persistence and recovery
- [ ] Write-ahead journal of every inbound message, with sequence numbers and checksums
- [ ] Fully deterministic engine (replaying the journal yields identical state)
- [ ] Periodic snapshots + journal truncation/replay from snapshot
- [ ] Crash test: kill at random points, recover, and verify the book matches
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 9)
- [ ] Study guide `docs/learn/phase-9.md` + 8 questions

## Phase 10: Clients and simulation
- [ ] C++ load generator: many sessions, latency percentiles
- [ ] pybind11 client library
- [ ] Python market maker bot
- [ ] Python random noise trader
- [ ] Python live terminal book view from the market data feed
- [ ] Run everything together as a small simulated market
- [ ] Unit tests for every new component in this phase
- [ ] Student function (Phase 10)
- [ ] Study guide `docs/learn/phase-10.md` + 8 questions

## Phase 11: Polish
- [ ] README: architecture diagram, quick start, headline benchmark numbers with machine specs, "what I learned"
- [ ] `docs/DESIGN.md`: life of an order from socket to trade print
- [ ] Three resume bullets, using real measured numbers only
- [ ] Study guide `docs/learn/phase-11.md` (whole-system review) + 8 questions
