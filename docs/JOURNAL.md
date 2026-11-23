# litbook journal

The journal is a write-ahead log of every inbound request the matching engine handles. Each request is written to the journal before it is handled, so after a crash the exchange can be rebuilt by replaying the journal, either from the start or from a snapshot of the whole state plus the records after it. The matching thread reads no clock and nothing else outside its requests (D-052, D-055), so replaying the same records gives the same books, the same replies byte for byte and the same market data feed.

The format is litbook's own. It is a local file that only litbook reads, so it is not modeled on an exchange specification. The record layout (a length, a type, the data and a CRC32C) is close to the write-ahead log of LevelDB. The code lives in `include/litbook/journal/` and `src/journal/`, and the tool `litbook_journal` checks, prints, replays and snapshots a journal.

## Directory layout

A journal is a directory. The records are split into segment files, and snapshot files sit next to them.

```
00000000000000000001.journal    records 1 to 1,000,000 (a closed segment)
00000000000001000001.journal    records 1,000,001 onwards (the open segment)
00000000000001000000.snapshot   the whole state as of record 1,000,000
00000000000001200000.snapshot.tmp   a snapshot being written, or left half written by a crash
```

| File | Named by | Contents |
|---|---|---|
| `<N>.journal` | the sequence number of its first record | A segment, laid out as below. Its file header holds the same number. |
| `<N>.snapshot` | the sequence number of the last record it includes | A snapshot (see "Snapshots"). |
| `<N>.snapshot.tmp` | as above | A snapshot that is not finished. It is never read, and recovery deletes it. |

The numbers have 20 digits, enough for any 64-bit number, so the names sort in sequence order. Any other file in the directory is left alone. Only the last segment is ever written to. The segments must form one chain. The first one may start anywhere (older ones are removed once a snapshot covers them), and each later one must start at the sequence number where the one before it ended.

## Segment file layout

A segment file is one 32-byte file header followed by records, one after another with nothing in between. A segment is only ever appended to.

## Data types

Every number is little-endian (least significant byte first), unlike the order entry protocol and the feed, which are big-endian on the wire. Every machine litbook runs on is little-endian, and nothing outside litbook reads the file. The fields are still written one byte at a time at fixed offsets, never by copying a struct, so the file is the same whatever the compiler does with padding.

| Type | Width | Encoding |
|---|---|---|
| Byte | 1 | Unsigned. |
| Short | 2 | Unsigned, little-endian. |
| Integer | 4 | Unsigned, little-endian. |
| Long | 8 | Unsigned, little-endian. |
| Signed Long | 8 | Two's complement, little-endian. Prices, in the 4-decimal unit of the order entry protocol. |
| Timestamp | 8 | Long. Nanoseconds since midnight, as the gateway stamped the request. |
| Symbol | 8 | ASCII, left-justified, padded on the right with spaces, with the rules of the order entry protocol. |

## File header, 32 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Magic | 0 | 8 | ASCII | `LITBOOKJ` |
| Version | 8 | 2 | Short | 1 |
| Header Length | 10 | 2 | Short | 32 |
| Flags | 12 | 4 | Integer | 0. Any other value is refused. |
| First Sequence | 16 | 8 | Long | The sequence number of the first record. At least 1. A new journal starts at 1. |
| Reserved | 24 | 4 | Integer | 0 |
| Checksum | 28 | 4 | Integer | CRC32C of bytes 0 to 27. |

## Records

Every record has a 24-byte header, then its payload, then a 4-byte checksum.

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Record Length | 0 | 2 | Short | The whole record, header and checksum included. 28 to 75. |
| Record Type | 2 | 1 | Byte | One ASCII letter, see below. |
| Reserved | 3 | 1 | Byte | 0 |
| Owner | 4 | 4 | Integer | The account, or 0 for a record that belongs to no account. |
| Sequence Number | 8 | 8 | Long | One more than the record before. The first record has the header's First Sequence. |
| Timestamp | 16 | 8 | Timestamp | The time the gateway put on the request. Setup records carry the startup time. |
| Payload | 24 | n | | Depends on the type. 0 to 47 bytes. |
| Checksum | 24 + n | 4 | Integer | CRC32C of every byte of the record before it. |

The checksum is CRC32C, the CRC with the Castagnoli polynomial 0x1EDC6F41 used by iSCSI (RFC 3720). Its register starts as all ones and its result is inverted. RFC 3720 appendix B.4 gives test values, which the unit tests check: 32 zero bytes give 0x8A9136AA, and the ASCII bytes `123456789` give 0xE3069283. litbook computes it with the CRC32C instruction of the CPU when there is one (the CRC extension on arm64, SSE4.2 on x86), and with a table otherwise. The two always agree.

### Record types

| Type | Name | Owner | Payload | Meaning |
|---|---|---|---|---|
| `B` | Session Start | 0 | 12 bytes, below | The order entry configuration. Always the first record. |
| `S` | Add Symbol | 0 | 44 bytes, below | One stock listed at startup. |
| `R` | Risk Limits | the account, not 0 | 8 bytes, below | One account's risk limits, set at startup. |
| `F` | Frame | any | the OUCH message, 1 to 47 bytes | One inbound message from the account, exactly as the client sent it, still big-endian. |
| `C` | Cancel All | any | none | Cancel every live order of the account (cancel on disconnect). |
| `E` | Stop | 0 | none | The exchange shut down cleanly here. |
| `A` | Restart | 0 | none | The exchange started again from this journal. Recovery writes it, and its sequence number names the new market data feed session. |

Frame and Cancel All records may carry any owner, because replay must see exactly what the matching thread saw, and the order entry layer itself counts an owner that does not exist. Account names and passwords are never written to the journal. Only the order entry layer's state depends on the journal, and it knows accounts only by number.

### Session Start payload, 12 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Max Owners | 0 | 4 | Integer | At least 1. |
| Max Live Orders | 4 | 4 | Integer | At least 1. |
| Max Symbols | 8 | 4 | Integer | 1 to 65,535 (one per SymbolId). |

### Add Symbol payload, 44 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Symbol Id | 0 | 2 | Short | The SymbolId the engine gave the stock. Replay checks it gets the same one. |
| Symbol | 2 | 8 | Symbol | |
| Self Trade Policy | 10 | 1 | Byte | 0 cancel newest, 1 cancel oldest, 2 cancel both, 3 decrement. |
| Reserved | 11 | 1 | Byte | 0 |
| Lot Size | 12 | 4 | Integer | At least 1. |
| Max Orders | 16 | 4 | Integer | At least 1. |
| Tick Size | 20 | 8 | Signed Long | At least 1. |
| Min Price | 28 | 8 | Signed Long | At most Max Price. |
| Max Price | 36 | 8 | Signed Long | |

### Risk Limits payload, 8 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Max Order Quantity | 0 | 4 | Integer | |
| Max Open Orders | 4 | 4 | Integer | |

### Decoding rules

A reader checks a record in this order and stops at the first rule it breaks.

1. Fewer than 2 bytes are left, or fewer than Record Length: **Incomplete**.
2. Record Length below 28 or above 75: **Bad Length**.
3. The checksum does not match: **Bad Checksum**. It is checked right after the length, so bytes that were only half written, or changed on disk, are reported as what they are.
4. Record Type is not one of the letters above: **Bad Type**.
5. The payload length is wrong for the type: **Bad Length**.
6. Reserved is not 0: **Bad Reserved**.
7. Owner is not allowed for the type: **Bad Owner**.
8. A payload field is out of range (a symbol that is not left-justified and padded, a policy above 3, a tick size, lot size or max orders of 0, a min price above the max price, a configuration of 0 or too many stocks): **Bad Payload**.

Every field is checked, so a record that decodes encodes back into exactly the same bytes. The journal fuzzer checks that property.

## How a journal ends

A reader walks the records from the start. Each one must decode and carry the next sequence number. Where that stops, the journal ends in one of four ways.

- **Clean.** Every byte after the header belongs to a good record.
- **Torn.** The journal ends with bytes that are not a good record, and no good record starts at any later byte offset. A crash in the middle of a write leaves this. Recovery cuts those bytes off and appends after the last good record.
- **Corrupt.** A good record starts somewhere after the bad bytes, or a good record has the wrong sequence number. Something in the middle was lost or damaged. Recovery refuses to start, because cutting there would throw away good records.
- **Bad header.** The file header is missing or wrong, and nothing is read.

The search for a later good record only runs after something went wrong, and after a crash the bad tail is at most one write. A checksum proves only that the bytes were not damaged by accident, not who wrote them. A Frame's payload is whatever the client sent, so it can hold the bytes of a whole valid record. If a crash cuts short exactly such a Frame, the reader finds the record hidden inside it and calls the journal corrupt. That mistake is the safe one: recovery stops and asks for an operator, and nothing good is ever cut off. The journal fuzzer found this case, and it is kept as a regression input.

## Writing

`JournalWriter` appends records to a buffer that is allocated once at startup, so appending never allocates and never makes a system call. `commit()` writes everything appended since the last commit with as few `write()` calls as the kernel allows. One commit for a whole batch of records is group commit. A write that takes only part of the bytes, or is interrupted by a signal, is continued.

| Policy | What `commit()` does | What it survives |
|---|---|---|
| Buffered | Writes only once the buffer is half full. | Nothing. A crash of the process loses what is buffered. For benchmarks only. |
| Write (default) | Calls `write()`. | A crash of the process, including `kill -9`, because the kernel's page cache still writes the data out. Not a crash of the machine or a power loss. |
| Sync | Calls `write()` and then `fdatasync()`. | A power loss on real hardware, because `fdatasync` returns only once the disk has the data. |

A new journal is created with `O_EXCL`, so an existing journal is never overwritten. Under Sync its directory is synced as well, because a new file whose name was never synced can disappear after a power loss. A journal that is continued after recovery is opened with `O_APPEND`, after its torn tail has been cut.

### Segments and rotation

The journal thread starts a new segment once the open one holds a set number of records (`--segment-records`, one million by default). It checks after each batch it forwarded, so a segment can hold a little more than that number, and setup, Stop and Restart records never start one. `JournalWriter::rotate()` flushes the open segment, creates the next one with `O_EXCL` under the name of the next sequence number, closes the old file, and writes the new file's header. Under Sync the header is synced and so is the directory, so the new name survives a power loss. The writer keeps its buffer, so rotating never allocates. A segment that holds no record yet is never rotated, because its successor would have its own name, and `O_EXCL` would refuse it and stop the writer.

A crash while a new segment is created can leave a last file that holds only the start of the header the chain expects next (a **torn rotation**). It holds no record, so recovery deletes it and goes on with the segment before it.

Any failed write or sync stops the writer for good, and every later call is refused. After a failed `fsync` or `fdatasync` this is the only safe choice. Linux may already have dropped the pages it could not write and cleared the error, so a second sync can report success for data that is gone (PostgreSQL found this in 2018 and now stops the server on such an error). The exchange must stop and recover from what is on disk.

### What this machine can prove

The development machine is a Mac, and litbook runs in a Linux virtual machine under Docker. A test that kills the process with `kill -9` proves the Write policy loses nothing in a process crash. It cannot prove survival of a power loss. Inside the virtual machine, `fdatasync` is only as good as the hypervisor's promise to the virtual disk. The repository folder `/work` is shared from macOS, where an ordinary `fsync` does not even flush the drive's own cache. Journals for tests and measurements therefore live on the virtual machine's own disk (`/tmp`), and the Sync numbers in BENCHMARKS.md say which file system they were measured on.

## Snapshots

A snapshot is the whole state of the exchange as of one record, so recovery can start from it and replay only the records after it. It holds what a replay needs and nothing a replay could not rebuild. That is the setup records, each account's reply count (its next sequence number minus 1) and last UserRefNum, the order entry layer's counters, every book's match counter, every live order of every account in the order of that account's list, and every resting order of every book (bids then asks, best price first, and in time priority within a price). A book's memory layout is not in it, since nothing outside the book can see it, so a snapshot taken on one book type restores into any other.

### Snapshot header, 40 bytes

| Field | Offset | Length | Type | Notes |
|---|---|---|---|---|
| Magic | 0 | 8 | ASCII | `LITBOOKS` |
| Version | 8 | 2 | Short | 1 |
| Reserved | 10 | 2 | Short | 0 |
| Accounts | 12 | 4 | Integer | At least 1. |
| As Of | 16 | 8 | Long | The sequence number of the last record the snapshot includes. Its file name says the same. |
| As Of Time | 24 | 8 | Timestamp | That record's timestamp. |
| Body Length | 32 | 4 | Integer | The bytes between the header and the checksum. |
| Reserved | 36 | 4 | Integer | 0 |

### Snapshot body

The body is these parts, one after another. A count is an Integer.

| Part | Layout |
|---|---|
| Setup records | A count, then for each record its type letter (Byte), owner (Integer), payload length (Byte) and payload, as in a journal record. Only Session Start, Add Symbol and Risk Limits, each with the payload length of its type. |
| Replies | One Long per account, accounts 1 to Accounts: how many replies the account has been sent. |
| Last UserRefNum | One Integer per account. |
| Counters | Last Order Ref (Long), Last Match Id (Long), then the five order entry statistics (malformed, bad owner, stale UserRefNum, unknown order, no-effect cancel), a Long each. |
| Books | A count (one per stock, in SymbolId order), then for each book its next match number (Long) and how many orders rest in it (Integer). |
| Live orders | A count, then 35 bytes per order: Order Ref (Long), Owner (Integer), UserRefNum (Integer), SymbolId (Short), Side (Byte, `B` or `S`), Price (Signed Long), Open (Integer), Executed (Integer). Account 1's list first, each list front to back. |
| Resting orders | A count, then 26 bytes per order: Order Id (Long, equal to the live order's Order Ref), Owner (Integer), Side (Byte), Price (Signed Long), Remaining (Integer), Post Only (Byte, 0 or 1). Book by book, bids then asks, best first, time priority within a price. |

A Checksum (Integer), the CRC32C of every byte before it, ends the file.

### Snapshot decoding rules

A reader checks a snapshot in this order and stops at the first rule it breaks.

1. Shorter than a header and a checksum: **Too Short**.
2. Magic is not `LITBOOKS`: **Bad Magic**. Version is not 1: **Bad Version**.
3. The last 4 bytes are not the CRC32C of everything before them: **Bad Checksum**. As in a record, the checksum comes right after the fields that say what the file is, so damage is reported as damage.
4. A reserved field is not 0, or Accounts is 0: **Bad Header**.
5. Body Length is not the file length minus the header and the checksum, a count is larger than the bytes left could hold (checked before anything is allocated for it), or bytes are left over at the end: **Bad Length**.
6. A setup record has the wrong type or payload length: **Bad Setup Record**.
7. A side is not `B` or `S`, or Post Only is not 0 or 1: **Bad Field**.

So a snapshot that decodes encodes back into exactly the same bytes, which the snapshot fuzzer checks. Whether the state itself makes sense is checked when it is restored. The order entry layer refuses a snapshot whose number of accounts or stocks differs from its setup, whose live orders do not each match a resting order (owner, side, price and quantity), whose order refs or UserRefNums were never handed out, or that a book refuses (a price off the tick or out of the band, a quantity that is not a whole lot, an order id that is already there, an order that would cross the other side, a full book). Finally it captures its own state again, which must give back exactly what was restored, so a state in any other order is refused too.

### Writing a snapshot

A snapshot is written to `<N>.snapshot.tmp`, synced under the Sync policy, and renamed to `<N>.snapshot`. Under Sync the directory is synced after the rename. `rename()` replaces the name in one step, so a crash leaves either no snapshot of that number or a whole one, never half of one under its real name. The `.tmp` file it may leave behind is deleted at the next start.

### When snapshots are taken, and what is removed

- At every start, after recovery and before any thread runs.
- At a clean stop, after every thread has finished.
- On the snapshot thread, each time the journal thread closes a segment (with `--snapshot-thread on`, the default). The snapshot thread keeps a second copy of the exchange, rebuilt from the start snapshot. It maps each closed segment, replays it into that copy and writes a snapshot as of the segment's last record, so the matching thread never waits for a snapshot.
- On demand, with `litbook_journal snapshot DIR`.

After each snapshot the exchange keeps the newest `--keep-snapshots` usable snapshots (2 by default) and removes older ones, and then removes every segment whose records are all in the oldest snapshot it kept. Snapshots are removed before segments, so a crash in between never leaves a snapshot without the segments after it. A snapshot that cannot be used (damaged, or not followed by the segments) is never removed, so an operator can look at it. `--keep-snapshots 0` turns snapshots off and nothing is ever removed.

## Replay

`Replayer<Book>` applies records in order. Session Start builds the order entry layer, Add Symbol and Risk Limits make its startup calls, a Frame goes to `on_frame` and a Cancel All to `cancel_all` with the record's timestamp, exactly as `MatchingWorker` calls them. A Stop and a Restart are only counted, because an exchange that stopped cleanly or crashed may have been started again and kept writing. Replay refuses a journal whose first record is not Session Start, that has a second Session Start, that lists a stock or a risk limit after the first request, or whose stocks get different SymbolIds than they did when they were listed. `start_from(snapshot)` starts a replay from a snapshot instead. It applies the snapshot's setup records, restores its state, and from then on skips every record the snapshot already holds, so whole segments that start before the snapshot can be given to it. No setup record may follow.

"The same state" is checked by the tests in four ways. The replies to every account are the same bytes in the same order (timestamps included). The trades and level updates are the same, at the same times. Every book has the same levels, quantities and order counts, and the order entry layer has the same live orders, UserRefNums, open order counts and statistics. And both handle any further requests the same way. The memory layout inside a book may differ (FastOrderBook's pool can hand out different slots), and nothing outside the book can tell. Every account's next sequence number is 1 plus its replies, so recovery can rebuild it from a replay.

## In the pipeline

With a journal, the threaded exchange has a fourth thread between the gateway thread and the matching thread (D-061).

```
gateway thread -> requests -> journal thread -> journaled requests -> matching thread
```

The journal thread takes every request that is waiting, at most 64 at a time, appends them all and writes them with one `commit()`. Only then does it pass them on to the matching thread. So no reply and no market data can leave the exchange before the request that caused it is in the journal. A Stop is flushed before it is passed on, so a clean shutdown always ends the journal with a Stop record.

If a write or a sync fails, the batch is not passed on, the exchange ends the session for every client, and stopping it reports the error. The requests of a failed batch may still be in the file in full, and then recovery replays them, although their clients never got an answer. Once a request is in the journal, it has happened.

## Recovery

`litbook_gateway --journal DIR` starts from the journal when the directory holds one, and creates the directory if it does not exist. Before any thread runs, it

1. deletes half written snapshots (`.snapshot.tmp`), scans every segment, and refuses to start if a segment has a bad header or is corrupt, if a segment's name disagrees with its header, if a segment does not start where the one before it ended, or if a segment other than the last ends torn. A torn rotation at the end is deleted;
2. reads the snapshots, newest first. A snapshot written with other setup records than the command line gives refuses the start, naming the first difference. A damaged snapshot, or one the segments do not continue, is skipped and counted, and recovery falls back to the one before it;
3. without a snapshot, compares the setup records at the start of the journal with the command line's, and refuses to start if they differ, or if the first segment does not start at record 1;
4. restores the newest usable snapshot into the matching thread's order entry layer (its reply counts too, and the feed's copy of the books from the restored books), or starts from nothing;
5. replays every later record into it, counting the replies of each account and giving the level updates to the market data feed's copy of the books, without sending anything;
6. cuts off a torn last record and resumes the last segment;
7. writes a Restart record and a Cancel All for every account with cancel on disconnect, flushes them, and then applies the cancels;
8. sets every account's next sequence number to 1 plus its replies, and names the feed's new session after the Restart record's sequence number;
9. writes the start snapshot and removes what it makes unnecessary.

Then it starts the threads and keeps appending to the last segment. A directory without segments is a new journal, unless it holds snapshots, which refuses the start. A segment that holds only the 32-byte header counts as no record. A journal that ends inside its setup records (a crash during the very first start) gets the missing ones.

Messages that were produced for an account but never reached it are not sent again. The account's next sequence number skips past them, and a login that asks for an older number is refused, as before.

The crash tests (`GatewayJournalCrashTest`) kill `litbook_gateway` with SIGKILL at random points and start it again on the same journal each time. After every kill they replay the journal and check that every message any client ever saw is there, with the same sequence number and the same bytes. Linux can cut a large `write()` short when the process is killed (the copy into the page cache stops between pages), so a torn last record is possible and is accepted. A corrupt journal never is. The second test uses segments of 16 records, so kills also land while segments rotate and snapshots are written. There it also checks after every kill that every snapshot file is whole and continued by the segments, and that replaying the later records from each one ends in exactly the state and reply counts of a full replay. For its last rounds it keeps only 2 snapshots, so old files are also removed under the kills.

## What it costs

Measured on the development machine (an Apple M3 Pro running the Linux VM under Docker, files on the VM's own disk), with the rows, dates and conditions in BENCHMARKS.md (2026-11-23). These runs were made with a browser open, so the medians are the numbers to trust.

| What | Cost |
|---|---|
| CRC32C | 2.2 ns per byte with the table, 0.11 ns per byte with the arm64 instruction |
| Appending one record (encode and CRC, no system call) | 12.7 ns |
| Commit under Write | 289 ns for one record, 25.6 ns per record in batches of 64 |
| Commit under Sync | 368 us for one record (the VM's `fdatasync`), 7.5 us per record in batches of 64 |
| Rotation | 5.4 us under Write, 0.5 ms under Sync |
| Replay | 108.5 ns per record on FastOrderBook, 119.5 on SortedOrderBook, 149.7 on MapOrderBook |
| Snapshot of about 100,000 live orders (6.3 MB) | capture 2.0 ms, encode 1.4 ms, decode 3.1 ms, restore 18.3 ms |
| Snapshot thread, per segment of 100,000 records | about 14 ms (about 6,900 live orders) |
| Round trip through the pipeline, p50 | about +3 us in spin mode and +18 us in blocking mode with the Write journal; about 0.3 ms with Sync |

Recovery time (`litbook_journal_recovery`, warm page cache, about 6,900 live orders):

| Requests in the journal | Without a snapshot | With the snapshot thread's snapshots |
|---|---|---|
| 150,000 | 28.9 ms | 18.9 ms |
| 550,000 | 84.1 ms | 19.7 ms |
| 1,050,000 | 147.6 ms | 18.5 ms |
| 2,050,000 | 282.2 ms | 19.0 ms |
| 4,050,000 | 551.3 ms | 18.6 ms |

Without a snapshot recovery replays everything, about 136 ns per request. With snapshots it replays only the open segment (here 50,000 requests) after decoding the newest snapshot, so it does not grow with the journal.

## Tool

```
litbook_journal verify PATH                 # how it ends, record counts, segments, snapshots, where recovery starts
litbook_journal dump PATH [--limit N]       # one line per record
litbook_journal replay PATH [--from-start]  # rebuilds the state on SortedOrderBook and prints digests
litbook_journal snapshot DIR                # writes a snapshot as of the last good record
```

PATH is a journal directory or a single segment file. For a directory, `replay` starts from the newest usable snapshot as recovery does, or from record 1 with `--from-start`, and `snapshot` writes through a temporary file that is synced and renamed, and never removes anything. Exit codes are 0 for a clean journal, 1 for a torn tail or a torn rotation, 3 for a corrupt segment, a bad header, a broken chain, a journal that lacks its first records and has no usable snapshot, or a replay that fails, and 2 for bad arguments or a file that cannot be read. `replay` prints a CRC32C digest of every reply and of every market event it produced (the exact byte order is in the comment at the top of `tools/journal_main.cpp`), so two replays can be compared line by line. A replay from a snapshot only produces what follows the snapshot, so its digests cover only that, while the next sequence numbers it prints include the snapshot's replies.
