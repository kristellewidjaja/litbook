# litbook order entry protocol

This is the binary protocol clients use to enter, replace and cancel orders, and that the exchange uses to report what happened to them. It is modeled on Nasdaq OUCH 5.0 (specification revision of October 2026). It keeps OUCH's message type letters, field names, field encodings, byte order and session rules, and drops the fields and options that litbook does not implement. Every difference is listed in [Differences from OUCH 5.0](#differences-from-ouch-50).

The codec lives in `include/litbook/protocol/codec.hpp` and `src/protocol/codec.cpp`. The session rules are implemented by the order entry layer in `include/litbook/protocol/order_entry.hpp`.

## Transport and framing

Each message is carried in one frame, and the transport says where a frame starts and ends. In Phase 6 that is a 2-byte big-endian length prefix in front of each message, in the style of SoupBinTCP. The codec only ever sees one whole frame.

Every message has a fixed length. A frame whose length is not exactly the length of its message type is malformed.

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
| `Z` | System. The exchange removed it because the book had no room left for another resting order. |

### Reject reasons

Codes below 0x0100 are OUCH's own codes. Codes from 0x0100 up are litbook extensions for rules OUCH has no code for.

| Code | Meaning |
|---|---|
| 0x000F | Other. |
| 0x0013 | Invalid quantity (zero, or not a multiple of the lot size). |
| 0x0017 | Invalid symbol (the stock is not listed). |
| 0x001D | Invalid price (off the tick, or outside the stock's price band). |
| 0x0101 | Post only would cross (a post-only order would trade on arrival). |
| 0x0102 | Invalid order type (the fields contradict each other, such as a post-only market order). |

## Malformed versus rejected

There are two kinds of bad input, and they get different answers.

A **malformed** frame has bytes that are wrong in themselves. The decoder refuses it with a `DecodeStatus` and nothing reaches the engine. The cases are an empty frame, an unknown type byte, a frame shorter or longer than its type, a side, time in force, order type or flag byte outside its listed values, a price above the maximum, and a symbol that is not left-justified and space-padded. Fields are checked in wire order, so the status names the first bad field. A malformed frame means the client is broken or out of sync, so in Phase 6 the gateway will end the session.

A **rejected** order is well formed but breaks a trading rule, such as a zero quantity, a price off the tick, a market order with a price, or an unlisted stock. The exchange answers with a Rejected message and the session continues.

In inbound messages every enumerated field is checked strictly. In outbound messages the Reason codes and the Liquidity Flag are open sets, so a client decoder accepts any byte value there. That way a client keeps working if the exchange adds a new code. Every frame that decodes re-encodes to exactly the same bytes, so two different frames never mean the same message. The `protocol_fuzzer` target checks this property on random input.

## Session rules

These rules belong to the order entry layer, not the codec.

1. **Accounts.** Each client account (litbook's `OwnerId`) has its own UserRefNum sequence. In Phase 6 the account is the session the message arrived on.
2. **UserRefNum.** An Enter Order or Replace Order whose new UserRefNum is not higher than the last one that account used is silently ignored, as in OUCH, because it is treated as a retransmission. This includes 0, since numbering starts at 1. A UserRefNum is used up once a message with it is processed, whether the order was accepted or rejected.
3. **Enter Order.** An unlisted symbol gets Rejected (0x0017). Otherwise the order goes to the stock's book. A rule violation gets Rejected, and an accepted order gets Order Accepted with a fresh Order Reference Number. Executions and cancels that happen on arrival follow the Accepted message.
4. **Executions.** Both sides of a trade get Order Executed with the same Match Number. The resting side's Liquidity Flag is `A` and the incoming side's is `R`.
5. **Cancel Order.** A Cancel for a UserRefNum that is not a live order of that account is silently ignored, as in OUCH. Otherwise the open quantity becomes the intended size minus the shares already executed (never below 0). If that is 0 the order is canceled. If it is smaller than the current open quantity the order shrinks in place and keeps its priority. An intended size that would not reduce the order is ignored. The answer is Order Canceled with the number of shares taken off.
6. **Replace Order.** A Replace whose OrigUserRefNum is not a live order of that account, or whose new UserRefNum breaks rule 2, is silently ignored. Otherwise the new open quantity is the Quantity field minus the shares already executed. If that would be 0 or less, or the book refuses the new price or size, the replace fails. A failed replace gets Rejected for the new UserRefNum, and then the original order is canceled with Order Canceled (reason `U`), as OUCH does with an invalid replace. A successful replace gets Order Replaced and the order is known by its new UserRefNum from then on. The order keeps its priority only if the price is unchanged and the size does not grow (D-021). Otherwise it goes to the back of the queue and may trade at once, and those executions carry the new UserRefNum.
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
| Other messages | Modify Order, Mass Cancel, Disable and Enable Order Entry, Account Query, System Event, Broken Trade, Cancel Pending, Cancel Reject, AIQ Canceled, Order Priority Update, Order Modified, Order Restated and others. | Not implemented. |
