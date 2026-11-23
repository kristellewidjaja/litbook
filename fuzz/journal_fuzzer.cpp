// Fuzz target for the journal.
//
// The first input byte picks one of two modes with its lowest bit.
//
// Mode 0 treats the rest of the input as a journal file. scan_journal must never crash
// or read outside the bytes (the sanitizers catch that), and what it reports must be
// true. The good part ends inside the file, its records carry consecutive sequence
// numbers and each one encodes back to exactly its own bytes. A torn tail really has no
// good record after it, and a corrupt one really has. If the journal starts a small
// enough session, it is also replayed, and every book must keep its invariants.
//
// Mode 1 builds a valid journal from the input (setup records, then orders, replaces,
// cancels, raw frames, cancels on disconnect and stops), and then may damage it by
// cutting it short or flipping one byte. An undamaged journal must scan clean and replay
// into FastOrderBook and SortedOrderBook with exactly the replies and state that the
// same requests gave a MapOrderBook exchange live. A cut journal must replay exactly
// like the whole records before the cut. A flipped byte must stop the scan exactly at the
// record it is in, as torn if that was the last record and as corrupt otherwise.
// The target aborts if any check fails.

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "fuzz_target.hpp"
#include "journal_fuzz_support.hpp"
#include "support/unwrap.hpp"

namespace litbook::fuzz {

namespace {

using namespace litbook::journal;

// The largest session a raw journal may ask for before the target replays it. A random
// header could otherwise ask for billions of orders, and the allocation would be the
// only thing tested.
bool small_enough_to_replay(std::span<const std::byte> file, const JournalScan& scan) {
    RecordReader reader(file.subspan(kFileHeaderLength, scan.valid_length - kFileHeaderLength));
    JournalRecord r;
    while (reader.next(r)) {
        if (const auto c = session_config_of(r)) {
            if (c->max_owners > 64 || c->max_live_orders > 4096 || c->max_symbols > 16) {
                return false;
            }
        }
        if (const auto s = symbol_listing_of(r)) {
            if (s->config.max_orders > 4096) {
                return false;
            }
        }
    }
    return true;
}

void raw_journal(std::span<const std::byte> file) {
    const JournalScan s = scan_journal(file);
    check(s.valid_length <= file.size());
    check(s.file_length == file.size());
    if (s.header != HeaderStatus::Ok) {
        check(s.tail == TailStatus::BadHeader);
        check(s.valid_length == 0 && s.records == 0);
        return;
    }
    check(s.tail != TailStatus::BadHeader);
    check(s.valid_length >= kFileHeaderLength);
    check(s.next_sequence == s.first_sequence + s.records);
    check((s.tail == TailStatus::Clean) == (s.valid_length == file.size()));

    // Walk the good part again and check every record.
    const std::span<const std::byte> good =
        file.subspan(kFileHeaderLength, s.valid_length - kFileHeaderLength);
    RecordReader reader(good);
    JournalRecord r;
    std::uint64_t count = 0;
    std::size_t offset = 0;
    std::array<std::byte, kMaxRecordLength> out{};
    while (reader.next(r)) {
        check(r.sequence == s.first_sequence + count);
        const std::size_t n = encode_record(out, r);
        check(n > 0 && offset + n <= good.size());
        check(std::memcmp(out.data(), good.data() + offset, n) == 0);
        offset += n;
        ++count;
    }
    check(reader.status() == RecordStatus::Ok);
    check(count == s.records && offset == good.size());

    if (s.tail == TailStatus::Torn) {
        for (std::size_t p = s.valid_length; p < file.size(); ++p) {
            check(decode_record(file.subspan(p)).status != RecordStatus::Ok);
        }
    }
    if (s.tail == TailStatus::Corrupt) {
        check(s.corrupt_at >= s.valid_length && s.corrupt_at < file.size());
        check(decode_record(file.subspan(s.corrupt_at)).status == RecordStatus::Ok);
    }

    if (s.tail != TailStatus::Corrupt && small_enough_to_replay(file, s)) {
        Replies replies;
        Replayer<SortedOrderBook> replayer(replies);
        static_cast<void>(replay_journal(file, replayer));
        if (replayer.started()) {
            check(replayer.order_entry().engine().check_invariants());
        }
    }
}

template <class Book>
struct Replayed {
    Replies replies;
    Replayer<Book> replayer{replies};
    ReplayOutcome outcome;
    explicit Replayed(std::span<const std::byte> file) : outcome(replay_journal(file, replayer)) {}
};

void structured_journal(Input& in) {
    const std::uint8_t damage = in.byte() % 4U;  // 0 and 1 none, 2 cut, 3 flip.
    const std::uint16_t damage_at = in.u16();
    const Sequence first = 1 + in.byte() % 3U;
    const std::size_t stocks = 1 + in.byte() % kStocks.size();

    std::vector<std::byte> file(kFileHeaderLength);
    encode_file_header(file, first);
    std::vector<std::size_t> starts;  // Where each record begins.
    Sequence seq = first;
    const auto add = [&](auto encode) {
        std::array<std::byte, kMaxRecordLength> out{};
        const std::size_t n = encode(std::span<std::byte>(out), seq);
        check(n > 0);
        starts.push_back(file.size());
        file.insert(file.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
        ++seq;
    };

    add([&](auto out, Sequence s) { return encode_session_start(out, s, 0, session_config()); });
    for (std::size_t i = 0; i < stocks; ++i) {
        const SymbolListing listing{.id = static_cast<SymbolId>(i),
                                    .ticker = testing::unwrap(make_ticker(kStocks.at(i))),
                                    .config = stock_config()};
        add([&](auto out, Sequence s) { return encode_add_symbol(out, s, 0, listing); });
    }
    const std::uint8_t limited = in.byte();
    const protocol::RiskLimits limits{.max_order_qty = 10U * (1 + in.byte() % 8U),
                                      .max_open_orders = 1U + in.byte() % 5U};
    if (limited % 5U != 0) {
        add([&](auto out, Sequence s) {
            return encode_risk_limits(out, s, 0, 1 + limited % 4U, limits);
        });
    }

    // The live exchange handles the requests as they are journaled.
    Replies live_replies;
    protocol::OrderEntry<MapOrderBook> live(live_replies, session_config());
    for (std::size_t i = 0; i < stocks; ++i) {
        live.add_symbol(testing::unwrap(make_ticker(kStocks.at(i))), stock_config());
    }
    if (limited % 5U != 0) {
        live.set_risk_limits(1 + limited % 4U, limits);
    }
    pipeline::Timestamp now = 0;
    for (int i = 0; i < 400 && !in.empty(); ++i) {
        const pipeline::Request r = next_request(in, now);
        add([&](auto out, Sequence s) { return encode_request(out, s, r); });
        apply(live, r);
    }

    if (damage <= 1) {
        const JournalScan scan = scan_journal(file);
        check(scan.tail == TailStatus::Clean);
        check(scan.records == starts.size());
        Replayed<FastOrderBook> fast(file);
        Replayed<SortedOrderBook> sorted(file);
        check(fast.outcome.status == ReplayStatus::Ok && sorted.outcome.status == ReplayStatus::Ok);
        check(fast.replies.sent == live_replies.sent);
        check(sorted.replies.sent == live_replies.sent);
        check_same_state(live, fast.replayer.order_entry());
        check_same_state(live, sorted.replayer.order_entry());
        return;
    }

    if (damage == 2) {
        // Cut the file. The scan must give back exactly the whole records before the cut,
        // and the replay of the cut file must equal the replay of those records.
        const std::size_t cut = damage_at % (file.size() + 1);
        const std::span<const std::byte> cut_file = std::span<const std::byte>(file).first(cut);
        const JournalScan scan = scan_journal(cut_file);
        if (cut < kFileHeaderLength) {
            check(scan.tail == TailStatus::BadHeader);
            return;
        }
        std::size_t whole = 0;
        while (whole < starts.size() &&
               (whole + 1 < starts.size() ? starts[whole + 1] : file.size()) <= cut) {
            ++whole;
        }
        check(scan.records == whole);
        const std::size_t end_of_whole = whole < starts.size() ? starts[whole] : file.size();
        check(scan.valid_length == end_of_whole);
        if (cut == end_of_whole) {
            check(scan.tail == TailStatus::Clean);
        } else if (scan.tail == TailStatus::Corrupt) {
            // The cut record carried a whole record inside its frame, and that one is
            // still there. Recovery refuses such a journal instead of guessing.
            check(scan.corrupt_at > end_of_whole && scan.corrupt_at < cut);
            return;
        } else {
            check(scan.tail == TailStatus::Torn);
        }
        const Replayed<SortedOrderBook> from_cut(cut_file);
        const Replayed<SortedOrderBook> from_prefix(
            std::span<const std::byte>(file).first(end_of_whole));
        check(from_cut.outcome.status == from_prefix.outcome.status);
        check(from_cut.replies.sent == from_prefix.replies.sent);
        // A journal built here is always valid, so every whole record replays.
        check(from_cut.outcome.status == ReplayStatus::Ok);
        check(from_cut.replayer.stats().records == whole);
        return;
    }

    // Flip one byte. Only the record that holds it may be lost, and the scan must say
    // whether it was the last record.
    const std::size_t at = damage_at % file.size();
    file[at] ^= std::byte{0xFF};
    const JournalScan scan = scan_journal(file);
    if (at < kFileHeaderLength) {
        check(scan.tail == TailStatus::BadHeader);
        return;
    }
    std::size_t index = 0;
    while (index + 1 < starts.size() && starts[index + 1] <= at) {
        ++index;
    }
    check(scan.records == index);
    check(scan.valid_length == starts[index]);
    const bool last = index + 1 == starts.size();
    const std::size_t end = last ? file.size() : starts[index + 1];
    if (scan.tail == TailStatus::Corrupt) {
        // The good record the scan found is the next real one, or a whole record that a
        // raw frame carried inside the damaged one (fuzz/journal_regressions has one).
        check(scan.corrupt_at > starts[index] && scan.corrupt_at <= end);
        check(decode_record(std::span<const std::byte>(file).subspan(scan.corrupt_at)).status ==
              RecordStatus::Ok);
    } else {
        // Without such a record inside it, damage to the last record is a torn tail, and
        // damage to any other is always corruption, because the next record is good.
        check(last && scan.tail == TailStatus::Torn);
    }
}

}  // namespace

int run_one(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data + 1, size - 1);
    if ((data[0] & 1U) == 0) {
        raw_journal(std::as_bytes(input));
    } else {
        Input in(input);
        structured_journal(in);
    }
    return 0;
}

}  // namespace litbook::fuzz
