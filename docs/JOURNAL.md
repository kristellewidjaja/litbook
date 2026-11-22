# litbook journal

The journal is a write-ahead log of every inbound request the matching engine handles. Each request is written to the journal before it is handled, so after a crash the exchange can be rebuilt by replaying the journal from the start. The matching thread reads no clock and nothing else outside its requests (D-052, D-055), so replaying the same records gives the same books, the same replies byte for byte and the same market data feed.

The format is litbook's own. It is a local file that only litbook reads, so it is not modeled on an exchange specification. The record layout (a length, a type, the data and a CRC32C) is close to the write-ahead log of LevelDB. The code lives in `include/litbook/journal/` and `src/journal/`, and the tool `litbook_journal` checks, prints and replays a journal file.

## File layout

A journal file is one 32-byte file header followed by records, one after another with nothing in between. A journal is only ever appended to.

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

Any failed write or sync stops the writer for good, and every later call is refused. After a failed `fsync` or `fdatasync` this is the only safe choice. Linux may already have dropped the pages it could not write and cleared the error, so a second sync can report success for data that is gone (PostgreSQL found this in 2018 and now stops the server on such an error). The exchange must stop and recover from what is on disk.

### What this machine can prove

The development machine is a Mac, and litbook runs in a Linux virtual machine under Docker. A test that kills the process with `kill -9` proves the Write policy loses nothing in a process crash. It cannot prove survival of a power loss. Inside the virtual machine, `fdatasync` is only as good as the hypervisor's promise to the virtual disk. The repository folder `/work` is shared from macOS, where an ordinary `fsync` does not even flush the drive's own cache. Journals for tests and measurements therefore live on the virtual machine's own disk (`/tmp`), and the Sync numbers in BENCHMARKS.md say which file system they were measured on.

## Replay

`Replayer<Book>` applies records in order. Session Start builds the order entry layer, Add Symbol and Risk Limits make its startup calls, a Frame goes to `on_frame` and a Cancel All to `cancel_all` with the record's timestamp, exactly as `MatchingWorker` calls them. A Stop is only counted, because an exchange that stopped cleanly may have been started again and kept writing. Replay refuses a journal whose first record is not Session Start, that has a second Session Start, that lists a stock or a risk limit after the first request, or whose stocks get different SymbolIds than they did when they were listed.

"The same state" is checked by the tests in four ways. The replies to every account are the same bytes in the same order (timestamps included). The trades and level updates are the same, at the same times. Every book has the same levels, quantities and order counts, and the order entry layer has the same live orders, UserRefNums, open order counts and statistics. And both handle any further requests the same way. The memory layout inside a book may differ (FastOrderBook's pool can hand out different slots), and nothing outside the book can tell. Every account's next sequence number is 1 plus its replies, so recovery can rebuild it from a replay.

## Tool

```
litbook_journal verify FILE            # how the file ends, record counts, sequence range
litbook_journal dump FILE [--limit N]  # one line per record
litbook_journal replay FILE            # rebuilds the state on SortedOrderBook and prints digests
```

Exit codes are 0 for a clean journal, 1 for a torn tail, 3 for a corrupt journal, a bad header or a replay that fails, and 2 for bad arguments or a file that cannot be read. `replay` prints a CRC32C digest of every reply and of every market event (the exact byte order is in the comment at the top of `tools/journal_main.cpp`), so two replays can be compared line by line.
