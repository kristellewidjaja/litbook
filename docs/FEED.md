# litbook market data feed

This is the binary feed the exchange uses to tell everyone what its books look like. It is a market by price feed. It reports the total quantity and the number of orders at each price level, plus every trade, but never single orders. The packet layer follows Nasdaq's MoldUDP64 specification (version 1.00, formatting update of August 2024). The messages inside the packets are litbook's own, written in the same style as Nasdaq ITCH 5.0 (a type byte, then big-endian fields at fixed offsets). Every choice that the MoldUDP64 specification leaves open is marked as litbook's choice, and every difference is listed in [Differences](#differences-from-moldudp64-and-itch-50).

The feed has two streams, each sent to its own UDP multicast group.

- The **incremental stream** carries every change in the order it happened. A receiver that has seen every message from the first one has an exact copy of every book.
- The **snapshot stream** repeats the full state of every book over and over. A receiver that joined late, or that lost a packet, uses it to get back to a correct copy (see [Recovery](#recovery)).

## Packets: MoldUDP64

Every UDP datagram is one MoldUDP64 downstream packet. All numbers in the header and the message lengths are big-endian.

| Field | Offset | Length | Notes |
|---|---|---|---|
| Session | 0 | 10 | ASCII. The name of the trading session the packet belongs to. |
| Sequence Number | 10 | 8 | The sequence number of the first message in the packet. The messages after it are numbered one by one from there. |
| Message Count | 18 | 2 | How many message blocks follow. 0 means a heartbeat and 0xFFFF means end of session. |

The 20-byte header is followed by Message Count message blocks. Each block is a 2-byte Message Length, which counts only the bytes after it, followed by that many bytes of message.

### Rules of the litbook feed

1. **Session name.** The session name is padded on the left with spaces, as in SoupBinTCP's Login Accepted packet. The specification does not say how to pad it, so this is litbook's choice. Both streams use the same session name.
2. **Sequence numbers.** Each stream counts its own messages, starting at 1 for the first message of the session. The specification does not say where numbering starts, so this is litbook's choice. A snapshot message's sequence number belongs to the snapshot stream and has nothing to do with the incremental numbers, except through the As Of Sequence field below.
3. **Packet size.** A packet is at most 1,200 bytes of UDP payload. The specification lets the sender choose. 1,200 stays below the 1,472 bytes that fit in one Ethernet frame with an MTU of 1,500, with room left for tunnels and virtual networks with smaller MTUs, so a packet is never split into IP fragments. A message is never split across two packets.
4. **When a packet is sent.** The publisher adds messages to a packet until the next one would not fit, or until it has no more events waiting. So one order that trades through several levels usually leaves in one packet, and no message waits for a timer.
5. **Heartbeats.** After one second with nothing sent, a stream sends a heartbeat (Message Count 0) whose Sequence Number is the next number it will use. A receiver can therefore notice a lost packet even when the market is quiet.
6. **End of session.** At the end of the session each stream sends a few packets with Message Count 0xFFFF and the next sequence number, in place of heartbeats.
7. **No retransmission.** MoldUDP64 also defines a Request Packet that asks a re-request server to send missed messages again. litbook has no re-request server. A receiver that misses something recovers from the snapshot stream.
8. **Other sessions.** A receiver ignores and counts packets whose session name is not the one it expects.
9. **Silence.** A receiver that heard the feed and then gets no packet of its session on either stream for three seconds (three heartbeat intervals) reports the feed as silent. This is litbook's choice, the specification has no such rule. It is not the same as End of Session. The receiver cannot tell whether the publisher stopped, the network failed, or End of Session was sent and lost, so it keeps its books as they are and says it is silent. The next packet ends the silence, and any messages lost in between show up as a gap in the sequence numbers.

## Transport

Each stream is sent to its own IPv4 multicast group by UDP, from one socket per stream.

| | Incremental stream | Snapshot stream |
|---|---|---|
| Default group | 239.255.0.1 | 239.255.0.2 |
| Default port | 15100 | 15101 |

- The 239.255.0.0/16 range is the IPv4 organization-local scope (RFC 2365), meant for groups that stay inside one site, so the defaults never clash with public multicast.
- The publisher and every receiver name the network interface, `lo` by default. Tests and the tools on one machine use `lo`. Linux carries multicast on `lo` when a socket asks for it by interface index, even though `lo` does not have the MULTICAST flag.
- The TTL is 1, so a packet never crosses a router.
- The sender never waits. If the kernel has no room for a packet, the packet is lost and counted, and receivers recover from the snapshot stream as from any other loss.
- A receiver binds to the group address and turns `IP_MULTICAST_ALL` off, so it only gets the group it joined, even when another group uses the same port on the same host.
- The two groups can be changed with `litbook_gateway --feed INTERFACE [--feed-incremental ADDRESS:PORT] [--feed-snapshot ADDRESS:PORT]`, and the same flags of `litbook_feed_listener`.

To watch the feed of a running exchange on one machine:

```sh
./build/release/tools/litbook_gateway --feed lo --account alice:secret --symbol AAPL &
./build/release/tools/litbook_feed_listener --every 1 --depth 5
```

The listener prints each stock's best levels every second. It exits with 0 at End of Session, and with 3 if the feed goes silent (rule 9) after it was heard.

A receiver only accepts packets of the session it expects (rule 8), which is `1` unless it is told otherwise. A gateway without a journal always starts session `1`. A gateway that restarts from its journal starts a new session named after the journal's Restart record (docs/JOURNAL.md), so the receivers of the old session hear nothing more and the new session's receivers rebuild their books from snapshots. The gateway's second line names the session, for example `litbook_gateway feed on lo: incremental 239.255.0.1:15100, snapshot 239.255.0.2:15101, session 7`, and `litbook_feed_listener --session 7` or `python3 -m litbook_bots.book_view --session 7` then follows it.

## Data types

| Type | Width | Encoding |
|---|---|---|
| Byte | 1 | A single ASCII code, such as a message type or a side. |
| Short | 2 | Unsigned, big-endian. |
| Integer | 4 | Unsigned, big-endian. |
| Long | 8 | Unsigned, big-endian. |
| Price | 8 | Unsigned, big-endian, 4 implied decimals, the same unit as the [order entry protocol](PROTOCOL.md#data-types). The largest legal price is 1,999,999,900 ($199,999.9900). |
| Timestamp | 8 | Long. Nanoseconds since midnight. |
| Locate | 2 | Short. The exchange's number for a stock, from 0 up, the same for the whole session. The Symbol Directory message says which stock it is. ITCH calls this field Stock Locate. |
| Symbol | 8 | ASCII, left-justified, padded on the right with spaces, with the same rules as in the order entry protocol. |
| Side | 1 | Byte. `B` buy (bids) or `S` sell (offers). |

Timestamps on incremental messages come from the inbound order that caused them, which the gateway stamped when it read the order. So the incremental stream depends only on the order of the inbound requests, which is what the journal of Phase 9 needs. Only heartbeats and snapshots use the publisher's clock.

## Messages

Both streams use the same message types. Every message has a fixed length, and a message whose length does not match its type is malformed.

### System Event, type `S`, 10 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Message Type | 0 | 1 | Byte | `S` |
| Timestamp | 1 | 8 | Timestamp | |
| Event Code | 9 | 1 | Byte | `O` start of messages, the first message of the incremental stream. `C` end of messages, the last one. |

### Symbol Directory, type `R`, 31 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Message Type | 0 | 1 | Byte | `R` |
| Timestamp | 1 | 8 | Timestamp | |
| Locate | 9 | 2 | Locate | |
| Symbol | 11 | 8 | Symbol | |
| Tick Size | 19 | 8 | Price | Every price in this stock's book is a multiple of it. |
| Lot Size | 27 | 4 | Integer | Every order quantity is a multiple of it. |

The incremental stream sends one Symbol Directory per stock right after Start of Messages. The snapshot stream sends it again before each stock's snapshot, so a late joiner learns the directory there.

### Price Level Update, type `L`, 32 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Message Type | 0 | 1 | Byte | `L` |
| Timestamp | 1 | 8 | Timestamp | |
| Locate | 9 | 2 | Locate | |
| Side | 11 | 1 | Side | |
| Price | 12 | 8 | Price | |
| Total Quantity | 20 | 8 | Long | The quantity of every order resting at this price on this side, after the change. |
| Order Count | 28 | 4 | Integer | How many orders rest at this price on this side, after the change. |

The message gives the new state of the level, not the change, so applying the same message twice does no harm. Total Quantity 0 together with Order Count 0 means the level is now empty and is removed. A message where only one of the two is 0 is malformed.

### Trade, type `P`, 32 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Message Type | 0 | 1 | Byte | `P`, the letter ITCH uses for a trade. |
| Timestamp | 1 | 8 | Timestamp | |
| Locate | 9 | 2 | Locate | |
| Aggressor Side | 11 | 1 | Side | The side of the incoming order that traded against a resting one. |
| Price | 12 | 8 | Price | |
| Quantity | 20 | 4 | Integer | Not 0. |
| Match Number | 24 | 8 | Long | The same number the two orders' Order Executed messages carry. Unique for the day across all stocks. |

A trade is always followed by the Price Level Update of the resting level it took quantity from.

### Snapshot Start, type `Y`, 25 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Message Type | 0 | 1 | Byte | `Y` |
| Timestamp | 1 | 8 | Timestamp | When the snapshot was sent. |
| Locate | 9 | 2 | Locate | |
| As Of Sequence | 11 | 8 | Long | The incremental sequence number of the last message this snapshot includes. 0 means it includes no incremental message. |
| Level Count | 19 | 4 | Integer | How many Price Level Update messages follow, bids first from best to worst, then offers from best to worst. |
| Symbol Count | 23 | 2 | Short | How many stocks the feed has. Locates go from 0 to Symbol Count minus 1, so a receiver that joined late knows when it has a snapshot of every stock. A Snapshot Start whose Locate is not below its Symbol Count is malformed. |

### Snapshot End, type `Z`, 19 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Message Type | 0 | 1 | Byte | `Z` |
| Timestamp | 1 | 8 | Timestamp | |
| Locate | 9 | 2 | Locate | |
| As Of Sequence | 11 | 8 | Long | The same value as in Snapshot Start. |

On the snapshot stream one stock's snapshot is a Symbol Directory, a Snapshot Start, Level Count Price Level Updates and a Snapshot End, sent with consecutive snapshot sequence numbers. It may span several packets. A cycle sends one snapshot per stock and then starts again.

## Recovery

A receiver keeps the next incremental sequence number it expects. For every stock it also keeps whether the stock is **live** (its book is right) or **stale** (it may have missed something), and the last incremental sequence number its book includes.

1. A message with exactly the expected number is applied, and the expected number goes up by one. A packet that starts below the expected number but also carries new messages has only its new messages applied.
2. A message with a lower number was seen already (for example in a duplicated packet) and is dropped. So is a level update for a live stock whose book already includes that number, which happens after a snapshot that was newer than the incremental stream.
3. A higher number, in a packet or in a heartbeat, means messages were lost. The receiver cannot know which stocks they were about, so every stock becomes stale. The stock now needs a snapshot that includes everything before the first message that arrived after the gap. From then on the receiver keeps the level updates of stale stocks in a bounded buffer and reads the snapshot stream. Trades of stale stocks are counted as lost.
4. A stock's snapshot counts only if its messages from Snapshot Start to Snapshot End arrived with no gap in the snapshot sequence numbers and the number of levels matches Level Count. It can be used only if As Of Sequence plus one is at least the number the stock needs. Then the stock's book is replaced by the snapshot, the buffered updates for that stock with a number above As Of Sequence are applied, and the stock is live again. An older snapshot is ignored, and the receiver waits for the next cycle.
5. When every stock is live again, the buffer is emptied and the receiver goes back to reading only the incremental stream.
6. If the buffer fills up, it is emptied. Every stale stock then needs a snapshot that includes everything before the update that did not fit.

A receiver that joins late does the same thing, because its first packet shows a gap from sequence 1. It learns the directory and the number of stocks from the snapshot stream. A message lost at the very end of a busy period shows up only when the next packet or heartbeat arrives, which is why a quiet stream sends a heartbeat every second. Trades are not part of a book's state, so trades lost in a gap are not recovered. Without a re-request server there is no way to get them back.

The publisher is `FeedPublisher` (`include/litbook/feed/feed_publisher.hpp`) and the receiver is `FeedHandler` (`include/litbook/feed/feed_handler.hpp`). Neither has sockets, and both are tested with a channel that drops, duplicates, reorders and delays packets on purpose. On the network, the publisher runs inside `FeedWorker` on the publisher thread of the exchange and sends through `UdpFeedSink`, and `FeedReceiver` reads both groups into a `FeedHandler` and watches for silence.

A late joiner needs one whole snapshot of every stock taken after it joined. Snapshots go out one stock at a time, one every millisecond by default, so a late joiner is in step after about one cycle, the number of stocks times one millisecond. Until a receiver has read its first incremental packet it has not seen a gap yet, so a receiver that joined late should not trust its books before then. A heartbeat arrives within a second even in a quiet market.

## Differences from MoldUDP64 and ITCH 5.0

| Area | Nasdaq | litbook |
|---|---|---|
| Retransmission | A Request Packet to a re-request server gets missed messages back. | No re-request server. Recovery uses the snapshot stream only, and lost trades stay lost. |
| Silence | Not specified. | A receiver reports the feed as silent after three seconds without a packet (rule 9). |
| Session name and first sequence number | Not specified. | Padded on the left with spaces. Numbering starts at 1. |
| Packet size | Chosen by the sender. | At most 1,200 bytes of UDP payload. |
| Book data | ITCH 5.0 is a market by order feed. It sends every order (Add Order, Order Executed, Order Cancel, Order Delete, Order Replace), and the receiver builds the levels itself. | A market by price feed. One Price Level Update gives the new total of a level. |
| Timestamp | 6 bytes in ITCH. | 8 bytes, as in OUCH 5.0 and the order entry protocol. |
| Snapshots | ITCH has no snapshot messages. Nasdaq offers separate snapshot products. | Snapshot Start, Price Level Updates and Snapshot End on a second multicast group. |
| Symbol Directory | ITCH's `R` message has many more fields (market category, financial status, round lot size and others). | Locate, Symbol, Tick Size and Lot Size only. |
| Other messages | Stock Trading Action, Reg SHO, MWCB, IPO, LULD, NOII, cross trades and others. | Not implemented. |
