// Looks inside a litbook journal (docs/JOURNAL.md).
//
//   litbook_journal verify PATH                checks every record and says how it ends
//   litbook_journal dump PATH [--limit N]      prints one line per record
//   litbook_journal replay PATH [--from-start] rebuilds the exchange's state
//   litbook_journal snapshot DIRECTORY         writes a snapshot as of the last record
//
// PATH is a journal directory, as litbook_gateway --journal writes it, or one segment
// file of such a directory. For a directory, verify also checks that the segments form
// one chain and reads every snapshot, and says where a recovery would start. replay
// starts from the newest usable snapshot, like recovery, unless --from-start asks for
// record 1, which needs the first segment. snapshot replays the directory the same way
// and writes the state as of its last good record into it, through a temporary file that
// is synced and renamed. It never removes a file, since that is the exchange's job.
//
// Exit codes. 0 means the journal is clean (and for replay and snapshot, that it
// replayed). 1 means it ends in a torn record, which a crash during a write leaves behind
// and recovery cuts off, or a directory's last segment was cut short while it was
// created. 3 means it is corrupt in the middle, has a bad header, has segments that do
// not form a chain, lacks the records a replay needs, or does not replay. 2 means bad
// arguments or a file that cannot be read.
//
// replay rebuilds the books on SortedOrderBook, which accepts any price band. It prints a
// CRC32C digest of every reply and of every market event it produced, so two replays,
// or a replay and a test, can be compared with one line each. The reply digest runs over
// each reply's owner (4 bytes, little-endian) followed by its bytes. The event digest
// runs over each event's fields in this order, little-endian, 1 byte for the kind (T or
// L) and the side (B or S), 8 for the time, 2 for the stock, 8 for the price, 8 for the
// quantity, 4 for the order count and 8 for the match number. A replay from a snapshot
// only produces what follows the snapshot, so its digests cover only that.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/crc32c.hpp"
#include "litbook/journal/directory_replay.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/snapshot_format.hpp"
#include "litbook/journal/snapshot_store.hpp"
#include "litbook/protocol/byte_order.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
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

int verify_file(const std::string& path) {
    const MappedFile file(path);
    const JournalScan scan = scan_journal(file.bytes());
    print_header(path, scan);
    std::map<RecordType, std::uint64_t> counts;
    for (const JournalRecord& r : good_records(file.bytes(), scan)) {
        ++counts[r.type];
    }
    const std::uint64_t setup = counts[RecordType::SessionStart] + counts[RecordType::AddSymbol] +
                                counts[RecordType::RiskLimits];
    std::printf(
        "records %llu (setup %llu, frames %llu, cancel alls %llu, stops %llu, restarts %llu)\n",
        static_cast<unsigned long long>(scan.records), static_cast<unsigned long long>(setup),
        static_cast<unsigned long long>(counts[RecordType::Frame]),
        static_cast<unsigned long long>(counts[RecordType::CancelAll]),
        static_cast<unsigned long long>(counts[RecordType::Stop]),
        static_cast<unsigned long long>(counts[RecordType::Restart]));
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
        case RecordType::Restart:
            break;
    }
    std::printf("\n");
}

int dump_file(const std::string& path, std::uint64_t limit) {
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

// Prints what a replay produced. `earlier_replies` holds each account's replies before
// the replay started (from a snapshot), or is empty.
void print_replay(Replayer<SortedOrderBook>& replayer, const DigestReplies& replies,
                  const DigestEvents& events, const std::vector<std::uint64_t>& earlier_replies) {
    const ReplayStats& s = replayer.stats();
    std::printf(
        "replayed %llu records (setup %llu, frames %llu, malformed %llu, cancel alls %llu, "
        "stops %llu, restarts %llu) through sequence %llu\n",
        static_cast<unsigned long long>(s.records),
        static_cast<unsigned long long>(s.setup_records), static_cast<unsigned long long>(s.frames),
        static_cast<unsigned long long>(s.malformed_frames),
        static_cast<unsigned long long>(s.cancel_alls), static_cast<unsigned long long>(s.stops),
        static_cast<unsigned long long>(s.restarts),
        static_cast<unsigned long long>(s.last_sequence));
    if (s.skipped > 0) {
        std::printf("skipped %llu records the snapshot already holds\n",
                    static_cast<unsigned long long>(s.skipped));
    }
    std::printf("replies %llu crc32c %08x\n", static_cast<unsigned long long>(replies.count()),
                replies.crc());
    std::printf("market events %llu crc32c %08x\n", static_cast<unsigned long long>(events.count()),
                events.crc());
    if (!replayer.started()) {
        return;
    }
    const auto& orders = replayer.order_entry();
    std::printf("live orders %u, trades %llu\n", orders.live_orders(),
                static_cast<unsigned long long>(orders.engine().last_match_id()));
    // Every reply uses up one of its account's sequence numbers, so this is the number
    // the account's next reply would get (docs/PROTOCOL.md).
    for (OwnerId owner = 1; owner <= orders.config().max_owners; ++owner) {
        std::uint64_t sent = 0;
        if (owner < earlier_replies.size()) {
            sent = earlier_replies[owner];
        }
        const auto it = replies.per_owner().find(owner);
        if (it != replies.per_owner().end()) {
            sent += it->second;
        }
        if (sent > 0) {
            std::printf("account %u next sequence %llu\n", owner,
                        static_cast<unsigned long long>(sent) + 1);
        }
    }
}

int replay_file(const std::string& path) {
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
    print_replay(replayer, replies, events, {});
    std::printf("tail %s\n", text(outcome.scan.tail).c_str());
    return outcome.scan.tail == TailStatus::Torn ? kExitTorn : kExitClean;
}

// The directory commands.

std::string file_name(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

// One line per segment of a chain.
void print_segments(const ChainScan& chain) {
    for (const SegmentScan& segment : chain.segments) {
        const JournalScan& scan = segment.scan;
        std::printf("segment %s: first sequence %llu, records %llu, tail %s\n",
                    file_name(segment.file.path).c_str(),
                    static_cast<unsigned long long>(scan.first_sequence),
                    static_cast<unsigned long long>(scan.records), text(scan.tail).c_str());
    }
    if (!chain.torn_rotation.empty()) {
        std::printf("segment %s: header cut short while it was created (a torn rotation)\n",
                    file_name(chain.torn_rotation).c_str());
    }
}

// Says why a chain is broken, naming the segment and what its scan found.
void print_chain(const ChainScan& chain) {
    if (chain.status == ChainStatus::Ok) {
        std::printf("chain ok, records %llu, next sequence %llu\n",
                    static_cast<unsigned long long>(chain.records),
                    static_cast<unsigned long long>(chain.next_sequence));
        return;
    }
    if (chain.status == ChainStatus::Empty) {
        std::printf("chain empty, the directory holds no segment\n");
        return;
    }
    std::printf("chain %s at segment %s (first sequence %llu)\n", text(chain.status).c_str(),
                file_name(chain.problem.file.path).c_str(),
                static_cast<unsigned long long>(chain.problem.scan.first_sequence));
    print_tail(chain.problem.scan);
}

void print_snapshots(const std::vector<SnapshotCheck>& checks) {
    for (const SnapshotCheck& check : checks) {
        std::printf("snapshot %s: as of %llu, ", file_name(check.file.path).c_str(),
                    static_cast<unsigned long long>(check.file.as_of));
        if (check.status != SnapshotStatus::Ok) {
            std::printf("refused (%s)\n", text(check.status).c_str());
        } else if (!check.continued) {
            std::printf("not continued by the segments\n");
        } else {
            std::printf("ok\n");
        }
    }
}

// The exit code for a directory whose chain and start are usable.
int exit_code_for(const ChainScan& chain) {
    const bool torn_last = chain.segments.back().scan.tail == TailStatus::Torn;
    if (torn_last || !chain.torn_rotation.empty()) {
        return kExitTorn;
    }
    return kExitClean;
}

int verify_directory(const std::string& directory) {
    std::printf("journal directory %s\n", directory.c_str());
    const DirectoryListing listing = list_journal_directory(directory);
    const ChainScan chain = scan_segment_chain(listing);
    const SnapshotChoice choice = check_snapshots(listing, chain);
    print_segments(chain);
    print_chain(chain);
    print_snapshots(choice.checks);
    if (!listing.temporary.empty()) {
        std::printf("temporary snapshot files %zu (a crash while writing one)\n",
                    listing.temporary.size());
    }
    std::printf("crc32c %s\n", crc32c_implementation());
    if (chain.status != ChainStatus::Ok) {
        return kExitBad;
    }
    if (choice.newest) {
        std::printf("recovery starts from the snapshot as of %llu\n",
                    static_cast<unsigned long long>(choice.newest->as_of));
    } else if (chain.segments.front().scan.first_sequence == 1) {
        std::printf("recovery starts from record 1\n");
    } else {
        std::printf(
            "recovery cannot start, the first segment starts at record %llu and no "
            "snapshot holds the records before it\n",
            static_cast<unsigned long long>(chain.segments.front().scan.first_sequence));
        return kExitBad;
    }
    return exit_code_for(chain);
}

int dump_directory(const std::string& directory, std::uint64_t limit) {
    std::printf("journal directory %s\n", directory.c_str());
    const ChainScan chain = scan_segment_chain(list_journal_directory(directory));
    std::uint64_t printed = 0;
    for (const SegmentScan& segment : chain.segments) {
        if (limit != 0 && printed == limit) {
            break;
        }
        std::printf("segment %s\n", file_name(segment.file.path).c_str());
        const MappedFile file(segment.file.path);
        for (const JournalRecord& r : good_records(file.bytes(), segment.scan)) {
            if (limit != 0 && printed == limit) {
                break;
            }
            print_record(r);
            ++printed;
        }
    }
    if (limit != 0 && printed == limit && chain.records > printed) {
        std::printf("... %llu more records\n",
                    static_cast<unsigned long long>(chain.records - printed));
    }
    print_chain(chain);
    if (chain.status != ChainStatus::Ok) {
        return kExitBad;
    }
    return exit_code_for(chain);
}

// Says why a directory could not be replayed, or returns false if it could.
bool print_directory_failure(const DirectoryReplay& result,
                             const Replayer<SortedOrderBook>& replayer) {
    switch (result.status) {
        case DirectoryStatus::Ok:
            return false;
        case DirectoryStatus::Empty:
        case DirectoryStatus::BrokenChain:
            print_chain(result.chain);
            break;
        case DirectoryStatus::MissingStart:
            std::printf(
                "replay failed: the first segment starts at record %llu, and no usable "
                "snapshot holds the records before it\n",
                static_cast<unsigned long long>(result.chain.segments.front().scan.first_sequence));
            break;
        case DirectoryStatus::ReplayFailed:
            std::printf("replay failed: %s after %llu records\n", text(result.replay).c_str(),
                        static_cast<unsigned long long>(replayer.stats().records));
            break;
    }
    return true;
}

void print_start(const DirectoryReplay& result) {
    if (result.snapshot_as_of == 0) {
        std::printf("start record 1\n");
    } else {
        std::printf("start snapshot as of %llu\n",
                    static_cast<unsigned long long>(result.snapshot_as_of));
    }
}

int replay_directory_command(const std::string& directory, StartFrom start) {
    DigestReplies replies;
    DigestEvents events;
    Replayer<SortedOrderBook> replayer(replies, &events);
    events.attach(replayer);
    const DirectoryReplay result = replay_directory(directory, replayer, start);
    if (print_directory_failure(result, replayer)) {
        return kExitBad;
    }
    print_start(result);
    print_replay(replayer, replies, events, result.snapshot_replies);
    std::printf("tail %s\n", text(result.chain.segments.back().scan.tail).c_str());
    return exit_code_for(result.chain);
}

// Counts each account's replies, starting from the counts of the snapshot the replay
// started from.
class CountReplies final : public protocol::OutboundSink {
public:
    void send(OwnerId owner, std::span<const std::byte> /*frame*/) override {
        if (owner >= counts_.size()) {
            counts_.resize(owner + std::size_t{1}, 0);
        }
        ++counts_[owner];
    }
    void start_from(const std::vector<std::uint64_t>& counts) { counts_ = counts; }
    [[nodiscard]] const std::vector<std::uint64_t>& counts() const noexcept { return counts_; }

private:
    std::vector<std::uint64_t> counts_;
};

int snapshot_directory(const std::string& directory) {
    CountReplies replies;
    Replayer<SortedOrderBook> replayer(replies);
    const DirectoryReplay result = replay_directory(directory, replayer, StartFrom::NewestSnapshot);
    if (print_directory_failure(result, replayer)) {
        return kExitBad;
    }
    print_start(result);

    Snapshot snapshot;
    snapshot.as_of = replayer.stats().last_sequence;
    snapshot.as_of_time = replayer.now();
    for (JournalRecord record : result.setup) {
        // A snapshot keeps only what a setup record says, not where it was in the journal.
        record.sequence = 0;
        record.now = 0;
        snapshot.setup.push_back(record);
    }
    // The replies count starts at the snapshot's counts, plus whatever the replay sent
    // after it, with one entry per account plus the unused entry 0.
    const std::size_t accounts = replayer.order_entry().config().max_owners;
    snapshot.replies = result.snapshot_replies;
    snapshot.replies.resize(accounts + 1, 0);
    for (std::size_t owner = 1; owner < replies.counts().size() && owner <= accounts; ++owner) {
        snapshot.replies[owner] += replies.counts()[owner];
    }
    replayer.order_entry().capture(snapshot.state);

    std::vector<std::byte> bytes(snapshot_size(snapshot));
    if (bytes.empty() || encode_snapshot(snapshot, bytes) != bytes.size()) {
        std::printf("snapshot failed: the state could not be encoded\n");
        return kExitBad;
    }
    PathBuffer paths(directory);
    const int error = write_snapshot_file(paths, snapshot.as_of, bytes, true);
    if (error != 0) {
        std::fprintf(stderr, "litbook_journal: writing the snapshot failed (%s)\n",
                     std::generic_category().message(error).c_str());
        return kExitUsage;
    }
    std::printf("snapshot as of %llu written to %s (%zu bytes)\n",
                static_cast<unsigned long long>(snapshot.as_of),
                file_name(paths.snapshot(snapshot.as_of)).c_str(), bytes.size());
    return kExitClean;
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
                 "usage: litbook_journal verify PATH\n"
                 "       litbook_journal dump PATH [--limit N]\n"
                 "       litbook_journal replay PATH [--from-start]\n"
                 "       litbook_journal snapshot DIRECTORY\n");
    return kExitUsage;
}

int verify(const std::string& path) {
    if (std::filesystem::is_directory(path)) {
        return verify_directory(path);
    }
    return verify_file(path);
}

int dump(const std::string& path, std::uint64_t limit) {
    if (std::filesystem::is_directory(path)) {
        return dump_directory(path, limit);
    }
    return dump_file(path, limit);
}

int replay(const std::string& path, StartFrom start) {
    if (std::filesystem::is_directory(path)) {
        return replay_directory_command(path, start);
    }
    return replay_file(path);
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
        return replay(path, StartFrom::NewestSnapshot);
    }
    // A single file always replays from its own first record, so --from-start is only
    // accepted for a directory.
    if (args[0] == "replay" && args.size() == 3 && args[2] == "--from-start" &&
        std::filesystem::is_directory(path)) {
        return replay(path, StartFrom::FirstRecord);
    }
    if (args[0] == "snapshot" && args.size() == 2) {
        if (!std::filesystem::is_directory(path)) {
            return usage();
        }
        return snapshot_directory(path);
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
