# litbook

A low-latency electronic exchange simulator in modern C++ (C++20).

A *lit* market is a venue with a publicly visible order book, as opposed to a dark pool, where resting orders are hidden. litbook is a lit venue. It matches orders with price-time priority and publishes its book to anyone listening on the market data feed.

## Quick start

litbook runs on Linux. On macOS or Windows the dev container gives the same environment, and needs only Docker.
```sh
git clone https://github.com/kristellewidjaja/litbook.git
cd litbook
./scripts/dev.sh      # builds the dev image the first time, then opens a shell in it
```
Inside that shell, build and run every test.
```sh
cmake --preset release && cmake --build --preset release
ctest --preset release
```
Then start a simulated market. It runs the exchange with one market maker and four noise traders for 20 seconds and shows the live book. At the end it prints each bot's position and profit, the final book, and five checks that the market added up.
```sh
export PYTHONPATH=build/release/python:python
python3 -m litbook_bots.simulate --seconds 20 --makers 1 --noise 4 --seed 1 \
    --gateway build/release/tools/litbook_gateway --view
```
Or make one trade by hand, the same trade that [DESIGN](docs/DESIGN.md) follows through the code. ALICE offers 100 shares at $100.00, which rests in the book, and then BOB buys them.
```sh
build/release/tools/litbook_gateway --account ALICE:pw --account BOB:pw --feed lo &
python3 -c 'import litbook; c = litbook.OrderClient("127.0.0.1", 15000, "ALICE", "pw"); print(c.enter("AAPL", litbook.Side.Sell, 100, 1_000_000))'
python3 -m litbook_bots.book_view --once
python3 -c 'import litbook; c = litbook.OrderClient("127.0.0.1", 15000, "BOB", "pw"); print(c.enter("AAPL", litbook.Side.Buy, 100, 1_000_000)); print(c.events(timeout_ms=100))'
python3 -m litbook_bots.book_view --once
kill %1
```
BOB gets `Accepted` and then `Executed` with match number 1, and the book that showed ALICE's offer is empty afterwards. ALICE's own `Executed` is counted as dropped when the gateway stops, because ALICE's client had already exited.

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

[DESIGN](docs/DESIGN.md) follows one order through all of this, from the socket to the trade print, with the file, the function and the design decision at every step.

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

All numbers come from one machine, an Apple M3 Pro laptop (11 cores, 18 GiB) running macOS, with everything inside Docker Desktop's Linux virtual machine (11 vCPUs, 7.7 GiB, an Ubuntu 26.04 arm64 container), built by GCC 15.2 with `-O3`. A virtual machine adds noise, so most rows are medians over several passes and several runs. Its timer ticks every 41.67 ns, which is longer than one book operation, so book operations are timed as averages over many operations and only the far tails of single operations are meaningful. Every row, with its date, commit, flags and conditions (including the runs made with other apps open), is in [BENCHMARKS](docs/BENCHMARKS.md).

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

## What I learned

- **Real data breaks assumptions.** My fast book was a flat price ladder, one slot per possible price. A real Nasdaq day has resting quotes from $0.0001 to $199,999.99 in the same stock, which no ladder can hold. I kept the ladder and added SortedOrderBook, which keeps only the levels that exist. On the 1,611 stocks a ladder can hold, the sorted book came within about 6% of the ladder, and it handles every stock.
- **Timing something changes it.** Timing every message of the ITCH day put about 54% of the profile's samples in reading the clock. Timing one message in 64 brought the clock down to 0.81% of the samples, and the per-message numbers from the first method had to be marked as inflated.
- **Profile before guessing.** Under load the pipeline suddenly fell to about 0.8 million requests per second. My first explanation, from reading code, was wrong. A `perf` profile put about 59% of the matching thread's samples in two functions that erase from a hash map, and a counter showed each erase reading about 261 slots on average. Keeping the entries in Robin Hood order fixed it (8.27 million requests per second in the same test) and costs about 3 ns per FastOrderBook operation, which I measured and wrote down instead of hiding.
- **A hash map is only as good as its keys.** Putting the account in the high bits of the client order key made one probe run of about 50,000 slots at 100,000 live orders. Moving the account to the lowest digits took a message from 30.4 us to 184.7 ns.
- **Sleeping costs more than queues.** A hop through the lock-free queue between two spinning cores costs about 78 ns. Waking a sleeping thread on another vCPU costs tens of microseconds in this VM. A queue benchmark that switched between 1.9 us and 31 us per round trip turned out to depend on whether the scheduler had put both threads on the same vCPU.
- **Determinism makes recovery simple.** The gateway puts the time on every request and the journal writes it before matching sees it, so the exchange is a function of its journal. Recovery is a replay, and a snapshot taken on another thread cut recovery from a 4 million request journal from 551 ms to 18.6 ms.
- **Tests that hope for timing fail somewhere else.** A feed test that waited for the kernel to drop packets passed on my machine and failed in CI's release build, where the reader kept up. It now loses packets on purpose. Every timing-sensitive test is also run many times pinned to two CPUs, like CI.

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

## Python

The `litbook` Python module (pybind11) has a blocking order entry client and a book kept from the market data feed. Prices are integer ticks with 4 implied decimals, as on the wire, so 1,000,000 is $100.00.
```python
# Against a gateway started with: litbook_gateway --account ALICE:pw --feed lo
import litbook

with litbook.OrderClient("127.0.0.1", 15000, "ALICE", "pw") as client:
    reply = client.enter("AAPL", litbook.Side.Buy, 100, 1_000_000)  # Accepted or Rejected
    for event in client.events(timeout_ms=100):                      # executions and cancels
        print(event)
    client.cancel(reply.user_ref)  # Canceled, or None if the order already filled

book = litbook.FeedBook()          # joins the default feed groups on lo
book.wait_update(0, 1000)
print(book.best("AAPL"), book.depth("AAPL", litbook.Side.Sell, levels=5))
```
The `debug` and `release` presets build it into `build/<preset>/python/` (add that folder to `PYTHONPATH`). The sanitizer presets do not build it, because a sanitizer runtime cannot be loaded into a normal Python process (D-071).

### Bots and a simulated market

`python/litbook_bots/` is a small package of bots written on top of the module (D-072).

| Module | What it does |
|---|---|
| `market_maker` | Keeps one post-only bid and one offer around a fair value taken from the other traders' best prices, leans its quotes against its position and stops buying or selling at a position limit. It requotes with OUCH Replace and pulls its quotes while the feed is not live. |
| `noise_trader` | Sends random orders at exponentially distributed times from a seeded generator, a mix of IOC orders that take liquidity, resting limit orders a few ticks behind the middle, and cancels. |
| `book_view` | A live terminal view of one stock's book from the feed (curses), or one printed picture with `--once`. |
| `simulate` | Starts `litbook_gateway` with its feed on loopback, runs makers and noise traders as threads, then checks that every match was seen by both sides, positions add up to zero, the feed printed one trade per match, the feed's book equals the bots' open orders, and the book is empty once they log out. |

```sh
export PYTHONPATH=build/release/python:python
python3 -m litbook_bots.simulate --seconds 20 --makers 1 --noise 4 --seed 1 \
    --gateway build/release/tools/litbook_gateway --view
```
Each module keeps its decisions (quotes, inventory skew, the next random order, the picture of the book) in plain functions, separate from the loop that waits on the network, and the tests check those functions with plain values and the loops with stand-in clients.

## Building

Development targets Linux. On macOS or Windows, use the Docker dev container (Ubuntu 26.04, GCC 15, Clang 21):
```sh
./scripts/dev.sh                      # shell inside the container, repo mounted at /work
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```
Presets are `debug`, `release`, `asan` (AddressSanitizer and UBSan) and `tsan` (ThreadSanitizer), each also as `-clang`. Run benchmarks from the `release` preset only.

## Docs

- [Design](docs/DESIGN.md), the life of one order from socket to trade print
- [Design decisions](docs/DECISIONS.md), every choice with the alternative that was rejected
- [Benchmarks](docs/BENCHMARKS.md), where every number comes from a recorded run
- [Order entry protocol](docs/PROTOCOL.md), the byte layouts of the OUCH and SoupBinTCP messages
- [Market data feed](docs/FEED.md), the feed messages, packets and recovery rules
- [Journal](docs/JOURNAL.md), the journal and snapshot formats and how recovery works
