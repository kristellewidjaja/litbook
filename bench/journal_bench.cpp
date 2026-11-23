// Benchmarks of the journal and snapshot pieces of Phase 9, each on its own, with no
// threads. CRC32C (table and hardware), appending a record, committing a batch under each
// sync policy, rotating a segment, replaying a journal into each book, and capturing,
// encoding, decoding and restoring a snapshot at three state sizes.
//
// The benchmarks that touch files work in LITBOOK_BENCH_DIR, or /tmp/litbook_bench if it
// is not set. Inside the dev container /tmp is the virtual machine's own disk, while /work
// is shared from macOS and makes fdatasync look much cheaper than it is (DECISIONS.md
// D-060), so do not point it there.

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/crc32c.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/journal/snapshot_format.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "journal/steady_stream.hpp"
#include "support/unwrap.hpp"

namespace litbook::journal {
namespace {

using protocol::OrderEntry;
using protocol::OrderEntryConfig;
using testing::InboundFrame;

// The exchange every benchmark here uses, which is the one steady_stream is written for.
// Books hold up to 2^18 orders, more than the largest state measured.
constexpr std::uint32_t kBookOrders = 1U << 18U;

OrderEntryConfig bench_order_entry_config() {
    return OrderEntryConfig{.max_owners = testing::kSteadyOwners, .max_symbols = 2};
}

std::vector<SymbolListing> bench_listings() {
    std::vector<SymbolListing> listings;
    SymbolId id = 0;
    for (const char* name : testing::kStreamStocks) {
        listings.push_back(SymbolListing{.id = id,
                                         .ticker = testing::unwrap(make_ticker(name)),
                                         .config = testing::steady_book_config(kBookOrders)});
        ++id;
    }
    return listings;
}

std::vector<JournalRecord> bench_setup_records() {
    std::vector<JournalRecord> setup;
    setup.push_back(session_start_record(bench_order_entry_config()));
    for (const SymbolListing& listing : bench_listings()) {
        setup.push_back(add_symbol_record(listing));
    }
    return setup;
}

template <OrderBook Book>
void list_stocks(OrderEntry<Book>& orders) {
    for (const SymbolListing& listing : bench_listings()) {
        orders.add_symbol(listing.ticker, listing.config);
    }
}

// Counts the replies of each account, which a snapshot needs.
class ReplyCounter final : public protocol::OutboundSink {
public:
    void send(OwnerId owner, std::span<const std::byte> /*frame*/) override { ++counts.at(owner); }
    std::vector<std::uint64_t> counts = std::vector<std::uint64_t>(testing::kSteadyOwners + 1);
};

// An empty directory for one benchmark's files, removed again when it goes away.
class BenchDirectory {
public:
    explicit BenchDirectory(const char* name) {
        const char* root = std::getenv("LITBOOK_BENCH_DIR");  // NOLINT(concurrency-mt-unsafe)
        path_ = std::filesystem::path(root != nullptr ? root : "/tmp/litbook_bench") / name;
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~BenchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    BenchDirectory(const BenchDirectory&) = delete;
    BenchDirectory& operator=(const BenchDirectory&) = delete;
    BenchDirectory(BenchDirectory&&) = delete;
    BenchDirectory& operator=(BenchDirectory&&) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }
    [[nodiscard]] std::string file(const char* name) const { return (path_ / name).string(); }

    // Removes every file in the directory. A writer may still hold one of them open,
    // which is fine, because an unlinked file can still be written.
    void clear() const {
        for (const auto& entry : std::filesystem::directory_iterator(path_)) {
            std::filesystem::remove(entry.path());
        }
    }

private:
    std::filesystem::path path_;
};

// Requests made from the start of a steady stream, as the gateway would hand them to the
// journal thread. The frames are kept alive in `frames`, since a Request copies them.
struct BenchRequests {
    std::vector<pipeline::Request> requests;

    explicit BenchRequests(std::size_t count, std::size_t target_live = 1000) {
        testing::SteadyStream stream(1, target_live);
        requests.reserve(count);
        pipeline::Timestamp now = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const InboundFrame frame = stream.next();
            requests.push_back(pipeline::make_frame_request(frame.owner, frame.bytes, ++now));
        }
    }
};

// CRC32C over buffers of a few sizes, the table version against the instruction.
template <bool Hardware>
void BM_Crc32c(benchmark::State& state) {
    if (Hardware && !crc32c_hardware_available()) {
        state.SkipWithError("no CRC32C instruction on this CPU");
        return;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(state.range(0)));
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(i * 31U);
    }
    std::uint32_t crc = 0;
    for (auto _ : state) {
        crc = Hardware ? crc32c_hardware(crc, bytes) : crc32c_software(crc, bytes);
        benchmark::DoNotOptimize(crc);
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
    state.counters["ns/byte"] = benchmark::Counter(
        static_cast<double>(bytes.size()),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

// A write and a sync that do nothing, so append and commit cost no system call.
ssize_t pretend_write(int /*fd*/, const void* /*data*/, std::size_t length) {
    return static_cast<ssize_t>(length);
}
int pretend_sync(int /*fd*/) {
    return 0;
}

// Appending one Frame record, which encodes it and computes its CRC32C into the buffer.
// The writer's system calls are replaced by ones that do nothing, so this is the cost
// the journal thread pays per request before any I/O. Every 64 records it commits, which
// with the pretend write only resets the buffer.
void BM_JournalAppend(benchmark::State& state) {
    const BenchDirectory directory("append");
    const BenchRequests requests(4096);
    JournalWriterConfig config;
    config.ops = FileOps{.write = &pretend_write, .sync = &pretend_sync};
    JournalWriter writer = JournalWriter::create(directory.file("append.journal"), config);
    std::size_t i = 0;
    for (auto _ : state) {
        if (writer.append(requests.requests[i % requests.requests.size()]) != JournalStatus::Ok) {
            state.SkipWithError("append failed");
            break;
        }
        ++i;
        if (i % 64 == 0) {
            static_cast<void>(writer.commit());
        }
    }
}

// Appending a batch of records and committing it to a real file in /tmp. Write makes one
// write() per commit, Sync adds one fdatasync(). The thread sleeps inside fdatasync, so
// this and the rotate benchmark report wall time, not CPU time. Every 64 MB the file is started
// again with the timer paused, so the file stays a normal size.
void BM_JournalCommit(benchmark::State& state) {
    const auto policy = static_cast<SyncPolicy>(state.range(0));
    const auto batch = static_cast<std::size_t>(state.range(1));
    const BenchDirectory directory("commit");
    const BenchRequests requests(4096);
    JournalWriterConfig config;
    config.sync = policy;
    std::uint64_t file_number = 0;
    const auto open = [&] {
        directory.clear();
        ++file_number;
        return std::make_unique<JournalWriter>(JournalWriter::create(
            directory.file(("commit" + std::to_string(file_number) + ".journal").c_str()), config));
    };
    std::unique_ptr<JournalWriter> writer = open();
    constexpr std::uint64_t kFileLimit = std::uint64_t{64} << 20U;
    std::size_t next = 0;
    for (auto _ : state) {
        for (std::size_t k = 0; k < batch; ++k) {
            static_cast<void>(writer->append(requests.requests[next % requests.requests.size()]));
            ++next;
        }
        if (writer->commit() != JournalStatus::Ok) {
            state.SkipWithError("commit failed");
            break;
        }
        if (writer->stats().bytes_written > kFileLimit) {
            state.PauseTiming();
            writer = open();
            state.ResumeTiming();
        }
    }
    state.counters["records/commit"] = static_cast<double>(batch);
    state.counters["time/record"] = benchmark::Counter(
        static_cast<double>(batch),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

// One rotate() per iteration, after one record was written to the open segment with the
// timer paused. Under Write it creates the next file, writes its header and closes the
// old one. Under Sync it also syncs the old file, the new header and the directory.
void BM_JournalRotate(benchmark::State& state) {
    const auto policy = static_cast<SyncPolicy>(state.range(0));
    const BenchDirectory directory("rotate");
    const BenchRequests requests(64);
    PathBuffer paths(directory.path());
    JournalWriterConfig config;
    config.sync = policy;
    JournalWriter writer = JournalWriter::create(paths.segment(1), config);
    std::size_t i = 0;
    for (auto _ : state) {
        state.PauseTiming();
        static_cast<void>(writer.append(requests.requests[i % requests.requests.size()]));
        static_cast<void>(writer.commit());
        ++i;
        if (i % 1000 == 0) {
            directory.clear();
        }
        const char* path = paths.segment(writer.next_sequence());
        state.ResumeTiming();
        if (writer.rotate(path, paths.directory()) != JournalStatus::Ok) {
            state.SkipWithError("rotate failed");
            break;
        }
    }
}

// A journal of 100,000 steady stream requests (about 6,700 live orders at the end) in
// memory, written by the real writer. Built once and shared by every replay benchmark.
const std::vector<std::byte>& replay_journal_bytes() {
    static const std::vector<std::byte> bytes = [] {
        const BenchDirectory directory("replay");
        const BenchRequests requests(100'000, 10'000);
        const std::string path = directory.file("replay.journal");
        {
            JournalWriter writer = JournalWriter::create(path, JournalWriterConfig{});
            for (const JournalRecord& record : bench_setup_records()) {
                static_cast<void>(append_setup_record(writer, 0, record));
            }
            for (const pipeline::Request& request : requests.requests) {
                if (writer.append(request) == JournalStatus::BufferFull) {
                    static_cast<void>(writer.commit());
                    static_cast<void>(writer.append(request));
                }
            }
            static_cast<void>(writer.flush());
        }
        const MappedFile file(path);
        return std::vector<std::byte>(file.bytes().begin(), file.bytes().end());
    }();
    return bytes;
}

class NullSink final : public protocol::OutboundSink {
public:
    void send(OwnerId /*owner*/, std::span<const std::byte> frame) override {
        bytes += frame.size();
    }
    std::uint64_t bytes = 0;
};

// Replays the whole journal into a fresh exchange: reading each record, checking its
// CRC32C and handling its request. The setup records (which allocate the books) are
// applied with the timer paused.
template <OrderBook Book>
void BM_Replay(benchmark::State& state) {
    const std::vector<std::byte>& file = replay_journal_bytes();
    const std::span<const std::byte> records = std::span(file).subspan(kFileHeaderLength);
    const std::size_t setup = bench_setup_records().size();
    NullSink sink;
    std::uint64_t frames = 0;
    for (auto _ : state) {
        state.PauseTiming();
        auto replayer = std::make_unique<Replayer<Book>>(sink);
        RecordReader reader(records);
        JournalRecord record;
        for (std::size_t k = 0; k < setup; ++k) {
            static_cast<void>(reader.next(record));
            static_cast<void>(replayer->apply(record));
        }
        state.ResumeTiming();
        while (reader.next(record)) {
            if (replayer->apply(record) != ReplayStatus::Ok) {
                state.SkipWithError("replay failed");
                break;
            }
        }
        state.PauseTiming();
        frames = replayer->stats().frames;
        replayer.reset();
        state.ResumeTiming();
    }
    state.counters["records"] = static_cast<double>(frames);
    state.counters["time/record"] = benchmark::Counter(
        static_cast<double>(frames),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

// An exchange grown by a steady stream to about two thirds of `target` live orders, its
// snapshot and the snapshot's bytes. Built once per target.
struct SnapshotFixture {
    ReplyCounter replies;
    std::unique_ptr<OrderEntry<SortedOrderBook>> orders;
    Snapshot snapshot;
    std::vector<std::byte> bytes;
};

const SnapshotFixture& snapshot_fixture(std::size_t target) {
    static std::map<std::size_t, std::unique_ptr<SnapshotFixture>> cache;
    std::unique_ptr<SnapshotFixture>& slot = cache[target];
    if (slot) {
        return *slot;
    }
    slot = std::make_unique<SnapshotFixture>();
    SnapshotFixture& f = *slot;
    f.orders = std::make_unique<OrderEntry<SortedOrderBook>>(f.replies, bench_order_entry_config());
    list_stocks(*f.orders);
    testing::SteadyStream stream(2, target);
    protocol::Timestamp now = 0;
    for (std::size_t i = 0; i < 6 * target; ++i) {
        const InboundFrame frame = stream.next();
        static_cast<void>(f.orders->on_frame(frame.owner, frame.bytes, ++now));
    }
    f.snapshot.as_of = 6 * target;
    f.snapshot.as_of_time = now;
    f.snapshot.setup = bench_setup_records();
    f.snapshot.replies = f.replies.counts;
    f.orders->capture(f.snapshot.state);
    f.bytes.resize(snapshot_size(f.snapshot));
    if (encode_snapshot(f.snapshot, f.bytes) != f.bytes.size()) {
        f.bytes.clear();
    }
    return f;
}

void snapshot_counters(benchmark::State& state, const SnapshotFixture& f) {
    state.counters["live"] = static_cast<double>(f.orders->live_orders());
    state.counters["bytes"] = static_cast<double>(f.bytes.size());
}

// Capturing the exchange's visible state into a state that was reserved first, as the
// snapshot thread does after each segment.
void BM_SnapshotCapture(benchmark::State& state) {
    const SnapshotFixture& f = snapshot_fixture(static_cast<std::size_t>(state.range(0)));
    protocol::OrderEntryState captured;
    f.orders->reserve_state(captured);
    for (auto _ : state) {
        f.orders->capture(captured);
        benchmark::DoNotOptimize(captured.orders.data());
    }
    snapshot_counters(state, f);
}

// Encoding a snapshot into a buffer sized at startup, with its CRC32C.
void BM_SnapshotEncode(benchmark::State& state) {
    const SnapshotFixture& f = snapshot_fixture(static_cast<std::size_t>(state.range(0)));
    std::vector<std::byte> out(f.bytes.size());
    for (auto _ : state) {
        std::size_t written = encode_snapshot(f.snapshot, out);
        benchmark::DoNotOptimize(written);
    }
    snapshot_counters(state, f);
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(f.bytes.size()));
}

// Decoding a snapshot file's bytes, checksum and every field, as recovery does.
void BM_SnapshotDecode(benchmark::State& state) {
    const SnapshotFixture& f = snapshot_fixture(static_cast<std::size_t>(state.range(0)));
    Snapshot out;
    for (auto _ : state) {
        SnapshotStatus status = decode_snapshot(f.bytes, out);
        benchmark::DoNotOptimize(status);
    }
    snapshot_counters(state, f);
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(f.bytes.size()));
}

// Restoring a decoded state into a fresh exchange whose books were built with the timer
// paused. This includes restore()'s final capture and comparison.
void BM_SnapshotRestore(benchmark::State& state) {
    const SnapshotFixture& f = snapshot_fixture(static_cast<std::size_t>(state.range(0)));
    ReplyCounter replies;
    for (auto _ : state) {
        state.PauseTiming();
        auto fresh =
            std::make_unique<OrderEntry<SortedOrderBook>>(replies, bench_order_entry_config());
        list_stocks(*fresh);
        state.ResumeTiming();
        fresh->restore(f.snapshot.state);
        state.PauseTiming();
        fresh.reset();
        state.ResumeTiming();
    }
    snapshot_counters(state, f);
}

constexpr auto kWrite = static_cast<std::int64_t>(SyncPolicy::Write);
constexpr auto kSync = static_cast<std::int64_t>(SyncPolicy::Sync);

BENCHMARK_TEMPLATE(BM_Crc32c, false)->Arg(64)->Arg(256)->Arg(4096)->Arg(65536);
BENCHMARK_TEMPLATE(BM_Crc32c, true)->Arg(64)->Arg(256)->Arg(4096)->Arg(65536);
BENCHMARK(BM_JournalAppend);
BENCHMARK(BM_JournalCommit)
    ->ArgNames({"policy", "batch"})
    ->Args({kWrite, 1})
    ->Args({kWrite, 8})
    ->Args({kWrite, 64})
    ->Args({kSync, 1})
    ->Args({kSync, 8})
    ->Args({kSync, 64})
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_JournalRotate)
    ->ArgName("policy")
    ->Arg(kWrite)
    ->Arg(kSync)
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_Replay, FastOrderBook)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Replay, SortedOrderBook)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_Replay, MapOrderBook)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SnapshotCapture)->Arg(1500)->Arg(15'000)->Arg(150'000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_SnapshotEncode)->Arg(1500)->Arg(15'000)->Arg(150'000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_SnapshotDecode)->Arg(1500)->Arg(15'000)->Arg(150'000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_SnapshotRestore)->Arg(1500)->Arg(15'000)->Arg(150'000)->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace litbook::journal
