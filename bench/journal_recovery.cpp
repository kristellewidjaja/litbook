// litbook_journal_recovery [--segments K,K,...] [--segment-records S] [--tail T]
//                          [--target-live L] [--runs R] [--dir DIR]
//
// How long the exchange takes to start again from its journal, against the size of the
// journal, with and without a snapshot (Phase 9).
//
// For each K it writes a journal directory of K full segments of S records followed by
// an open segment of T records, with the requests of a steady stream that holds the
// exchange near two thirds of L live orders. That directory is the journal a crash would
// leave without a snapshot. A copy of it then gets the snapshots the snapshot thread
// would have written: a start snapshot as of the setup records, and one after each closed
// segment, made by the real SnapshotWorker driven by hand, with retention removing the
// older snapshots and segments as it would. The time of each of those steps is the
// snapshot thread's cost per segment. So the second directory needs only the snapshot and
// the open segment of T records to be replayed.
//
// Each run copies one of the two directories to a scratch directory (not timed, because
// a start changes the directory) and times the real ThreadedGateway::start() on it, which
// scans the segments, reads the snapshots, replays, cuts any torn tail, writes a Restart
// record, writes its own start snapshot and removes old files. The runs alternate between
// the two directories, so noise hits both alike. The files were just written or copied,
// so they are in the page cache, and these are warm cache times. A cold start from disk
// would also pay the reads.
//
// The requests are written straight to the journal writer, not sent over TCP, because a
// million round trips would take minutes and the bytes on disk are the same. The setup
// records are built with the same calls in the same order as the exchange makes them, and
// start() would refuse the journal if they differed.
//
// After the runs it checks that both directories rebuilt exactly the same state, that the
// snapshot directory really started from a snapshot and replayed at most T requests, and
// that the other replayed all of them. A run that fails any check exits with 1.
//
// Put DIR on the virtual machine's own disk (/tmp in the dev container, the default).

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/journal/snapshot_format.hpp"
#include "litbook/journal/snapshot_store.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/pipeline/snapshot_worker.hpp"
#include "litbook/pipeline/threaded_gateway.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "journal/steady_stream.hpp"
#include "support/unwrap.hpp"

namespace {

using namespace litbook;  // NOLINT(google-build-using-namespace): a small program.
using Book = SortedOrderBook;
using Exchange = pipeline::ThreadedGateway<Book, concurrency::SpscQueue>;
using Clock = std::chrono::steady_clock;

struct Options {
    std::vector<std::uint64_t> segments{1, 5, 10, 20, 40};
    std::uint64_t segment_records = 100'000;
    std::uint64_t tail = 50'000;
    std::size_t target_live = 10'000;
    int runs = 5;
    std::string dir = "/tmp/litbook_recovery";
};

// The exchange the journals are written for. Four accounts and the two stocks of the
// steady stream, each set up in this order, which is the order of the setup records.
constexpr std::array<std::string_view, testing::kSteadyOwners> kUsers{"a1", "a2", "a3", "a4"};
constexpr std::uint32_t kBookOrders = 1U << 16U;

protocol::OrderEntryConfig order_entry_config() {
    return protocol::OrderEntryConfig{.max_owners = testing::kSteadyOwners, .max_symbols = 2};
}

std::vector<journal::JournalRecord> setup_records() {
    std::vector<journal::JournalRecord> setup;
    setup.push_back(journal::session_start_record(order_entry_config()));
    for (OwnerId owner = 1; owner <= testing::kSteadyOwners; ++owner) {
        setup.push_back(journal::risk_limits_record(owner, protocol::RiskLimits{}));
    }
    SymbolId id = 0;
    for (const char* name : testing::kStreamStocks) {
        setup.push_back(journal::add_symbol_record(
            journal::SymbolListing{.id = id,
                                   .ticker = testing::unwrap(make_ticker(name)),
                                   .config = testing::steady_book_config(kBookOrders)}));
        ++id;
    }
    return setup;
}

void set_up(Exchange& exchange) {
    for (const std::string_view user : kUsers) {
        exchange.add_account(gateway::AccountConfig{.username = user, .password = user});
    }
    for (const char* name : testing::kStreamStocks) {
        exchange.add_symbol(testing::unwrap(make_ticker(name)),
                            testing::steady_book_config(kBookOrders));
    }
}

// Writes the journal of K full segments plus the open tail. Returns the first sequence
// number of every segment, in order.
std::vector<journal::Sequence> write_journal(const std::string& directory, std::uint64_t full,
                                             const Options& options) {
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    journal::PathBuffer paths(directory);
    journal::JournalWriter writer =
        journal::JournalWriter::create(paths.segment(1), journal::JournalWriterConfig{});
    std::vector<journal::Sequence> firsts{1};
    for (const journal::JournalRecord& record : setup_records()) {
        if (journal::append_setup_record(writer, 0, record) != journal::JournalStatus::Ok) {
            throw std::runtime_error("a setup record was refused");
        }
    }
    testing::SteadyStream stream(11, options.target_live);
    const std::uint64_t total = (full * options.segment_records) + options.tail;
    pipeline::Timestamp now = 0;
    // Records written so far, the setup records included, so every full segment holds
    // exactly segment_records records.
    std::uint64_t written = setup_records().size();
    std::uint64_t requests = 0;
    while (requests < total) {
        const testing::InboundFrame frame = stream.next();
        now += 1000;
        const pipeline::Request request =
            pipeline::make_frame_request(frame.owner, frame.bytes, now);
        journal::JournalStatus status = writer.append(request);
        if (status == journal::JournalStatus::BufferFull) {
            static_cast<void>(writer.commit());
            status = writer.append(request);
        }
        if (status != journal::JournalStatus::Ok) {
            throw std::runtime_error("a request was refused by the journal writer");
        }
        ++requests;
        ++written;
        if (written % options.segment_records == 0 && firsts.size() <= full) {
            const journal::Sequence next = writer.next_sequence();
            if (writer.rotate(paths.segment(next), paths.directory()) !=
                journal::JournalStatus::Ok) {
                throw std::runtime_error("rotate failed");
            }
            firsts.push_back(next);
        }
    }
    if (writer.flush() != journal::JournalStatus::Ok) {
        throw std::runtime_error("the last flush failed");
    }
    return firsts;
}

class NullSink final : public protocol::OutboundSink {
public:
    void send(OwnerId /*owner*/, std::span<const std::byte> /*frame*/) override {}
};

// The snapshot the exchange wrote at its first start, as of the last setup record.
journal::Snapshot start_snapshot() {
    NullSink sink;
    journal::Replayer<Book> replayer(sink);
    journal::Snapshot snapshot;
    journal::Sequence sequence = 0;
    for (journal::JournalRecord record : setup_records()) {
        record.sequence = ++sequence;
        static_cast<void>(replayer.apply(record));
        record.sequence = 0;
        snapshot.setup.push_back(record);
    }
    snapshot.as_of = sequence;
    snapshot.replies.assign(testing::kSteadyOwners + 1, 0);
    replayer.order_entry().capture(snapshot.state);
    return snapshot;
}

struct SnapshotThreadCost {
    std::vector<double> step_ms;  // One per closed segment.
    std::uint64_t snapshots = 0;
    std::uint64_t removed = 0;
    std::uint64_t failures = 0;
    std::uintmax_t snapshot_bytes = 0;  // The newest snapshot file.
};

// Gives the directory the snapshots the snapshot thread would have written, by driving a
// real SnapshotWorker over every closed segment, and times each step.
SnapshotThreadCost add_snapshots(const std::string& directory,
                                 const std::vector<journal::Sequence>& firsts) {
    const journal::Snapshot start = start_snapshot();
    std::vector<std::byte> bytes(journal::snapshot_size(start));
    static_cast<void>(journal::encode_snapshot(start, bytes));
    journal::PathBuffer paths(directory);
    if (journal::write_snapshot_file(paths, start.as_of, bytes, false) != 0) {
        throw std::runtime_error("the start snapshot could not be written");
    }
    pipeline::SnapshotWorker<Book> worker(
        pipeline::SnapshotWorkerOptions{.directory = directory, .sync = false, .keep = 2}, start,
        firsts.front(), {start.as_of});
    SnapshotThreadCost cost;
    for (std::size_t k = 0; k + 1 < firsts.size(); ++k) {
        worker.on_segment_closed(firsts[k], firsts[k + 1]);
        const auto t0 = Clock::now();
        static_cast<void>(worker.step());
        const auto t1 = Clock::now();
        cost.step_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    cost.snapshots = worker.stats().snapshots;
    cost.removed = worker.stats().removed;
    cost.failures = worker.stats().failures;
    const journal::DirectoryListing listing = journal::list_journal_directory(directory);
    if (!listing.snapshots.empty()) {
        cost.snapshot_bytes = std::filesystem::file_size(listing.snapshots.back().path);
    }
    return cost;
}

struct Recovery {
    double ms = 0;
    pipeline::RecoveryReport report;
    protocol::OrderEntryState state;
};

// Copies `source` to a scratch directory and times start() on it.
Recovery recover(const std::string& source, const std::string& scratch, const Options& options) {
    std::filesystem::remove_all(scratch);
    std::filesystem::copy(source, scratch, std::filesystem::copy_options::recursive);
    pipeline::JournalOptions journal;
    journal.directory = scratch;
    journal.records_per_segment = options.segment_records;
    journal.keep_snapshots = 2;
    journal.snapshot_thread = true;
    pipeline::PipelineConfig pipeline_config;
    pipeline_config.wait = pipeline::WaitMode::Block;
    Exchange exchange(gateway::SessionConfig{.max_connections = 4}, order_entry_config(),
                      gateway::GatewayConfig{}, pipeline_config, {}, &journal);
    set_up(exchange);
    const auto t0 = Clock::now();
    exchange.start();
    const auto t1 = Clock::now();
    exchange.stop();
    Recovery out;
    out.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.report = exchange.recovery();
    exchange.matching().order_entry().capture(out.state);
    return out;
}

struct Summary {
    double median = 0;
    double min = 0;
    double max = 0;
};

Summary summarize(std::vector<double> values) {
    std::ranges::sort(values);
    if (values.empty()) {
        return {};
    }
    return {.median = values[values.size() / 2], .min = values.front(), .max = values.back()};
}

// Runs one journal size. Returns false if a check failed.
bool run_size(std::uint64_t full, const Options& options) {
    const std::string plain = options.dir + "/plain";
    const std::string snapshots = options.dir + "/snapshots";
    const std::string scratch = options.dir + "/scratch";
    const std::uint64_t requests = (full * options.segment_records) + options.tail;

    const std::vector<journal::Sequence> firsts = write_journal(plain, full, options);
    std::filesystem::remove_all(snapshots);
    std::filesystem::copy(plain, snapshots, std::filesystem::copy_options::recursive);
    const SnapshotThreadCost cost = add_snapshots(snapshots, firsts);

    std::vector<double> plain_ms;
    std::vector<double> snapshot_ms;
    Recovery plain_last;
    Recovery snapshot_last;
    for (int run = 0; run < options.runs; ++run) {
        Recovery a = recover(plain, scratch, options);
        plain_ms.push_back(a.ms);
        Recovery b = recover(snapshots, scratch, options);
        snapshot_ms.push_back(b.ms);
        plain_last = std::move(a);
        snapshot_last = std::move(b);
    }
    std::filesystem::remove_all(scratch);

    const Summary a = summarize(plain_ms);
    const Summary b = summarize(snapshot_ms);
    const Summary step = summarize(cost.step_ms);
    const pipeline::RecoveryReport& ra = plain_last.report;
    const pipeline::RecoveryReport& rb = snapshot_last.report;
    std::printf(
        "journal: %llu segments of %llu records plus %llu, %llu requests, %u live orders "
        "at the end\n",
        static_cast<unsigned long long>(full),
        static_cast<unsigned long long>(options.segment_records),
        static_cast<unsigned long long>(options.tail), static_cast<unsigned long long>(requests),
        static_cast<unsigned>(plain_last.state.orders.size()));
    std::printf(
        "  no snapshot:   start %10.2f ms median, %10.2f min, %10.2f max; replayed %llu "
        "requests from %u segments\n",
        a.median, a.min, a.max, static_cast<unsigned long long>(ra.frames), ra.segments);
    std::printf(
        "  with snapshot: start %10.2f ms median, %10.2f min, %10.2f max; replayed %llu "
        "requests from %u segments after the snapshot as of %llu\n",
        b.median, b.min, b.max, static_cast<unsigned long long>(rb.frames), rb.segments,
        static_cast<unsigned long long>(rb.snapshot_as_of));
    std::printf(
        "  snapshot thread: %zu segments, step %.2f ms median, %.2f min, %.2f max; "
        "snapshot %ju bytes, %llu snapshots, %llu files removed\n",
        cost.step_ms.size(), step.median, step.min, step.max, cost.snapshot_bytes,
        static_cast<unsigned long long>(cost.snapshots),
        static_cast<unsigned long long>(cost.removed));

    const bool same_state = plain_last.state == snapshot_last.state;
    const bool plain_full = ra.snapshot_as_of == 0 && ra.frames == requests;
    // Segments count the setup records too, so the open segment holds the tail plus as
    // many requests as there are setup records.
    const bool used_snapshot =
        rb.snapshot_as_of > 0 && rb.frames <= options.tail + setup_records().size();
    const bool thread_ok = cost.failures == 0 && cost.snapshots == full;
    std::printf("  health: states agree %s, full replay %s, snapshot used %s, snapshot thread %s\n",
                same_state ? "yes" : "NO", plain_full ? "yes" : "NO", used_snapshot ? "yes" : "NO",
                thread_ok ? "ok" : "FAILED");
    std::filesystem::remove_all(plain);
    std::filesystem::remove_all(snapshots);
    return same_state && plain_full && used_snapshot && thread_ok;
}

std::optional<std::uint64_t> parse_number(std::string_view text, std::uint64_t min) {
    if (text.empty() || text.size() > 12) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = (value * 10) + static_cast<std::uint64_t>(c - '0');
    }
    if (value < min) {
        return std::nullopt;
    }
    return value;
}

// Parses "1,5,10" into its numbers, each at least 1.
std::optional<std::vector<std::uint64_t>> parse_list(std::string_view text) {
    std::vector<std::uint64_t> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const auto n = parse_number(text.substr(0, comma), 1);
        if (!n) {
            return std::nullopt;
        }
        out.push_back(*n);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

// Returns nothing if the arguments are wrong.
std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i += 2) {
        const std::string_view flag = argv[i];
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        const std::string_view value = argv[i + 1];
        if (flag == "--segments") {
            auto list = parse_list(value);
            if (!list) {
                return std::nullopt;
            }
            options.segments = std::move(*list);
        } else if (flag == "--segment-records" || flag == "--tail" || flag == "--target-live" ||
                   flag == "--runs") {
            const auto n = parse_number(value, 1);
            if (!n) {
                return std::nullopt;
            }
            if (flag == "--segment-records") {
                options.segment_records = *n;
            } else if (flag == "--tail") {
                options.tail = *n;
            } else if (flag == "--target-live") {
                options.target_live = static_cast<std::size_t>(*n);
            } else {
                options.runs = static_cast<int>(std::min<std::uint64_t>(*n, 1000));
            }
        } else if (flag == "--dir" && !value.empty()) {
            options.dir = value;
        } else {
            return std::nullopt;
        }
    }
    // Every full segment must hold more than the setup records, and the tail must fit in
    // one segment, as it does when the exchange rotates.
    if (options.segment_records <= setup_records().size() ||
        options.tail >= options.segment_records) {
        return std::nullopt;
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) try {
    const std::optional<Options> parsed = parse_options(argc, argv);
    if (!parsed) {
        std::fprintf(stderr,
                     "usage: litbook_journal_recovery [--segments K,K,...] [--segment-records S]\n"
                     "       [--tail T (below S)] [--target-live L] [--runs R] [--dir DIR]\n");
        return 2;
    }
    const Options& options = *parsed;
    std::printf(
        "recovery time of ThreadedGateway::start(), %d runs per directory, warm page "
        "cache, journals in %s\n",
        options.runs, options.dir.c_str());
    bool ok = true;
    for (const std::uint64_t full : options.segments) {
        ok = run_size(full, options) && ok;
    }
    std::filesystem::remove_all(options.dir);
    return ok ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_journal_recovery: %s\n", e.what());
    return 1;
}
