// Fuzz target for snapshot files.
//
// The first input byte picks one of two modes with its lowest bit.
//
// Mode 0 treats the rest of the input as a snapshot file. decode_snapshot must never
// crash or read outside the bytes (the sanitizers catch that). A file that decodes must
// encode back to exactly its own bytes, so decoding is canonical. If its session is small
// enough, the snapshot is also restored into a fresh exchange. The restore may refuse it,
// but if it accepts it, every book must keep its invariants, capturing the state again
// must give back the snapshot's state, and a Cancel All for every account must work.
//
// Mode 1 runs requests built from the input on a live exchange, takes a snapshot of it
// after some of them, and then may damage the snapshot by cutting it short or flipping
// one byte. A cut snapshot must be refused, by its length or its checksum. A flipped byte must be
// found by the magic, the version or the checksum. An undamaged snapshot must decode to exactly
// what was encoded, restore into FastOrderBook and SortedOrderBook exchanges, and then
// give exactly the live exchange's replies and state for the rest of the requests.
// The target aborts if any check fails.

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/journal/snapshot_format.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/protocol/order_entry_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fuzz_target.hpp"
#include "journal_fuzz_support.hpp"

namespace litbook::fuzz {

namespace {

using namespace litbook::journal;

// Keeps every reply like Replies, and also counts them per account, which a snapshot
// needs to know each account's next sequence number.
class CountedReplies final : public protocol::OutboundSink {
public:
    explicit CountedReplies(std::size_t accounts) : counts(accounts + 1, 0) {}
    void send(OwnerId owner, std::span<const std::byte> frame) override {
        all.send(owner, frame);
        if (owner < counts.size()) {
            ++counts[owner];
        }
    }
    Replies all;
    std::vector<std::uint64_t> counts;
};

// The largest session a raw snapshot may ask for before the target restores it, for the
// same reason as in the journal fuzzer. A random setup record could otherwise ask for
// billions of orders.
bool small_enough_to_restore(const Snapshot& snapshot) {
    for (const JournalRecord& record : snapshot.setup) {
        if (const auto c = session_config_of(record)) {
            if (c->max_owners > 64 || c->max_live_orders > 4096 || c->max_symbols > 16) {
                return false;
            }
        }
        if (const auto s = symbol_listing_of(record)) {
            if (s->config.max_orders > 4096) {
                return false;
            }
        }
    }
    return true;
}

void raw_snapshot(std::span<const std::byte> file) {
    Snapshot snapshot;
    if (decode_snapshot(file, snapshot) != SnapshotStatus::Ok) {
        return;
    }
    // Canonical. The same snapshot always has the same bytes.
    check(snapshot_size(snapshot) == file.size());
    std::vector<std::byte> again(file.size());
    check(encode_snapshot(snapshot, again) == file.size());
    check(std::equal(again.begin(), again.end(), file.begin()));

    if (!small_enough_to_restore(snapshot)) {
        return;
    }
    CountedReplies replies(snapshot.replies.size() - 1);
    Replayer<SortedOrderBook> replayer(replies);
    if (replayer.start_from(snapshot) != ReplayStatus::Ok) {
        return;  // Refusing a state that does not fit is allowed.
    }
    auto& orders = replayer.order_entry();
    check(orders.engine().check_invariants());
    protocol::OrderEntryState captured;
    orders.capture(captured);
    check(captured == snapshot.state);
    for (OwnerId owner = 1; owner <= orders.config().max_owners; ++owner) {
        orders.cancel_all(owner, snapshot.as_of_time);
    }
    check(orders.engine().check_invariants());
    check(orders.live_orders() == 0);
}

// The setup records of the test exchange, as a snapshot holds them (no sequence number
// and no time).
std::vector<JournalRecord> setup_records(std::size_t stocks, std::uint8_t limited,
                                         const protocol::RiskLimits& limits) {
    std::vector<JournalRecord> setup;
    setup.push_back(session_start_record(session_config()));
    for (std::size_t i = 0; i < stocks; ++i) {
        setup.push_back(
            add_symbol_record(SymbolListing{.id = static_cast<SymbolId>(i),
                                            .ticker = testing::unwrap(make_ticker(kStocks.at(i))),
                                            .config = stock_config()}));
    }
    if (limited % 5U != 0) {
        setup.push_back(risk_limits_record(1 + limited % 4U, limits));
    }
    return setup;
}

// A restored exchange and the replies it produces after the restore.
template <class Book>
struct Restored {
    explicit Restored(const Snapshot& snapshot)
        : replies(snapshot.replies.size() - 1), status(replayer.start_from(snapshot)) {}

    CountedReplies replies;
    Replayer<Book> replayer{replies};
    ReplayStatus status;
};

void structured_snapshot(Input& in) {
    const std::uint8_t damage = in.byte() % 4U;  // 0 and 1 none, 2 cut, 3 flip.
    const std::uint16_t damage_at = in.u16();
    const std::size_t stocks = 1 + in.byte() % kStocks.size();
    const std::uint8_t limited = in.byte();
    const protocol::RiskLimits limits{.max_order_qty = 10U * (1 + in.byte() % 8U),
                                      .max_open_orders = 1U + in.byte() % 5U};
    const std::size_t split_wanted = std::size_t{2} * in.byte();

    // The live exchange.
    const std::size_t accounts = session_config().max_owners;
    CountedReplies live_replies(accounts);
    protocol::OrderEntry<MapOrderBook> live(live_replies, session_config());
    for (std::size_t i = 0; i < stocks; ++i) {
        live.add_symbol(testing::unwrap(make_ticker(kStocks.at(i))), stock_config());
    }
    if (limited % 5U != 0) {
        live.set_risk_limits(1 + limited % 4U, limits);
    }
    std::vector<pipeline::Request> requests;
    pipeline::Timestamp now = 0;
    for (int i = 0; i < 400 && !in.empty(); ++i) {
        requests.push_back(next_request(in, now));
    }
    const std::size_t split = std::min(split_wanted, requests.size());
    for (std::size_t i = 0; i < split; ++i) {
        apply(live, requests[i]);
    }

    // The snapshot after `split` requests, as if each request were one journal record
    // after the setup records.
    Snapshot snapshot;
    snapshot.setup = setup_records(stocks, limited, limits);
    snapshot.as_of = snapshot.setup.size() + split;
    snapshot.as_of_time = split > 0 ? requests[split - 1].now : 0;
    snapshot.replies = live_replies.counts;
    live.capture(snapshot.state);
    std::vector<std::byte> bytes(snapshot_size(snapshot));
    check(!bytes.empty());
    check(encode_snapshot(snapshot, bytes) == bytes.size());

    Snapshot decoded;
    if (damage == 2) {
        const std::size_t cut = damage_at % bytes.size();  // Always shorter than the file.
        const SnapshotStatus status =
            decode_snapshot(std::span<const std::byte>(bytes).first(cut), decoded);
        // Too short for a header and a checksum, or else its last 4 bytes are read as
        // the checksum and do not match. Only if they matched by chance (1 in 2^32)
        // would the body length in the header catch it.
        if (cut < kSnapshotHeaderLength + 4) {
            check(status == SnapshotStatus::TooShort);
        } else {
            check(status == SnapshotStatus::BadChecksum || status == SnapshotStatus::BadLength);
        }
        return;
    }
    if (damage == 3) {
        const std::size_t at = damage_at % bytes.size();
        bytes[at] ^= std::byte{0xFF};
        const SnapshotStatus status = decode_snapshot(bytes, decoded);
        // The header starts with 8 bytes of magic and 2 of version, and the checksum is
        // checked right after them, so any other byte is caught by the checksum.
        if (at < 8) {
            check(status == SnapshotStatus::BadMagic);
        } else if (at < 10) {
            check(status == SnapshotStatus::BadVersion);
        } else {
            check(status == SnapshotStatus::BadChecksum);
        }
        return;
    }

    check(decode_snapshot(bytes, decoded) == SnapshotStatus::Ok);
    check(decoded == snapshot);
    Restored<FastOrderBook> fast(decoded);
    Restored<SortedOrderBook> sorted(decoded);
    check(fast.status == ReplayStatus::Ok && sorted.status == ReplayStatus::Ok);
    check_same_state(live, fast.replayer.order_entry());
    check_same_state(live, sorted.replayer.order_entry());

    // The rest of the requests. Only the replies after the snapshot are compared, since
    // the restored exchanges never produced the earlier ones.
    const std::size_t replies_before = live_replies.all.sent.size();
    for (std::size_t i = split; i < requests.size(); ++i) {
        apply(live, requests[i]);
        apply(fast.replayer.order_entry(), requests[i]);
        apply(sorted.replayer.order_entry(), requests[i]);
    }
    const std::vector<std::uint32_t> live_after(
        live_replies.all.sent.begin() + static_cast<std::ptrdiff_t>(replies_before),
        live_replies.all.sent.end());
    check(fast.replies.all.sent == live_after);
    check(sorted.replies.all.sent == live_after);
    check_same_state(live, fast.replayer.order_entry());
    check_same_state(live, sorted.replayer.order_entry());

    // And the end states capture to the same thing, so a snapshot taken now would have
    // the same bytes on every book.
    protocol::OrderEntryState live_end;
    protocol::OrderEntryState fast_end;
    protocol::OrderEntryState sorted_end;
    live.capture(live_end);
    fast.replayer.order_entry().capture(fast_end);
    sorted.replayer.order_entry().capture(sorted_end);
    check(fast_end == live_end && sorted_end == live_end);
}

}  // namespace

int run_one(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data + 1, size - 1);
    if ((data[0] & 1U) == 0) {
        raw_snapshot(std::as_bytes(input));
    } else {
        Input in(input);
        structured_snapshot(in);
    }
    return 0;
}

}  // namespace litbook::fuzz
