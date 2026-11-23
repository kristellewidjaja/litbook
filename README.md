# litbook

A low-latency electronic exchange simulator in modern C++ (C++20).

A *lit* market is a venue with a publicly visible order book, as opposed to a dark pool, where resting orders are hidden. litbook is a lit venue. It matches orders with price-time priority and publishes its book to anyone listening on the market data feed.

## How it works

An exchange takes orders from clients over the network, matches buyers with sellers, tells every client what happened to its own orders, and tells the whole market what the book now looks like. litbook does each of these on its own thread, and the threads hand work to each other through lock-free single-producer, single-consumer queues.

```
 clients (TCP)                                                  market (UDP multicast)
      |                                                                  ^
      v                                                                  |
+-----------+   requests   +-----------+   requests   +-----------+  events  +-----------+
|  gateway  | -----------> |  journal  | -----------> | matching  | -------> | publisher |
|  thread   | <----------- |  thread   |              |  thread   |          |  thread   |
+-----------+    replies   +-----------+              +-----------+          +-----------+
      ^                          |                          |
      |                          v                          |
      |                  journal segments  ---> snapshot thread ---> snapshots
      +----------------------- replies ---------------------+
```

1. **The gateway thread** runs one `epoll` loop over every client connection. Clients log in and send orders with a binary protocol modeled on Nasdaq's: SoupBinTCP 3.0 for sessions (logins, heartbeats, sequence numbers) and OUCH 5.0 for orders (big-endian, fixed-length messages). The gateway checks each session and hands every request to the next thread. It never matches anything itself, so a slow book never stops it from reading sockets. A client that reads too slowly gets backpressure instead of unbounded buffering.
2. **The journal thread** writes every request to a write-ahead journal before the matching thread may see it, so nothing leaves the exchange before its cause is on disk. It takes every request that is waiting and writes them with one system call (group commit). Each record carries a sequence number and a CRC32C, computed with the CPU's CRC instruction where there is one.
3. **The matching thread** owns every order book. It decodes each request, applies the session rules and pre-trade risk limits, and runs the order through its stock's book. It sends replies (accepted, executed, canceled, rejected) back to the gateway, and market events (price level changes and trades) to the publisher. It is deterministic. The same requests in the same order always produce the same replies, which is what makes the journal enough to rebuild the exchange.
4. **The publisher thread** turns market events into a market by price feed, packed into MoldUDP64 packets and sent over UDP multicast. A second multicast stream repeats a snapshot of every book, so a receiver that loses packets or joins late can rebuild correct books and then follow the live stream again.
5. **The snapshot thread** keeps its own copy of the exchange. Each time the journal closes a segment file, this thread replays that file into its copy and writes a snapshot of the whole state, so the matching thread never pauses for a snapshot. Old segments are deleted once a snapshot covers them.

Without a journal or a feed, `litbook_gateway` can also run the gateway and matching on one thread, which is the baseline the threaded pipeline is measured against.

After a crash, the exchange starts from the newest good snapshot and replays only the journal records after it. Every account's sequence numbers continue where they stopped, and the feed starts a new session so receivers rebuild their books.

## The order books

Every stock has its own book, and there are three implementations with identical behavior.

- **MapOrderBook** is the reference. It is built from standard containers and written to be simple enough to trust.
- **FastOrderBook** is a flat price ladder with pooled orders and an open-addressing order index. It never allocates after startup, but it needs a bounded price range.
- **SortedOrderBook** keeps sorted price levels instead of a ladder. Real Nasdaq data contains quotes from $0.0001 to $199,999.99 in the same stock, which no ladder can hold, so this book handles any price without allocating.

All three support limit, market, immediate or cancel, fill or kill and post-only orders, modifications with the usual queue priority rules, self-trade prevention, tick and lot sizes, price bands and capacity limits.

## How correctness is checked

- **Differential tests and fuzzing.** The three books are run on the same random streams and must produce exactly the same events. Six libFuzzer targets cover the books, the OUCH decoder, the session layer, the feed handler, the journal reader and the snapshot decoder.
- **Unit tests** for every component, run under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer with both GCC and Clang, also pinned to two CPUs to match CI.
- **No-allocation tests** replace the global `operator new` and prove that the books, the pipeline and the journal allocate nothing after startup.
- **Crash tests** kill the running exchange with SIGKILL at random points, also while segments rotate and snapshots are written. After every kill they check that the journal explains every message any client saw, and that every snapshot replays to the same state as the full journal.
- **Real market data.** A full Nasdaq TotalView-ITCH 5.0 trading day (268.7 million messages, 8,892 stocks) replays through the books with an exact health check.
- **Mutation checks.** Bugs are planted on purpose to make sure the tests catch them.

## Some measured numbers

Measured on an Apple M3 Pro, inside a Linux virtual machine under Docker, so treat them as one machine's numbers. Every row, with its date, commit, compiler flags and conditions, is in [BENCHMARKS](docs/BENCHMARKS.md).

| What | Result |
|---|---|
| FastOrderBook, rest an order and cancel it | 25.2 ns per operation |
| A full ITCH trading day through MapOrderBook | 347.2 ns per book message |
| One client message through the order entry layer, 100,000 live orders | 184.7 ns |
| TCP round trip through the single-threaded gateway, spin mode, p50 | 9.46 us |
| TCP round trip through the threaded pipeline with the journal, spin mode, p50 | 13.3 us |
| From an order on TCP to its price level on the multicast feed, spin mode, p50 | 3.7 us |
| Recovery from a journal of 4 million requests, without and with snapshots | 551 ms and 18.6 ms |
| Load test, 8 client sessions, highest rate held with p99 under 1 ms: single thread, pipeline with feed, pipeline with journal | 3.2, 4.8 and 2.4 million requests/s |
| Load test at 400,000 requests/s, p50 / p99: single thread, pipeline with feed, pipeline with journal | 7.4 / 20.5, 42.0 / 81.9 and 55.3 / 112.6 us |

## Tools

| Program | What it does |
|---|---|
| `litbook_gateway` | Runs the exchange, with optional journal, snapshots and market data feed |
| `litbook_feed_listener` | Joins the multicast feed and keeps books from it |
| `litbook_journal` | Verifies, dumps or replays a journal, and writes a snapshot offline |
| `litbook_loadgen` | Logs in many client sessions to a running gateway, sends orders on a fixed schedule and reports latency percentiles per request type |
| `litbook_itch_replay`, `litbook_itch_stats` | Replay a Nasdaq ITCH 5.0 file through a book, or summarize one |
| `litbook_bench` | Google Benchmark microbenchmarks of every component |
| `litbook_latency`, `litbook_gateway_latency`, `litbook_queue_latency`, `litbook_feed_latency`, `litbook_feed_recovery`, `litbook_journal_recovery` | Latency percentiles and recovery times |

## Still to come

Python bindings through pybind11 with a market maker, a noise trader and a live book viewer, and a design document that follows one order from socket to trade print.

## Building

Development targets Linux. On macOS or Windows, use the Docker dev container (Ubuntu 26.04, GCC 15, Clang 21):
```sh
./scripts/dev.sh                      # shell inside the container, repo mounted at /work
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```
Presets are `debug`, `release`, `asan` (AddressSanitizer and UBSan) and `tsan` (ThreadSanitizer), each also as `-clang`. Run benchmarks from the `release` preset only.

## Docs

- [Design decisions](docs/DECISIONS.md), every choice with the alternative that was rejected
- [Benchmarks](docs/BENCHMARKS.md), where every number comes from a recorded run
- [Order entry protocol](docs/PROTOCOL.md), the byte layouts of the OUCH and SoupBinTCP messages
- [Market data feed](docs/FEED.md), the feed messages, packets and recovery rules
- [Journal](docs/JOURNAL.md), the journal and snapshot formats and how recovery works
