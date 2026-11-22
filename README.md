# litbook

A low-latency electronic exchange simulator in modern C++ (C++20).

A *lit* market is a venue with a publicly visible order book, as opposed to a dark pool, where resting orders are hidden. litbook is a lit venue: it matches orders with price-time priority and publishes its book to anyone listening on the market data feed.

> **Status:** early development. The reference order book is done: price-time priority matching, market / IOC / FOK / post-only orders, modify with queue-priority rules, self-trade prevention, tick/lot validation, price bands and capacity limits. A second, fast book (flat price ladder, pooled orders, open-addressing ID index) produces the identical event stream, checked by differential tests, and a test proves it makes no heap allocations after startup. A coverage-guided fuzzer compares all three books on every push, and first benchmarks of both books are logged in [BENCHMARKS](docs/BENCHMARKS.md). Per-operation latency percentiles are measured with an in-house HdrHistogram-style histogram. A full Nasdaq TotalView-ITCH 5.0 trading day (268.7 million messages, 8,892 stocks) replays through every book with an exact health check; real data broke the price-ladder book (market-maker stub quotes span $0.0001 to $199,999.99), so a third, sorted-level book was added that holds it without allocating. Profiling with perf showed the timer itself was the biggest cost of the measurement, so the replay now samples one message in 64 and reports the timer's own floor. On top of the books sits a multi-symbol engine and an order entry protocol modeled on Nasdaq OUCH 5.0 (big-endian fixed-length messages, byte layouts in [PROTOCOL](docs/PROTOCOL.md)), with malformed-input tests and a second fuzzer for the decoder. Clients reach it over TCP through a single-threaded epoll gateway speaking SoupBinTCP 3.0 (logins, heartbeats, sequence numbers), with pre-trade risk limits, cancel on disconnect, backpressure for clients that fall behind, a third fuzzer for the session layer, and tests over real sockets. The exchange can also run as a three-thread pipeline (gateway, matching and publisher threads joined by lock-free single-producer single-consumer queues), compared against a mutex queue. The publisher thread sends a market by price data feed in MoldUDP64 packets over UDP multicast, with a snapshot stream so receivers that lose packets or join late get back to correct books (byte layouts in [FEED](docs/FEED.md)), a fourth fuzzer for the feed handler, and tests where the kernel really drops packets. Every request can be written to a write-ahead journal (little-endian records with a CRC32C each) by a fourth thread before the matching thread sees it, so nothing leaves the exchange before its cause is on disk, and the exchange rebuilds itself from the journal at start, with every account's sequence numbers, after a crash. A test kills it with SIGKILL 20 times at random points and checks that the journal explains every reply any client saw ([JOURNAL](docs/JOURNAL.md)). Results with machine details are in [BENCHMARKS](docs/BENCHMARKS.md).

## Planned components
- **Matching engine**: a simple reference order book plus a fast, allocation-free book, verified against each other by differential testing and fuzzing
- **Order entry gateway**: a non-blocking epoll TCP server speaking a fixed-width binary protocol modeled on NASDAQ OUCH
- **Market data feed**: incremental UDP multicast with sequence numbers, plus a snapshot channel for gap recovery
- **Persistence**: a write-ahead journal with deterministic replay and crash recovery
- **Tooling**: a NASDAQ TotalView-ITCH 5.0 replayer, a latency histogram, a load generator, and Python trading bots via pybind11

## Building
Development targets Linux. On macOS or Windows, use the Docker dev container (Ubuntu 26.04, GCC 15, Clang 21):
```sh
./scripts/dev.sh                      # shell inside the container, repo mounted at /work
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```
Presets: `debug`, `release`, `asan` (AddressSanitizer + UBSan), `tsan` (ThreadSanitizer), each also as `-clang`.

## Docs
- [Design decisions](docs/DECISIONS.md)
- [Benchmarks](docs/BENCHMARKS.md): every number comes from a recorded run
