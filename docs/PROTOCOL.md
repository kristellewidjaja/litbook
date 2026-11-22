# litbook order entry protocol

This is the binary protocol clients use to enter, replace and cancel orders, and that the exchange uses to report what happened to them. It is modeled on Nasdaq OUCH 5.0 (specification revision of October 2026). It keeps OUCH's message type letters, field names, field encodings, byte order and session rules, and drops the fields and options that litbook does not implement. Every difference is listed in [Differences from OUCH 5.0](#differences-from-ouch-50).

The codec lives in `include/litbook/protocol/codec.hpp` and `src/protocol/codec.cpp`. The session rules are implemented by the order entry layer in `include/litbook/protocol/order_entry.hpp`. The transport is SoupBinTCP 3.0, implemented in `include/litbook/gateway/soup.hpp` and `include/litbook/gateway/session_manager.hpp`.

The market data feed, which tells everyone what the books look like, is described separately in [FEED.md](FEED.md).

## Transport: SoupBinTCP 3.0

Messages travel over TCP inside SoupBinTCP 3.0 packets, as OUCH does at Nasdaq. This section follows Nasdaq's SoupBinTCP specification (version 3.00) and marks every rule litbook adds where the specification leaves a choice open.

### Packets

Every packet starts with a 2-byte big-endian Packet Length that counts the bytes after it, which are a 1-byte Packet Type and then the payload. TCP may split a packet across reads or join several packets in one read, so the receiver collects bytes until a whole packet is there.

| Type | Sent by | Payload |
|---|---|---|
| `+` Debug | either side | Free text. The receiver ignores it. |
| `L` Login Request | client | Username (6, padded on the right with spaces), Password (10, padded on the right), Requested Session (10, all spaces for the current session), Requested Sequence Number (20, ASCII digits). 46 bytes. |
| `A` Login Accepted | server | Session (10, padded on the left with spaces), Sequence Number (20, ASCII digits padded on the left), the number the next Sequenced Data packet will have. 30 bytes. |
| `J` Login Rejected | server | Reject Reason Code (1). `A` not authorized, `S` session not available. |
| `U` Unsequenced Data | client | One inbound order entry message. |
| `S` Sequenced Data | server | One outbound order entry message. |
| `R` Client Heartbeat | client | None. |
| `H` Server Heartbeat | server | None. |
| `O` Logout Request | client | None. |
| `Z` End of Session | server | None. |

The order entry message inside a Unsequenced Data or Sequenced Data packet is exactly one message, so the codec only ever sees one whole frame. Every message has a fixed length, and a frame whose length is not exactly the length of its message type is malformed.

### Sequence numbers

Sequence numbers are never sent inside Sequenced Data packets. Both sides count them, starting at 1 for each account's first sequenced message of the session. litbook keeps one sequence per account, and it continues across connections. Every outbound message produced for an account uses up the next number, even when the account has no connection that can take the message (for example the cancels sent by cancel on disconnect). Such a message is lost, because litbook does not send old messages again yet.

When `litbook_gateway` runs with a journal (docs/JOURNAL.md), the sequence numbers also survive a restart. The exchange rebuilds itself from the journal and gives every account the next number it would have had, 1 plus every message produced for it before the restart, so Login Accepted after a crash names the number after the last message the journal explains. Messages that were produced but never reached the client before the crash are skipped, like the ones above.

### Session rules of the litbook gateway

1. **Login first.** The first packet on a new connection must be a Login Request. Debug packets are ignored at any time. Any other packet before a successful login closes the connection.
2. **Accounts.** Username and password are compared without regard to case. An unknown username or a wrong password gets Login Rejected `A`, and the connection is closed.
3. **When a login is refused with `S`.** The session has ended, or the requested session is not blank and not the current one, or the account is already logged in on another connection (litbook allows one connection per account), or the requested sequence number is above 0 and below the account's next number (sending old messages again is not supported yet). The connection is closed after Login Rejected.
4. **Sequence number ahead.** A requested sequence number above the account's next number is accepted, and Login Accepted tells the client the real next number. The specification says this case is handled but does not say how, so this is litbook's choice.
5. **Packet length limit.** The specification sets no maximum length. litbook closes a connection whose Packet Length is 0 or above 47 (the size of a Login Request, the largest packet a client needs), as soon as the two length bytes arrive. A long Debug packet therefore also closes the connection.
6. **Malformed input.** A Login Request with the wrong length or a sequence field that is not a number, a second Login Request on a logged in connection, a packet type that a client may not send, and an order entry message that does not decode all close the connection with no reply.
7. **Heartbeats and timeouts.** The server sends a Server Heartbeat to a logged in client when it sent nothing else for 1 second. A logged in client that sends nothing for 15 seconds is disconnected, and so is a connection that has not logged in after 30 seconds. The three times are configurable.
8. **Logout.** A Logout Request closes the connection at once.
9. **Backpressure and slow clients.** If a client does not read its replies, the server stops processing that client's input once 32 KiB of replies are waiting (configurable), and TCP flow control then slows the client down. Messages that cannot be held back, such as executions caused by other accounts' orders, still go into the client's 64 KiB output buffer. A client whose output buffer fills up is disconnected.
10. **Cancel on disconnect.** An account can be configured to have all of its live orders canceled when its connection goes away for any reason. Each order gets Order Canceled with reason `Z`, which uses up sequence numbers as described above. A crash takes every connection away too, so when the exchange starts again from its journal it cancels the orders of these accounts before it accepts any login.
11. **End of session.** When the exchange ends the session, every logged in client gets End of Session and every connection is closed. Later logins get Login Rejected `S`.

## Data types

| Type | Width | Encoding |
|---|---|---|
| Byte | 1 | A single ASCII code, such as a message type or a side. |
| Integer | 4 | Unsigned, big-endian. |
| Short | 2 | Unsigned, big-endian. |
| Long | 8 | Unsigned, big-endian. |
| Price | 8 | Unsigned, big-endian, 4 implied decimals. 1,000,000 is $100.0000. The largest legal price is 1,999,999,900 ($199,999.9900), as in OUCH. |
| Timestamp | 8 | Long. Nanoseconds since midnight. |
| UserRefNum | 4 | Integer. The client's number for an order (see [session rules](#session-rules)). |
| Symbol | 8 | ASCII, left-justified, padded on the right with spaces. 1 to 8 printable characters (codes 0x21 to 0x7E) with no space inside the name. |

Prices are in the same unit the books use, so the engine never converts them and there is no floating point anywhere on the path.

## Inbound messages (client to exchange)

### Enter Order, type `O`, 29 bytes

| Offset | Length | Field | Values |
|---|---|---|---|
| 0 | 1 | Type | `O` |
| 1 | 4 | UserRefNum | Must be higher than every UserRefNum this account used before. |
| 5 | 1 | Side | `B` buy, `S` sell |
| 6 | 4 | Quantity | Shares. Must be at least 1 and a multiple of the stock's lot size. |
| 10 | 8 | Symbol | Must be a stock the exchange lists. |
| 18 | 8 | Price | Limit price. Must be 0 for a market order. |
| 26 | 1 | Time In Force | `0` Day, `3` IOC, `4` FOK (FIX tag 59 codes) |
| 27 | 1 | Order Type | `L` limit, `M` market |
| 28 | 1 | Post Only | `Y` or `N` |

### Replace Order, type `U`, 21 bytes

| Offset | Length | Field | Values |
|---|---|---|---|
| 0 | 1 | Type | `U` |
| 1 | 4 | OrigUserRefNum | The UserRefNum of the live order, from its Enter Order or its last Replace Order. |
| 5 | 4 | UserRefNum | The new UserRefNum for the order. Same rule as in Enter Order. |
| 9 | 4 | Quantity | Total shares the order may ever execute, including shares that already executed. |
| 13 | 8 | Price | The new limit price. |

### Cancel Order, type `X`, 9 bytes

| Offset | Length | Field | Values |
|---|---|---|---|
| 0 | 1 | Type | `X` |
| 1 | 4 | UserRefNum | The order to cancel or reduce. |
| 5 | 4 | Quantity | The new intended order size, meaning the most shares the order may execute in total after the cancel, counting shares that already executed. 0 cancels everything still open. |

## Outbound messages (exchange to client)

Every outbound message starts with the type byte and an 8-byte Timestamp at offset 1.

### Order Accepted, type `A`, 45 bytes

| Offset | Length | Field | Values |
|---|---|---|---|
| 0 | 1 | Type | `A` |
| 1 | 8 | Timestamp | |
| 9 | 4 | UserRefNum | As entered. |
| 13 | 1 | Side | As entered. |
| 14 | 4 | Quantity | Shares accepted. |
| 18 | 8 | Symbol | As entered. |
| 26 | 8 | Price | As entered. 0 for a market order. |
| 34 | 1 | Time In Force | As entered. |
| 35 | 1 | Order Type | As entered. |
| 36 | 1 | Post Only | As entered. |
| 37 | 8 | Order Reference Number | The exchange's number for the order, unique for the day across all accounts and stocks. |

### Order Replaced, type `U`, 47 bytes

| Offset | Length | Field | Values |
|---|---|---|---|
| 0 | 1 | Type | `U` |
| 1 | 8 | Timestamp | |
| 9 | 4 | OrigUserRefNum | The order that was replaced. |
| 13 | 4 | UserRefNum | The order's new UserRefNum. |
| 17 | 1 | Side | The order's side. |
| 18 | 4 | Quantity | Shares open after the replace. |
| 22 | 8 | Symbol | The order's stock. |
| 30 | 8 | Price | The new price. |
| 38 | 8 | Order Reference Number | Unchanged by a replace. |
| 46 | 1 | Priority Kept | `Y` if the order kept its place in the queue, `N` if it went to the back. |

### Order Canceled, type `C`, 18 bytes

| Offset | Length | Field | Values |
|---|---|---|---|
| 0 | 1 | Type | `C` |
| 1 | 8 | Timestamp | |
| 9 | 4 | UserRefNum | The order that was reduced or canceled. |
| 13 | 4 | Quantity | Shares taken off by this cancel only. This is an increment, not a running total, and the order may still be partly open. |
| 17 | 1 | Reason | See [cancel reasons](#cancel-reasons). |

### Order Executed, type `E`, 34 bytes

| Offset | Length | Field | Values |
|---|---|---|---|
| 0 | 1 | Type | `E` |
| 1 | 8 | Timestamp | |
| 9 | 4 | UserRefNum | The order that executed. |
| 13 | 4 | Quantity | Shares in this execution only. |
| 17 | 8 | Price | The execution price, which is the resting order's price. |
| 25 | 1 | Liquidity Flag | `A` added (the order was resting), `R` removed (the order was the incoming one). |
| 26 | 8 | Match Number | Unique for the day across all stocks. Both sides of one trade get the same number. |

### Rejected, type `J`, 15 bytes

| Offset | Length | Field | Values |
|---|---|---|---|
| 0 | 1 | Type | `J` |
| 1 | 8 | Timestamp | |
| 9 | 4 | UserRefNum | The Enter Order or Replace Order that was refused. |
| 13 | 2 | Reason | See [reject reasons](#reject-reasons). |

## Codes

### Cancel reasons

These are OUCH's letters.

| Code | Meaning |
|---|---|
| `U` | User requested. The answer to a Cancel Order. |
| `I` | Immediate or cancel. The part of an IOC, FOK or market order that could not trade on arrival. |
| `Q` | Self match prevention. The order would have traded with an order from the same account. |
| `Z` | System. The exchange removed it, because the book had no room left for another resting order or because the account's connection went away and the account asked for cancel on disconnect. |

### Reject reasons

Codes below 0x0100 are OUCH's own codes. Codes from 0x0100 up are litbook extensions for rules OUCH has no code for.

| Code | Meaning |
|---|---|
| 0x000F | Other. |
| 0x0013 | Invalid quantity (zero, or not a multiple of the lot size). |
| 0x0017 | Invalid symbol (the stock is not listed). |
| 0x001D | Invalid price (off the tick, or outside the stock's price band). |
| 0x0031 | Risk: Max Quantity Exceeded (the Quantity is above the account's limit). |
| 0x0101 | Post only would cross (a post-only order would trade on arrival). |
| 0x0102 | Invalid order type (the fields contradict each other, such as a post-only market order). |
| 0x0103 | Max open orders exceeded (the account already has its limit of live orders). OUCH has no code for this limit. |

## Malformed versus rejected

There are two kinds of bad input, and they get different answers.

A **malformed** frame has bytes that are wrong in themselves. The decoder refuses it with a `DecodeStatus` and nothing reaches the engine. The cases are an empty frame, an unknown type byte, a frame shorter or longer than its type, a side, time in force, order type or flag byte outside its listed values, a price above the maximum, and a symbol that is not left-justified and space-padded. Fields are checked in wire order, so the status names the first bad field. A malformed frame means the client is broken or out of sync, so in Phase 6 the gateway will end the session.

A **rejected** order is well formed but breaks a trading rule, such as a zero quantity, a price off the tick, a market order with a price, or an unlisted stock. The exchange answers with a Rejected message and the session continues.

In inbound messages every enumerated field is checked strictly. In outbound messages the Reason codes and the Liquidity Flag are open sets, so a client decoder accepts any byte value there. That way a client keeps working if the exchange adds a new code. Every frame that decodes re-encodes to exactly the same bytes, so two different frames never mean the same message. The `protocol_fuzzer` target checks this property on random input.

## Session rules

These rules belong to the order entry layer, not the codec.

1. **Accounts.** Each client account (litbook's `OwnerId`) has its own UserRefNum sequence. The account is the one the connection logged in as.
2. **UserRefNum.** An Enter Order or Replace Order whose new UserRefNum is not higher than the last one that account used is silently ignored, as in OUCH, because it is treated as a retransmission. This includes 0, since numbering starts at 1. A UserRefNum is used up once a message with it is processed, whether the order was accepted or rejected.
3. **Enter Order.** An unlisted symbol gets Rejected (0x0017). Then come the account's pre-trade risk checks. A Quantity above its limit gets Rejected (0x0031), and so does an order that would give the account more live orders than its limit (0x0103). Otherwise the order goes to the stock's book. A rule violation gets Rejected, and an accepted order gets Order Accepted with a fresh Order Reference Number. Executions and cancels that happen on arrival follow the Accepted message.
4. **Executions.** Both sides of a trade get Order Executed with the same Match Number. The resting side's Liquidity Flag is `A` and the incoming side's is `R`.
5. **Cancel Order.** A Cancel for a UserRefNum that is not a live order of that account is silently ignored, as in OUCH. Otherwise the open quantity becomes the intended size minus the shares already executed (never below 0). If that is 0 the order is canceled. If it is smaller than the current open quantity the order shrinks in place and keeps its priority. An intended size that would not reduce the order is ignored. The answer is Order Canceled with the number of shares taken off.
6. **Replace Order.** A Replace whose OrigUserRefNum is not a live order of that account, or whose new UserRefNum breaks rule 2, is silently ignored. Otherwise the new open quantity is the Quantity field minus the shares already executed. If that would be 0 or less, if the Quantity field is above the account's size limit (0x0031), or if the book refuses the new price or size, the replace fails. A failed replace gets Rejected for the new UserRefNum, and then the original order is canceled with Order Canceled (reason `U`), as OUCH does with an invalid replace. A successful replace gets Order Replaced and the order is known by its new UserRefNum from then on. The order keeps its priority only if the price is unchanged and the size does not grow (D-021). Otherwise it goes to the back of the queue and may trade at once, and those executions carry the new UserRefNum.
7. **Timestamps.** The caller of the order entry layer supplies the timestamp of each inbound message, and every outbound message caused by it carries that time. Replaying the same inputs with the same timestamps therefore gives the same output, byte for byte, which the journal in Phase 9 relies on.

## Differences from OUCH 5.0

| Area | OUCH 5.0 | litbook |
|---|---|---|
| Optional fields | Every message ends with a 2-byte Appendage Length and a list of TagValue options. | No appendage. Every message has a fixed length. |
| Order type | Market orders use special prices (0x7FFFFFFF or $200,000.0000), and order types come from the PriceType option. | An explicit Order Type byte, `L` or `M`. |
| Post only | The PostOnly appendage option. | A fixed Post Only byte. |
| Time in force | `0` Day, `3` IOC, `5` GTX, `6` GTT, `E` after hours. | `0` Day, `3` IOC and `4` FOK (the FIX tag 59 code). There is no session close, so a Day order rests until it is filled or canceled. |
| Side | `B`, `S`, `T` short, `E` short exempt. | `B` and `S` only. |
| Display, Capacity, InterMarket Sweep, CrossType, ClOrdID | Fields in Enter Order and in the replies. | Not present. The UserRefNum is the only client identifier. |
| Replace | Can also change time in force, display and other options. | Changes price and quantity only. |
| Order Accepted | Also has Display, Capacity, CrossType, Order State and ClOrdID, and the Order Reference Number sits at offset 36. | Has Order Type and Post Only, and the Order Reference Number sits at offset 37 (see the table). |
| Order Replaced | Has its own Order Reference Number layout and no priority field. Priority changes are reported in a separate Order Priority Update message. | Has a Priority Kept byte. |
| Invalid replace | The original order is canceled. | The same, and a Rejected message for the new UserRefNum comes first, so the client learns why. |
| Reject reasons | OUCH codes. | The OUCH codes listed above plus extensions from 0x0100. |
| Session layer | SoupBinTCP 3.0. A client can log in again with an older sequence number and get every message it missed. | SoupBinTCP 3.0 with the limits in [the gateway's session rules](#session-rules-of-the-litbook-gateway). Missed messages cannot be requested again until the Phase 9 journal. |
| Other messages | Modify Order, Mass Cancel, Disable and Enable Order Entry, Account Query, System Event, Broken Trade, Cancel Pending, Cancel Reject, AIQ Canceled, Order Priority Update, Order Modified, Order Restated and others. | Not implemented. |
