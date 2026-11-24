# The life of an order

This document follows one order through litbook, from the bytes a client writes to its socket to the trade print on the market data feed. Each step names the file and function that does the work and the decision in [DECISIONS](DECISIONS.md) that explains why it works that way. The byte layouts are in [PROTOCOL](PROTOCOL.md), [FEED](FEED.md) and [JOURNAL](JOURNAL.md), so this document only shows the parts the order touches.

## The setup

The exchange runs as

```sh
litbook_gateway --account ALICE:pw --account BOB:pw --feed lo --journal /tmp/journal
```

which lists one stock, AAPL, with a tick of $0.01, and runs the pipeline with four threads (gateway, journal, matching, publisher) plus the snapshot thread. ALICE already has an order resting in the book, a sell of 100 shares at $100.00. Prices are integers with 4 implied decimals, so $100.00 is 1,000,000 everywhere in the system (D-013, D-042). No floating point number appears anywhere on this path.

BOB now sends a buy of 100 shares at $100.00. It will trade with ALICE's order in full. These are the stops it makes.

```
 BOB's socket                                                         multicast group
     |                                                                 239.255.0.1:15100
     v                                                                       ^
 1. gateway thread           2. journal thread        3. matching thread     |
    epoll_wait, recv            batch of up to 64        OrderEntry          |
    SoupBinTCP framing   --->   append + CRC32C   --->   risk checks  ---+   |
    session rules        SPSC   one write()       SPSC   SortedOrderBook |   |
    QueueBackend::forward       then pass on             events          |   |
     ^                                                                   |   |
     |                    Reply queue (SPSC)                             |   |
     +-------------- 5. replies: Accepted, Executed, RequestDone <-------+   |
     |                                                                   |   |
     v                                         4. publisher thread       |   |
 BOB's and ALICE's sockets                        FeedWorker  <----------+   |
                                                  MoldUDP64 packet  ---------+
                                                  (Trade, Price Level Update)
```

## 0. On the wire

BOB's client encodes an Enter Order of 29 bytes (type `O`, UserRefNum 7, side `B`, quantity 100, symbol `AAPL    `, price 1,000,000, time in force `0` for Day, order type `L`, post only `N`). It wraps it in a SoupBinTCP Unsequenced Data packet, which adds a 2-byte length and the type byte `U`, so 32 bytes go into the socket. Every integer is big-endian, as in Nasdaq's OUCH 5.0.

TCP is a byte stream, so these 32 bytes may arrive in two reads, or together with the next packet in one read. Nothing in the gateway assumes a read holds exactly one message.

## 1. The gateway thread reads the socket

`BasicEpollGateway::poll_once` in `include/litbook/gateway/epoll_gateway.hpp` calls `epoll_wait` over every client socket, the listening socket and two eventfds. BOB's socket is readable, so `read_from` calls `recv` once into a 64 KB buffer. The epoll set is level triggered, which means that if more bytes are waiting, the next `epoll_wait` reports the socket again, so one `recv` per event is enough and no connection can starve the others (D-047).

The gateway has two modes. In blocking mode it sleeps in `epoll_wait` until something happens. In spin mode it calls `epoll_wait` with a timeout of 0 in a loop, which keeps one core at 100% but skips the sleep and the wakeup.

The bytes go to the session layer through `receive`. The gateway itself knows nothing about SoupBinTCP. It only moves bytes between sockets and the session layer, which keeps every protocol rule testable with plain byte arrays and a made up clock (D-044).

## 2. The session layer checks the packet

`BasicSessionManager` in `include/litbook/gateway/session_manager.hpp` keeps one `Session` per connection with a fixed size input buffer and output buffer. `receive` appends the bytes to the input buffer and calls `process_input`, which asks `soup::next_packet` (`src/gateway/soup.cpp`) for the next whole packet. An incomplete packet waits in the buffer for more bytes. A packet whose length is impossible closes the connection.

`handle_packet` applies the session rules. A connection must log in before anything else, and BOB is logged in, so the Unsequenced Data packet goes to `forward_order`.

Before that, `paused` decides whether BOB may send anything at all. A connection whose output buffer is backed up stops being read, so TCP slows the client down instead of the gateway buffering without limit (D-045). In the pipeline the replies come later, so the session layer also counts each account's requests in flight and reserves room for their replies (D-051).

## 3. The order crosses to the next thread

`forward_order` hands the 29-byte frame to the backend. In the pipeline that is `QueueBackend::forward` in `include/litbook/pipeline/queue_backend.hpp`. It decodes the frame once, only to check it, because a malformed frame must close the connection before any later packet from that client goes through, and only the gateway thread can know that in time. Then it copies the raw frame into a `Request` with `make_frame_request` (`include/litbook/pipeline/messages.hpp`).

A `Request` is a 64-byte struct holding the account number, the raw frame and a timestamp. The gateway puts the time on every request here, at the moment it read it. The matching thread never reads a clock, so what the exchange does depends only on the order of the requests in this queue. That single fact makes the journal enough to rebuild the exchange later (D-052).

The request goes into a bounded single-producer, single-consumer queue (`include/litbook/concurrency/spsc_queue.hpp`, D-049). It is a ring of fixed slots with two counters, each on its own 128-byte padded cache line (D-048), and each side keeps a cached copy of the other side's counter, so a push touches no cache line the consumer is writing unless the queue looks full. If the queue is full, `forward` reports Busy, the packet stays in BOB's input buffer, and the gateway retries it on a later round. The gateway thread never waits for any other thread.

Last, `ring()` on the journal thread's doorbell (`include/litbook/pipeline/doorbell.hpp`, D-053). The doorbell is one atomic state. A producer only makes a system call (a write to an eventfd) when the consumer has announced that it is going to sleep, so a busy pipeline makes no system calls to hand work across.

## 4. The journal thread writes it down first

`JournalWorker::step` in `include/litbook/pipeline/journal_worker.hpp` takes every request that is waiting, up to 64, and calls `JournalWriter::append` for each (`src/journal/journal_writer.cpp`). Append encodes a record into a buffer that was allocated at startup. BOB's record is a Frame record (type `F`) of 57 bytes, which are a 24-byte header (length, type, owner, sequence number, timestamp), the 29 bytes of the frame exactly as BOB sent them, and a CRC32C of everything before it (D-059). The CRC uses the CPU's CRC32C instruction on arm64 and on x86, with a table as the fallback (`src/journal/crc32c.cpp`).

Then one `commit()` writes the whole batch with a single `write` system call. When many requests arrive at once they share that call, which is called group commit (D-060). With `--sync sync` the commit also calls `fdatasync` and waits for the disk. With the default it does not, so a crash of the process loses nothing, while a crash of the whole machine can lose the last batches.

Only after the commit does the worker push the batch on to the matching thread's queue (D-061). So the matching thread never sees a request that is not in the journal, and no reply or trade can leave the exchange before its cause is written. That is what makes it a write-ahead log. If a write fails, the writer stops for good, the batch is never passed on, and the exchange shuts down rather than run without a journal.

Every million records the journal thread starts a new segment file (D-065). The snapshot thread replays each closed segment into its own copy of the exchange and writes a snapshot, so recovery after a crash only replays the records after the newest snapshot, and the matching thread never pauses for a snapshot (D-066, D-062).

## 5. The matching thread runs the order

`MatchingWorker::step` in `include/litbook/pipeline/matching_worker.hpp` reads the request in place at the front of its queue and calls `handle`, which passes the frame to `OrderEntry::on_frame` (`include/litbook/protocol/order_entry.hpp`). The frame is decoded again here. Sending raw bytes and decoding twice costs one more decode (28.5 ns for an Enter Order) and keeps the queue messages small and fixed in size, and the journal gets the exact bytes the client sent (D-052).

`on_frame` decodes with `decode_inbound` (`src/protocol/codec.cpp`), which reads each field at its documented offset with explicit big-endian loads and never casts the buffer to a struct (D-042). `on_message(owner, EnterOrder)` then applies the exchange's rules before the book sees the order.

1. **UserRefNum.** BOB's 7 must be higher than every UserRefNum BOB used before, or the message is ignored.
2. **Symbol.** `Engine::find` looks the ticker up in a hash map and gets a dense SymbolId, the index of AAPL's book (D-041).
3. **Risk limits.** The order must not exceed BOB's maximum order size, and BOB's account must have fewer open orders than its limit. A refused order gets a Rejected message and never reaches the book (D-046).

The order entry layer then takes a record for the order from a pool, gives it the exchange's next Order Reference Number, and files the record under two keys in two `FlatIdMap`s (`include/litbook/container/flat_id_map.hpp`). One key is the reference number, which is how the book's events find the order. The other is the pair of BOB's account and UserRefNum, which is how BOB's later cancel finds it (D-043). FlatIdMap is an open addressing hash map that never allocates after startup. Its hash folds the key instead of multiplying it (D-039), the client key puts the account in its lowest digits so accounts do not crowd into one probe run (D-067), and its entries are kept in Robin Hood order so an erase stops early (D-070). Each of those three decisions came from a measured slowdown.

Then `Engine::submit` (`include/litbook/engine/engine.hpp`) passes the order to AAPL's book.

## 6. The book matches it

`litbook_gateway` uses `SortedOrderBook` (`src/book/sorted_order_book.cpp`). Each side keeps its price levels in a sorted array with the best price at the end, and each level keeps its orders in a first in, first out list of pooled nodes. Real Nasdaq data has resting quotes from $0.0001 to $199,999.99 in the same stock, which a flat price ladder cannot hold, and this book handles any price without allocating (D-036). The other two books, `FastOrderBook` (a flat ladder, D-027) and `MapOrderBook` (standard containers, the reference, D-014), behave identically and are tested against each other on the same random streams.

`SortedOrderBook::submit` validates the order (tick size, lot size, price band) and reports `on_accepted`. Then `match` looks at the best ask, which is ALICE's level at the end of the ask array. BOB's price crosses it, so it walks that level's list from the oldest order. That is price-time priority. ALICE's order is first. The self-trade check passes because the two orders belong to different accounts (D-022). The fill is 100 shares, which leaves both orders with nothing remaining, so the book removes ALICE's order and reports two events.

1. `on_trade` with the match number, both order IDs, the price, 100 shares, the aggressor's side (buy) and what is left of each order.
2. `on_level_update` for the ask level at $100.00, now with 0 shares and 0 orders, so the level is gone.

BOB's order has nothing left, so it does not rest. The order of every event is fixed by contract (D-015), which is what lets the three books be compared event by event and the feed be checked against the replies.

Each book numbers its own trades, so the engine's small forwarder replaces the match number with one counter for the whole exchange, unique for the day across every stock, as OUCH and ITCH require.

## 7. The events turn into messages

The book's events come back to the order entry layer, which is the book's listener.

- `on_accepted` sets the record's open quantity and sends BOB an Order Accepted (45 bytes) with the Order Reference Number.
- `on_trade` calls `executed` twice. BOB gets an Order Executed (34 bytes) with liquidity flag Removed, because BOB's order took liquidity, and ALICE gets one with flag Added. Both orders are now finished, so both records are released. Then the trade goes on to the market data sink.
- `on_level_update` goes on to the market data sink.

Every outbound message is encoded by the codec and handed to `MatchingWorker::send`, which wraps it in a `Reply` and pushes it on the reply queue. The trade and the level update become two `MarketEvent`s on the event queue, each stamped with the request's timestamp, so the feed depends only on the order of the requests too. After the request is finished, `handle` pushes one RequestDone reply for BOB, which tells the gateway this request is no longer in flight.

A full reply queue or event queue is never a reason to drop anything. The matching thread rings the consumer and spins until there is room. This cannot deadlock, because the gateway and the publisher never wait for the matching thread (D-052). At the end of the request it rings both doorbells once.

## 8. The replies go back to the clients

The reply doorbell's eventfd is in the gateway's epoll set, so a sleeping gateway wakes up. Every round of `poll_once` calls `ReplyPump::drain` (`include/litbook/pipeline/reply_pump.hpp`), which hands each Reply to the session layer. `on_reply` gives the message the account's next sequence number and wraps it in a SoupBinTCP Sequenced Data packet (`S`) in that connection's output buffer. RequestDone lowers BOB's count of requests in flight, which may let paused input from BOB run again.

At the end of the round `service_all` calls `flush` for every connection, which writes the output buffer with `send` until the kernel stops taking bytes. `send` uses `MSG_NOSIGNAL`, so writing to a client that already hung up is an error code instead of a signal that would kill the exchange. Anything the kernel does not take stays in the buffer, and the socket's epoll interest gains `EPOLLOUT` so the gateway hears when it can go on.

BOB reads Order Accepted and then Order Executed. ALICE reads Order Executed. Both carry the same match number.

## 9. The trade print goes out on the feed

`FeedWorker::step` in `include/litbook/pipeline/feed_worker.hpp` pops the two market events and passes them to `FeedPublisher::on_event` (`src/feed/feed_publisher.cpp`). The trade becomes a Trade message (`P`, 32 bytes) and the level update a Price Level Update (`L`, 32 bytes). The feed is market by price, so it tells the world what each price level now holds, never who owns the orders (D-055). The publisher adds both to the open MoldUDP64 packet, after its 20-byte header (session name, sequence number of the first message, message count).

When the event queue runs empty, `service` calls `flush`, and the packet goes out at once (D-057). There is no timer. An order that trades through several levels usually leaves in one packet, because all its events are waiting when the publisher looks. Here the packet holds 2 messages and is 88 bytes. `MulticastSender::send` (`src/feed/multicast.cpp`) writes it to a connected, non-blocking UDP socket on the incremental group. A slow network can make the publisher drop a packet and count it, but never makes it wait (D-056).

The publisher also keeps its own copy of every stock's levels, built from the same events. Between events it sends one stock's snapshot every millisecond on a second multicast group, so a receiver that loses a packet or joins late can rebuild correct books without asking the exchange for anything.

## 10. Someone reads it

A receiver (`litbook_feed_listener`, or the Python `FeedBook` that the bots use) runs a `FeedReceiver` on both groups (`src/feed/feed_receiver.cpp`). `FeedHandler` (`src/feed/feed_handler.cpp`) checks the session name and the sequence number. The next expected number means it applies the Price Level Update to its `LevelBook`, and AAPL's ask at $100.00 disappears there too. A higher number means packets were lost, so every stock turns stale until a snapshot repairs it (D-055). A receiver that hears nothing for three heartbeat intervals says the feed is silent and does not pretend the session ended (D-058). A receiver that joined late only trusts its books after it has read the incremental stream once (D-073).

## What each step costs

These numbers come from different benchmarks, run on different days, so they do not add up to the round trip. Each one is a row in [BENCHMARKS](BENCHMARKS.md), measured on an Apple M3 Pro inside a Linux VM under Docker.

| Step | Measured | Unit |
|---|---|---|
| Decode one Enter Order (the same frame every time) | 28.5 ns | per message |
| One hop through an SPSC queue between two cores | about 78 ns | per hop (half a round trip) |
| Append one journal record, encode and CRC32C, no system call | 12.7 ns | per record |
| `write` of a committed batch of 64 records | 1.64 us | per batch (25.6 ns per record) |
| `fdatasync` under `--sync sync` | about 0.35 ms | per commit |
| Order entry layer, decode, rules, book and every reply encoded (small books) | 80.2 to 87.3 ns | per frame |
| The same with 102,999 live orders | 184.7 ns | per frame |
| Publisher with a real UDP socket | 315.5 ns | per event (`send` is about 94%) |
| Feed handler applying the incremental stream | 49.6 ns | per message |

And the whole path, all medians (p50).

| Path | Spin mode | Blocking mode |
|---|---|---|
| TCP round trip, one thread doing everything (Phase 6) | 9.46 us | 34.0 to 34.6 us |
| TCP round trip, pipeline with the journal (Write policy, no feed) | 13.29 us | 85.29 to 87.96 us |
| Time inside the gateway for that order, from forward to every reply in the output buffer | 0.72 to 0.75 us | not recorded |
| From an Enter Order on TCP to its level on the multicast feed (pipeline, no journal) | 3.71 us | 73.29 us |

Two things stand out. First, the exchange's own work is under 1 us of the 13.29 us round trip. The rest is two kernels' loopback TCP paths and the client waking up to read its reply. Second, blocking mode costs far more than the queues. A thread that sleeps has to be woken from another vCPU through the hypervisor. Two threads on different vCPUs that wake each other once each took about 30 us per round trip (p50) in this VM, against a mean of 155 to 184 ns for the same round trip when both threads spin.

## What can go wrong on the way

| Where | What happens | Why |
|---|---|---|
| Bytes that are not a valid OUCH message | The gateway closes the connection before the frame reaches the queue | A malformed frame is a broken client, not a rejected order (D-042) |
| A full request queue | The packet waits in the input buffer and is retried | The gateway never waits for another thread (D-051) |
| A client that stops reading | Its input pauses, then TCP pushes back. Only replies that cannot be held back and fill the whole output buffer disconnect it | D-045 |
| A failed journal write | The batch is not passed on and the exchange shuts down | A request that may not be on disk must not have effects (D-060, D-061) |
| A crash, even `kill -9` | The next start loads the newest snapshot, replays the records after it, cancels orders of accounts with cancel on disconnect, and starts a new feed session | D-062, tested by killing the exchange at random points |
| A lost feed packet | The receiver sees the gap, marks every stock stale and rebuilds from snapshots | D-055 |

## Why this order of threads

The journal sits before matching, not after it. Writing the replies or the book changes after matching would also work for recovery, but then a reply could reach a client before anything on disk explains it, and a crash in between would leave a client holding a fill the exchange forgot. Writing the requests first means the journal is the cause and everything else is an effect that can be computed again.

Matching is one thread for every stock. Splitting stocks across threads would add throughput, but it would also make the order of events across stocks depend on timing, and a single deterministic thread keeps replay exact. The load tests held 4.8 million requests per second through the pipeline with the feed with p99 under 1 ms. More matching threads, each owning a group of stocks, would be the next step past that.

The gateway thread does no matching. In the single-threaded gateway a slow order delays reading every other socket. In the pipeline it only delays the matching queue, and the gateway keeps reading and applying backpressure.
