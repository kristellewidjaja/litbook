// The main check of snapshots. Starting from a snapshot taken after any record and
// replaying the rest of the journal must give exactly what a full replay gives: the same
// replies and market events after that point, the same state, the same answers to any
// future request, and the same snapshot at the end. It runs on all three books and from
// one book type into another, and every snapshot goes through the file format first.

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/journal/snapshot_format.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "book/book_test_support.hpp"
#include "journal/replay_test_support.hpp"
#include "support/temp_dir.hpp"

namespace litbook::journal {
namespace {

using testing::expect_same_state;
using testing::RecordingEvents;
using testing::RecordingReplies;
using testing::replay_order_entry_config;
using testing::replay_stream;

constexpr std::uint64_t kSeed = 99;
constexpr int kRequests = 6000;
constexpr std::size_t kSplitStep = 500;

std::vector<JournalRecord> setup_records() {
    return testing::replay_setup_records();
}

// Writes a journal with the setup and the requests, and reads its records back, so every
// test uses records exactly as they come out of a file.
std::vector<JournalRecord> journal_records(const std::vector<pipeline::Request>& requests) {
    const litbook::testing::TempDir dir;
    const std::string path = dir.file("journal");
    {
        JournalWriterConfig config;
        config.buffer_capacity = std::size_t{1} << 20;
        JournalWriter writer = JournalWriter::create(path, config);
        for (const JournalRecord& record : setup_records()) {
            EXPECT_EQ(append_setup_record(writer, 1, record), JournalStatus::Ok);
        }
        for (const pipeline::Request& request : requests) {
            EXPECT_EQ(writer.append(request), JournalStatus::Ok);
        }
        EXPECT_EQ(writer.flush(), JournalStatus::Ok);
    }
    const MappedFile file(path);
    const JournalScan scan = scan_journal(file.bytes());
    EXPECT_EQ(scan.tail, TailStatus::Clean);
    RecordReader reader(
        file.bytes().subspan(kFileHeaderLength, scan.valid_length - kFileHeaderLength));
    std::vector<JournalRecord> records;
    JournalRecord record;
    while (reader.next(record)) {
        records.push_back(record);
    }
    return records;
}

// A replayer with sinks that record what it sends.
template <class Book>
struct ReplayRun {
    RecordingReplies replies;
    pipeline::Timestamp now = 0;
    RecordingEvents events{&now};
    Replayer<Book> replayer{replies, &events};

    void apply(const JournalRecord& record) {
        now = record.now;
        ASSERT_EQ(replayer.apply(record), ReplayStatus::Ok) << "record " << record.sequence;
    }
    void apply_all(const std::vector<JournalRecord>& records, std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i) {
            apply(records[i]);
        }
    }

    // A snapshot as of the last record applied. Its setup records lose their sequence
    // numbers and timestamps, as they do in the file.
    [[nodiscard]] Snapshot snapshot(const std::vector<JournalRecord>& records,
                                    std::size_t applied) {
        Snapshot s;
        s.as_of = records[applied - 1].sequence;
        s.as_of_time = records[applied - 1].now;
        for (JournalRecord record : setup_records()) {
            record.sequence = 0;
            record.now = 0;
            s.setup.push_back(record);
        }
        s.replies.assign(static_cast<std::size_t>(replay_order_entry_config().max_owners) + 1, 0);
        for (const testing::RecordedReply& reply : replies.replies) {
            ++s.replies.at(reply.owner);
        }
        replayer.order_entry().capture(s.state);
        return s;
    }
};

// Sends the same requests after the journal to two order entry layers: more of the
// random stream, then a Cancel All for every account, which walks every account's list.
template <class A, class B>
void expect_same_future(ReplayRun<A>& a, ReplayRun<B>& b, pipeline::Timestamp after) {
    a.replies.replies.clear();
    b.replies.replies.clear();
    a.events.events.clear();
    b.events.events.clear();
    for (const pipeline::Request& request : replay_stream(kSeed + 1, 400, after + 1)) {
        a.now = request.now;
        b.now = request.now;
        testing::apply_request(a.replayer.order_entry(), request);
        testing::apply_request(b.replayer.order_entry(), request);
    }
    for (OwnerId owner = 1; owner <= replay_order_entry_config().max_owners; ++owner) {
        a.replayer.order_entry().cancel_all(owner, after + 10'000'000);
        b.replayer.order_entry().cancel_all(owner, after + 10'000'000);
    }
    EXPECT_EQ(a.replies.replies, b.replies.replies);
    EXPECT_EQ(a.events.events, b.events.events);
}

std::vector<std::byte> encoded(const Snapshot& s) {
    std::vector<std::byte> out(snapshot_size(s));
    EXPECT_EQ(encode_snapshot(s, out), out.size());
    return out;
}

// Takes a snapshot after `split` records on a From book, writes and reads it, starts a To
// book from it, gives it the WHOLE journal (it must skip what the snapshot holds), and
// compares everything with a full replay on a From book.
template <class From, class To>
void expect_snapshot_plus_replay_equals_full_replay(const std::vector<JournalRecord>& records,
                                                    std::size_t split) {
    SCOPED_TRACE("split after " + std::to_string(split) + " records");
    ReplayRun<From> full;
    full.apply_all(records, 0, records.size());

    ReplayRun<From> before;
    before.apply_all(records, 0, split);
    const std::size_t replies_before = before.replies.replies.size();
    const std::size_t events_before = before.events.events.size();
    Snapshot decoded;
    ASSERT_EQ(decode_snapshot(encoded(before.snapshot(records, split)), decoded),
              SnapshotStatus::Ok);

    ReplayRun<To> after;
    ASSERT_EQ(after.replayer.start_from(decoded), ReplayStatus::Ok);
    after.apply_all(records, 0, records.size());
    EXPECT_EQ(after.replayer.stats().skipped, split);
    EXPECT_EQ(after.replayer.stats().last_sequence, records.back().sequence);

    const std::vector<testing::RecordedReply> later_replies(
        full.replies.replies.begin() + static_cast<std::ptrdiff_t>(replies_before),
        full.replies.replies.end());
    const std::vector<testing::RecordedEvent> later_events(
        full.events.events.begin() + static_cast<std::ptrdiff_t>(events_before),
        full.events.events.end());
    EXPECT_EQ(after.replies.replies, later_replies);
    EXPECT_EQ(after.events.events, later_events);
    expect_same_state(full.replayer.order_entry(), after.replayer.order_entry());

    // A snapshot of both at the end is the same file, byte for byte. The restored run only
    // saw the replies after the split, so both use the full run's reply counts.
    const Snapshot full_end = full.snapshot(records, records.size());
    Snapshot after_end = after.snapshot(records, records.size());
    after_end.replies = full_end.replies;
    EXPECT_EQ(encoded(after_end), encoded(full_end));

    expect_same_future(full, after, records.back().now);
}

// Every 500th record, plus the first point after the setup and the very end.
std::vector<std::size_t> split_points(const std::vector<JournalRecord>& records) {
    std::vector<std::size_t> splits{setup_records().size()};
    for (std::size_t k = kSplitStep; k < records.size(); k += kSplitStep) {
        splits.push_back(k);
    }
    splits.push_back(records.size());
    return splits;
}

template <class Book>
class SnapshotReplayTest : public ::testing::Test {};
TYPED_TEST_SUITE(SnapshotReplayTest, litbook::testing::BookTypes, litbook::testing::BookTypeNames);

TYPED_TEST(SnapshotReplayTest, SnapshotPlusReplayEqualsAFullReplayAtEverySplit) {
    const std::vector<JournalRecord> records = journal_records(replay_stream(kSeed, kRequests));
    for (const std::size_t split : split_points(records)) {
        expect_snapshot_plus_replay_equals_full_replay<TypeParam, TypeParam>(records, split);
    }
}

TEST(SnapshotReplayAcrossBooksTest, ASnapshotOfOneBookTypeReplaysIntoTheOthers) {
    const std::vector<JournalRecord> records = journal_records(replay_stream(kSeed, kRequests));
    for (const std::size_t split : {std::size_t{2000}, std::size_t{4500}}) {
        expect_snapshot_plus_replay_equals_full_replay<MapOrderBook, FastOrderBook>(records, split);
        expect_snapshot_plus_replay_equals_full_replay<MapOrderBook, SortedOrderBook>(records,
                                                                                      split);
        expect_snapshot_plus_replay_equals_full_replay<SortedOrderBook, MapOrderBook>(records,
                                                                                      split);
    }
}

TEST(SnapshotReplayStatusTest, AReplayThatStartedCannotStartFromASnapshot) {
    const std::vector<JournalRecord> records = journal_records(replay_stream(kSeed, 100));
    ReplayRun<SortedOrderBook> source;
    source.apply_all(records, 0, records.size());
    const Snapshot s = source.snapshot(records, records.size());
    ReplayRun<SortedOrderBook> started;
    started.apply(records[0]);
    EXPECT_EQ(started.replayer.start_from(s), ReplayStatus::AlreadyStarted);
    EXPECT_EQ(started.replayer.start_from(s), ReplayStatus::AlreadyStarted);
}

TEST(SnapshotReplayStatusTest, ASnapshotWhoseStateDoesNotFitIsRefused) {
    const std::vector<JournalRecord> records = journal_records(replay_stream(kSeed, 300));
    ReplayRun<SortedOrderBook> source;
    source.apply_all(records, 0, records.size());
    Snapshot s = source.snapshot(records, records.size());
    ASSERT_FALSE(s.state.orders.empty());
    ++s.state.orders[0].open;
    ReplayRun<SortedOrderBook> fresh;
    EXPECT_EQ(fresh.replayer.start_from(s), ReplayStatus::SnapshotRefused);
    EXPECT_EQ(fresh.replayer.apply(records.back()), ReplayStatus::SnapshotRefused);
}

TEST(SnapshotReplayStatusTest, ASnapshotWithoutASessionStartIsRefused) {
    const std::vector<JournalRecord> records = journal_records(replay_stream(kSeed, 50));
    ReplayRun<SortedOrderBook> source;
    source.apply_all(records, 0, records.size());
    Snapshot s = source.snapshot(records, records.size());
    s.setup.clear();
    ReplayRun<SortedOrderBook> fresh;
    EXPECT_EQ(fresh.replayer.start_from(s), ReplayStatus::MissingSessionStart);
}

TEST(SnapshotReplayStatusTest, ASetupRecordAfterASnapshotIsRefused) {
    const std::vector<JournalRecord> records = journal_records(replay_stream(kSeed, 50));
    ReplayRun<SortedOrderBook> source;
    source.apply_all(records, 0, records.size());
    ReplayRun<SortedOrderBook> fresh;
    ASSERT_EQ(fresh.replayer.start_from(source.snapshot(records, records.size())),
              ReplayStatus::Ok);
    JournalRecord late = risk_limits_record(1, testing::kLimitedRisk);
    late.sequence = records.back().sequence + 1;
    EXPECT_EQ(fresh.replayer.apply(late), ReplayStatus::SetupAfterRequests);
}

}  // namespace
}  // namespace litbook::journal
