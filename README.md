# litbook

A low-latency electronic exchange simulator in modern C++ (C++20).

A *lit* market is a venue with a publicly visible order book, as opposed to a dark pool, where resting orders are hidden. litbook is a lit venue: it matches orders with price-time priority and publishes its book to anyone listening on the market data feed.

> **Status:** early development. The reference order book (price-time priority matching, cancels, full event stream) is done; order types and a fast book are next.

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
