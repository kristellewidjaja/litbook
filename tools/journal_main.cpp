// Looks inside a litbook journal (docs/JOURNAL.md).
//
//   litbook_journal verify FILE            checks every record and says how the file ends
//   litbook_journal dump FILE [--limit N]  prints one line per record
//   litbook_journal replay FILE            rebuilds the exchange's state from the records
//
// Exit codes. 0 means the journal is clean (and for replay, that it replayed). 1 means
// it ends in a torn record, which a crash during a write leaves behind and recovery cuts
// off. 3 means it is corrupt in the middle, has a bad header, or does not replay. 2 means
// bad arguments or a file that cannot be read.
//
// replay rebuilds the books on SortedOrderBook, which accepts any price band. It prints a
// CRC32C digest of every reply and of every market event it produced, so two replays,
// or a replay and a test, can be compared with one line each. The reply digest runs over
// each reply's owner (4 bytes, little-endian) followed by its bytes. The event digest
// runs over each event's fields in this order, little-endian, 1 byte for the kind (T or
// L) and the side (B or S), 8 for the time, 2 for the stock, 8 for the price, 8 for the
// quantity, 4 for the order count and 8 for the match number.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/crc32c.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/protocol/byte_order.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace litbook;
using namespace litbook::journal;

constexpr int kExitClean = 0;
constexpr int kExitTorn = 1;
constexpr int kExitUsage = 2;
constexpr int kExitBad = 3;

template <class T>
std::string text(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

int exit_code_for(const JournalScan& scan) {
    switch (scan.tail) {
        case TailStatus::Clean:
            return kExitClean;
        case TailStatus::Torn:
            return kExitTorn;
        case TailStatus::Corrupt:
        case TailStatus::BadHeader:
            return kExitBad;
    }
    return kExitBad;
}

// The last lines of verify and dump, which say how the file ends.
void print_tail(const JournalScan& scan) {
    std::printf("valid bytes %zu of %zu\n", scan.valid_length, scan.file_length);
    switch (scan.tail) {
        case TailStatus::Clean:
            std::printf("tail clean\n");
            return;
        case TailStatus::Torn:
            std::printf("tail torn (%s), %zu bytes after the last good record\n",
                        text(scan.stop_reason).c_str(), scan.file_length - scan.valid_length);
            return;
        case TailStatus::Corrupt:
            if (scan.sequence_break) {
                std::printf("tail corrupt, the record at offset %zu does not have sequence %llu\n",
                            scan.valid_length, static_cast<unsigned long long>(scan.next_sequence));
            } else {
                std::printf(
                    "tail corrupt, bad record at offset %zu (%s), good record at offset %zu\n",
                    scan.valid_length, text(scan.stop_reason).c_str(), scan.corrupt_at);
            }
            return;
        case TailStatus::BadHeader:
            std::printf("tail unreadable\n");
            return;
    }
}

void print_header(const std::string& path, const JournalScan& scan) {
    std::printf("journal %s\n", path.c_str());
    if (scan.header == HeaderStatus::Ok) {
        std::printf("header ok, first sequence %llu\n",
                    static_cast<unsigned long long>(scan.first_sequence));
    } else {
        std::printf("header %s\n", text(scan.header).c_str());
    }
}

// The good records of a journal, decoded.
std::vector<JournalRecord> good_records(std::span<const std::byte> file, const JournalScan& scan) {
    std::vector<JournalRecord> out;
    if (scan.header != HeaderStatus::Ok) {
        return out;
    }
    RecordReader reader(file.subspan(kFileHeaderLength, scan.valid_length - kFileHeaderLength));
    JournalRecord record;
    while (reader.next(record)) {
        out.push_back(record);
    }
    return out;
}

int verify(const std::string& path) {
    const MappedFile file(path);
    const JournalScan scan = scan_journal(file.bytes());
    print_header(path, scan);
    std::map<RecordType, std::uint64_t> counts;
    for (const JournalRecord& r : good_records(file.bytes(), scan)) {
        ++counts[r.type];
    }
    const std::uint64_t setup = counts[RecordType::SessionStart] + counts[RecordType::AddSymbol] +
                                counts[RecordType::RiskLimits];
    std::printf("records %llu (setup %llu, frames %llu, cancel alls %llu, stops %llu)\n",
                static_cast<unsigned long long>(scan.records),
                static_cast<unsigned long long>(setup),
                static_cast<unsigned long long>(counts[RecordType::Frame]),
                static_cast<unsigned long long>(counts[RecordType::CancelAll]),
                static_cast<unsigned long long>(counts[RecordType::Stop]));
    if (scan.records > 0) {
        std::printf("sequences %llu to %llu\n",
                    static_cast<unsigned long long>(scan.first_sequence),
                    static_cast<unsigned long long>(scan.next_sequence - 1));
    }
    print_tail(scan);
    std::printf("crc32c %s\n", crc32c_implementation());
    return exit_code_for(scan);
}

void print_record(const JournalRecord& r) {
    std::printf("%llu t=%llu %s", static_cast<unsigned long long>(r.sequence),
                static_cast<unsigned long long>(r.now), text(r.type).c_str());
    switch (r.type) {
        case RecordType::SessionStart: {
            const auto c = session_config_of(r);
            if (c) {
                std::printf(" max owners %u, max live orders %u, max stocks %zu", c->max_owners,
                            c->max_live_orders, c->max_symbols);
            }
            break;
        }
        case RecordType::AddSymbol: {
            const auto s = symbol_listing_of(r);
            if (s) {
                std::printf(" id %u %s tick %lld lot %u band %lld to %lld max orders %u %s",
                            static_cast<unsigned>(s->id), s->ticker.name().c_str(),
                            static_cast<long long>(s->config.tick_size), s->config.lot_size,
                            static_cast<long long>(s->config.min_price),
                            static_cast<long long>(s->config.max_price), s->config.max_orders,
                            text(s->config.self_trade_policy).c_str());
            }
            break;
        }
        case RecordType::RiskLimits: {
            const auto l = risk_limits_of(r);
            if (l) {
                std::printf(" owner %u max order qty %u max open orders %u", r.owner,
                            l->max_order_qty, l->max_open_orders);
            }
            break;
        }
        case RecordType::Frame: {
            // The first byte of an OUCH message is its type letter.
            const auto letter = std::to_integer<unsigned char>(r.payload[0]);
            const bool printable = letter >= 0x21 && letter <= 0x7E;
            std::printf(" owner %u %u bytes type %c", r.owner,
                        static_cast<unsigned>(r.payload_length), printable ? letter : '?');
            break;
        }
        case RecordType::CancelAll:
            std::printf(" owner %u", r.owner);
            break;
        case RecordType::Stop:
            break;
    }
    std::printf("\n");
}

int dump(const std::string& path, std::uint64_t limit) {
    const MappedFile file(path);
    const JournalScan scan = scan_journal(file.bytes());
    print_header(path, scan);
    std::uint64_t printed = 0;
    for (const JournalRecord& r : good_records(file.bytes(), scan)) {
        if (limit != 0 && printed == limit) {
            std::printf("... %llu more records\n",
                        static_cast<unsigned long long>(scan.records - printed));
            break;
        }
        print_record(r);
        ++printed;
    }
    print_tail(scan);
    return exit_code_for(scan);
}

// Keeps the reply digest and each account's reply count.
class DigestReplies final : public protocol::OutboundSink {
public:
    void send(OwnerId owner, std::span<const std::byte> frame) override {
        std::array<std::byte, 4> o{};
        protocol::store_le(std::span<std::byte>(o), 0, owner);
        crc_ = crc32c_extend(crc_, o);
        crc_ = crc32c_extend(crc_, frame);
        ++count_;
        ++per_owner_[owner];
    }
    [[nodiscard]] std::uint32_t crc() const noexcept { return crc_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] const std::map<OwnerId, std::uint64_t>& per_owner() const noexcept {
        return per_owner_;
    }

private:
    std::uint32_t crc_ = 0;
    std::uint64_t count_ = 0;
    std::map<OwnerId, std::uint64_t> per_owner_;
};

// Keeps the market event digest. The time of each event is the time of the record that
// caused it, which the replayer knows.
class DigestEvents final : public protocol::MarketDataSink {
public:
    // The replayer is built after this sink, because it takes the sink in its
    // constructor, so it is attached afterwards.
    void attach(const Replayer<SortedOrderBook>& replayer) noexcept { replayer_ = &replayer; }
    void on_trade(SymbolId symbol, const Trade& t) override {
        add('T', t.aggressor_side, symbol, t.price, t.qty, 0, t.match_id);
    }
    void on_level_update(SymbolId symbol, const LevelUpdate& u) override {
        add('L', u.side, symbol, u.price, u.total_qty, u.order_count, 0);
    }
    [[nodiscard]] std::uint32_t crc() const noexcept { return crc_; }
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
    void add(char kind, Side side, SymbolId symbol, Price price, AggregateQty qty,
             std::uint32_t order_count, MatchId match_id) {
        std::array<std::byte, 40> b{};
        const std::span<std::byte> s(b);
        b[0] = static_cast<std::byte>(kind);
        b[1] = static_cast<std::byte>(side == Side::Buy ? 'B' : 'S');
        protocol::store_le(s, 2, replayer_->now());
        protocol::store_le(s, 10, symbol);
        protocol::store_le(s, 12, static_cast<std::uint64_t>(price));
        protocol::store_le(s, 20, qty);
        protocol::store_le(s, 28, order_count);
        protocol::store_le(s, 32, match_id);
        crc_ = crc32c_extend(crc_, b);
        ++count_;
    }

    const Replayer<SortedOrderBook>* replayer_ = nullptr;
    std::uint32_t crc_ = 0;
    std::uint64_t count_ = 0;
};

int replay(const std::string& path) {
    const MappedFile file(path);
    DigestReplies replies;
    DigestEvents events;
    Replayer<SortedOrderBook> replayer(replies, &events);
    events.attach(replayer);
    const ReplayOutcome outcome = replay_journal(file.bytes(), replayer);
    if (outcome.status != ReplayStatus::Ok) {
        std::printf("replay failed: %s after %llu records\n", text(outcome.status).c_str(),
                    static_cast<unsigned long long>(replayer.stats().records));
        return kExitBad;
    }
    const ReplayStats& s = replayer.stats();
    std::printf(
        "replayed %llu records (setup %llu, frames %llu, malformed %llu, cancel alls %llu, "
        "stops %llu) through sequence %llu\n",
        static_cast<unsigned long long>(s.records),
        static_cast<unsigned long long>(s.setup_records), static_cast<unsigned long long>(s.frames),
        static_cast<unsigned long long>(s.malformed_frames),
        static_cast<unsigned long long>(s.cancel_alls), static_cast<unsigned long long>(s.stops),
        static_cast<unsigned long long>(s.last_sequence));
    std::printf("replies %llu crc32c %08x\n", static_cast<unsigned long long>(replies.count()),
                replies.crc());
    std::printf("market events %llu crc32c %08x\n", static_cast<unsigned long long>(events.count()),
                events.crc());
    if (replayer.started()) {
        const auto& orders = replayer.order_entry();
        std::printf("live orders %u, trades %llu\n", orders.live_orders(),
                    static_cast<unsigned long long>(orders.engine().last_match_id()));
        // Every reply uses up one of its account's sequence numbers, so this is the
        // number the account's next reply would get (docs/PROTOCOL.md).
        for (OwnerId owner = 1; owner <= orders.config().max_owners; ++owner) {
            const auto it = replies.per_owner().find(owner);
            const std::uint64_t sent = it == replies.per_owner().end() ? 0 : it->second;
            if (sent > 0) {
                std::printf("account %u next sequence %llu\n", owner,
                            static_cast<unsigned long long>(sent) + 1);
            }
        }
    }
    std::printf("tail %s\n", text(outcome.scan.tail).c_str());
    return outcome.scan.tail == TailStatus::Torn ? kExitTorn : kExitClean;
}

bool parse_limit(std::string_view s, std::uint64_t& out) {
    if (s.empty() || s.size() > 18) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = v;
    return true;
}

int usage() {
    std::fprintf(stderr,
                 "usage: litbook_journal verify FILE\n"
                 "       litbook_journal dump FILE [--limit N]\n"
                 "       litbook_journal replay FILE\n");
    return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) try {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() < 2) {
        return usage();
    }
    const std::string path(args[1]);
    if (args[0] == "verify" && args.size() == 2) {
        return verify(path);
    }
    if (args[0] == "replay" && args.size() == 2) {
        return replay(path);
    }
    if (args[0] == "dump") {
        std::uint64_t limit = 0;
        if (args.size() == 4 && args[2] == "--limit" && parse_limit(args[3], limit)) {
            return dump(path, limit);
        }
        if (args.size() == 2) {
            return dump(path, 0);
        }
    }
    return usage();
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_journal: %s\n", e.what());
    return kExitUsage;
}
