#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace litbook::protocol {

// Counts of inbound messages that were dropped without any outbound message. OUCH drops
// these silently, and the counters make them visible to tests and operators.
struct OrderEntryStats {
    std::uint64_t malformed = 0;         // The frame did not decode.
    std::uint64_t bad_owner = 0;         // The owner was 0 or above max_owners.
    std::uint64_t stale_user_ref = 0;    // The new UserRefNum was not above the last one.
    std::uint64_t unknown_order = 0;     // A cancel or replace named no live order.
    std::uint64_t no_effect_cancel = 0;  // A cancel would not reduce the order.
    bool operator==(const OrderEntryStats&) const = default;
};

// One live order as the order entry layer knows it. The types are the plain integers
// behind OrderRef and UserRefNum (order_entry.hpp checks that they match), so a snapshot
// file can use this header without pulling in every protocol message.
struct LiveOrderState {
    std::uint64_t ref = 0;  // The Order Reference Number, also the order's ID in its book.
    OwnerId owner = kNoOwner;
    std::uint32_t user_ref = 0;
    std::uint16_t symbol = 0;  // The SymbolId of its book.
    Side side = Side::Buy;
    Price price = 0;
    Qty open = 0;      // Shares still resting in the book.
    Qty executed = 0;  // Shares executed so far, over the order's whole life.
    bool operator==(const LiveOrderState&) const = default;
};

// What one book adds to a snapshot besides its orders.
struct BookState {
    MatchId next_match_id = 1;      // The number the book itself would give its next trade.
    std::uint32_t order_count = 0;  // How many entries of OrderEntryState::resting are its.
    bool operator==(const BookState&) const = default;
};

// Everything that makes the order entry layer and its books behave the way they do, and
// nothing about where things happen to sit in memory. OrderEntry::capture() fills it and
// OrderEntry::restore() rebuilds a fresh order entry layer from it, and a snapshot file is
// this struct written out.
//
// Invariants (restore() checks them all).
//   1. last_user_ref has max_owners + 1 entries, and entry 0 is unused (always 0).
//   2. orders lists every account's live orders, account 1 first, each account's orders in
//      the order of its list (newest first). That order is visible, because cancel_all()
//      walks the list and sends its cancels in that order.
//   3. books has one entry per stock, by SymbolId. resting holds every book's resting
//      orders one book after another, in that order, each book's bids before its asks and
//      each side best price first in priority order (what for_each_resting visits).
//   4. Every live order rests in exactly one book, with the same owner, side and price and
//      remaining == open, so orders and resting have the same length.
struct OrderEntryState {
    std::uint64_t last_order_ref = 0;  // The last Order Reference Number handed out.
    MatchId last_match_id = 0;         // The engine's last match number (day unique).
    OrderEntryStats stats;
    std::vector<std::uint32_t> last_user_ref;  // Indexed by OwnerId.
    std::vector<LiveOrderState> orders;
    std::vector<BookState> books;  // Indexed by SymbolId.
    std::vector<RestingOrder> resting;
    bool operator==(const OrderEntryState&) const = default;
};

}  // namespace litbook::protocol
