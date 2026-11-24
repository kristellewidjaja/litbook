# litbook

A stock exchange simulator written in C++20.

Clients connect over TCP and send buy and sell orders. litbook matches them by price and then by arrival time, tells each client what happened to its orders, and broadcasts the order book to everyone over UDP multicast. Every order is written to a journal first, so the exchange can rebuild itself after a crash. It speaks the same kinds of binary protocols as Nasdaq, and it can replay a real Nasdaq trading day.

**Highlights** (measured on one laptop, details below)
- Replays a full Nasdaq trading day, 268.7 million messages, through its order books with an exact health check.
- 13.3 us median round trip for an order over TCP, with the journal on.
- 4.8 million requests per second with a 99th percentile under 1 ms.
- Recovers from a crash in 18.6 ms using snapshots, instead of 551 ms replaying a journal of 4 million requests.

## Quick start

litbook runs on Linux. On macOS or Windows, the dev container gives you the same setup and only needs Docker.
```sh
git clone https://github.com/kristellewidjaja/litbook.git
cd litbook
./scripts/dev.sh      # builds the dev image the first time, then opens a shell in it
```
Inside that shell, build everything and run the tests.
```sh
cmake --preset release && cmake --build --preset release
ctest --preset release
```
Then watch a simulated market. One market maker and four random traders trade for 20 seconds while the live book updates on screen. At the end it prints each bot's profit and checks that every trade adds up.
```sh
export PYTHONPATH=build/release/python:python
python3 -m litbook_bots.simulate --seconds 20 --makers 1 --noise 4 --seed 1 \
    --gateway build/release/tools/litbook_gateway --view
```
Or make one trade by hand. ALICE offers 100 shares at $100.00, and then BOB buys them. This is the trade that [DESIGN](docs/DESIGN.md) follows through the code.
```sh
build/release/tools/litbook_gateway --account ALICE:pw --account BOB:pw --feed lo &
python3 -c 'import litbook; c = litbook.OrderClient("127.0.0.1", 15000, "ALICE", "pw"); print(c.enter("AAPL", litbook.Side.Sell, 100, 1_000_000))'
python3 -m litbook_bots.book_view --once
python3 -c 'import litbook; c = litbook.OrderClient("127.0.0.1", 15000, "BOB", "pw"); print(c.enter("AAPL", litbook.Side.Buy, 100, 1_000_000)); print(c.events(timeout_ms=100))'
python3 -m litbook_bots.book_view --once
kill %1
```
The first book shows ALICE's offer, BOB gets `Accepted` and then `Executed`, and the second book is empty. Prices are whole numbers with 4 implied decimals, so 1,000,000 means $100.00.

## How it works

The exchange runs on five threads. They pass work to each other through lock-free queues, so no thread ever waits on a lock.

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

1. **Gateway.** Accepts client connections with `epoll`, checks logins and messages, and passes orders on. Clients use binary protocols modeled on Nasdaq's (SoupBinTCP for sessions, OUCH for orders).
2. **Journal.** Writes every order to disk before anything acts on it, many orders per write.
3. **Matching.** Runs risk checks and matches each order in its stock's book. Given the same orders in the same order, it always does exactly the same thing, which is why replaying the journal rebuilds the exchange.
4. **Publisher.** Broadcasts price level changes and trades over UDP multicast, plus a steady stream of snapshots so a listener that missed packets can catch up.
5. **Snapshot.** Saves the whole exchange state in the background, so recovery after a crash only replays the last part of the journal.

[DESIGN](docs/DESIGN.md) follows one order through every step, with the file and function that handles it.

## Three order books

Each stock has its own book, and there are three versions that must behave identically.
- **MapOrderBook** uses standard containers. It is the simple reference the others are checked against.
- **FastOrderBook** keeps one slot per possible price. It is fast and never allocates memory after startup, but it needs a narrow price range.
- **SortedOrderBook** keeps only the prices that have orders. Real Nasdaq data has quotes from $0.0001 to $199,999.99 in one stock, so this is the book that can handle a real trading day.

## How it is tested

- Unit tests for every component (1,529 of them), also under the address, undefined behavior and thread sanitizers, with GCC and Clang.
- The three books run on the same random orders and must produce exactly the same results.
- Six fuzzers feed random bytes to everything that reads input from outside.
- Crash tests kill the exchange at random moments and check that nothing a client saw was lost.
- A full real Nasdaq trading day replays with zero unexpected trades or rejects.

## Measured numbers

All numbers come from one laptop (Apple M3 Pro) running Linux in a virtual machine, so expect some noise. "p50" is the median and "p99" is the 99th percentile. Round trips are measured on the same machine, with the exchange's threads spinning instead of sleeping. Every number, with its date, commit and settings, is in [BENCHMARKS](docs/BENCHMARKS.md).

| What | Result |
|---|---|
| Rest an order and cancel it, FastOrderBook | 25.2 ns per operation |
| A full Nasdaq trading day through MapOrderBook | 347.2 ns per message |
| One client message with 100,000 orders live | 184.7 ns |
| TCP round trip, single-threaded exchange, p50 | 9.46 us |
| TCP round trip, threaded exchange with the journal, p50 | 13.3 us |
| From an order on TCP to its price on the multicast feed, p50 | 3.7 us |
| Crash recovery from a journal of 4 million requests, without and with snapshots | 551 ms and 18.6 ms |
| Highest load held with p99 under 1 ms (single thread / with feed / with journal) | 3.2 / 4.8 / 2.4 million requests/s |

## What I learned

- **Real data breaks assumptions.** My fast book assumed a narrow price range, and real Nasdaq data had prices from $0.0001 to $199,999.99 in one stock. So I added a third book that handles any price, and it came within about 6% of the fast one's speed.
- **Profile before guessing.** Throughput once collapsed to about 0.8 million requests per second, and my first guess at the cause was wrong. A profiler showed the time going into hash map deletions, and after the fix it reached 8.27 million.
- **Measuring changes what you measure.** Timing every message put about 54% of the work in reading the clock. Timing one message in 64 brought that to under 1%.
- **Hash keys matter.** One bad key layout made a message take 30.4 us instead of 184.7 ns once 100,000 orders were live.
- **Waking a thread is expensive.** Passing a message between two busy cores takes about 78 ns, but waking a sleeping thread takes tens of microseconds.
- **Flaky tests hide real behavior.** A test that hoped the kernel would drop packets passed on my laptop and failed in CI. Now it drops them on purpose.

## Tools

| Program | What it does |
|---|---|
| `litbook_gateway` | Runs the exchange, optionally with the journal and the market data feed |
| `litbook_feed_listener` | Listens to the market data feed and prints the book |
| `litbook_loadgen` | Sends orders from many clients on a fixed schedule and reports latency |
| `litbook_journal` | Checks, prints or replays a journal |
| `litbook_itch_replay` | Replays a Nasdaq ITCH trading day through the books |
| `litbook_bench` and the `*_latency` tools | Benchmarks of every component |
| `python -m litbook_bots.*` | Python bots (market maker, random trader), a live book view and the simulated market |

The Python package uses the `litbook` module, a C++ order client and feed reader exposed to Python with pybind11. Build presets are `debug`, `release`, `asan` and `tsan`, each also with Clang.

## Docs

- [Design](docs/DESIGN.md), the life of one order from the socket to the trade
- [Design decisions](docs/DECISIONS.md), every choice and the alternative that was rejected
- [Benchmarks](docs/BENCHMARKS.md), every measured number and how it was measured
- [Order entry protocol](docs/PROTOCOL.md), [Market data feed](docs/FEED.md) and [Journal](docs/JOURNAL.md), the message formats
