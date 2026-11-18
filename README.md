# litbook

A low-latency electronic exchange simulator in modern C++ (C++20).

A *lit* market is a venue with a publicly visible order book, as opposed to a dark pool, where resting orders are hidden. litbook is a lit venue: it matches orders with price-time priority and publishes its book to anyone listening on the market data feed.

> **Status:** early development (Phase 0: scaffold). See [docs/ROADMAP.md](docs/ROADMAP.md) for the full plan and [docs/PROGRESS.md](docs/PROGRESS.md) for the current state.

## Planned components
- **Matching engine**: a simple reference order book plus a fast, allocation-free book, verified against each other by differential testing and fuzzing
- **Order entry gateway**: a non-blocking epoll TCP server speaking a fixed-width binary protocol modeled on NASDAQ OUCH
- **Market data feed**: incremental UDP multicast with sequence numbers, plus a snapshot channel for gap recovery
- **Persistence**: a write-ahead journal with deterministic replay and crash recovery
- **Tooling**: a NASDAQ TotalView-ITCH 5.0 replayer, a latency histogram, a load generator, and Python trading bots via pybind11

## Building
Build instructions will be added once the scaffold lands. Development targets Linux, with a Docker dev container for macOS hosts.

## Docs
- [Roadmap](docs/ROADMAP.md)
- [Design decisions](docs/DECISIONS.md)
- [Benchmarks](docs/BENCHMARKS.md): every number comes from a recorded run
