// Tests for replaying a whole journal directory offline. Each directory is written by
// hand with segments and snapshots, and every replay is compared with the reference the
// builder kept while writing.

#include "litbook/journal/directory_replay.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/snapshot_store.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "journal/journal_builder.hpp"
#include "journal/replay_test_support.hpp"
#include "support/temp_dir.hpp"

namespace litbook::journal {
namespace {

using Book = SortedOrderBook;
using testing::encoded;
using testing::JournalBuilder;
using testing::RecordedReply;
using testing::RecordingReplies;

// One replay of a directory, with the replies it produced.
struct DirectoryRun {
    RecordingReplies replies;
    Replayer<Book> replayer{replies};
    DirectoryReplay result;
};

void run(const std::string& directory, StartFrom start, DirectoryRun& out) {
    out.result = replay_directory(directory, out.replayer, start);
}

void write_snapshot(JournalBuilder& builder) {
    const Snapshot s = builder.snapshot();
    ASSERT_EQ(write_snapshot_file(builder.paths(), s.as_of, encoded(s), false), 0);
}

// The replay must end in the reference's state, and the replies it produced must be the
// reference's replies after the ones the snapshot already counted.
void expect_matches_reference(const DirectoryRun& run, const JournalBuilder& builder) {
    ASSERT_EQ(run.result.status, DirectoryStatus::Ok) << run.result.replay;
    ASSERT_TRUE(run.replayer.started());
    protocol::OrderEntryState replayed;
    protocol::OrderEntryState expected;
    run.replayer.order_entry().capture(replayed);
    builder.reference().capture(expected);
    EXPECT_EQ(replayed, expected);

    std::vector<std::uint64_t> skipped(testing::kReplayOwners + 1, 0);
    if (!run.result.snapshot_replies.empty()) {
        skipped = run.result.snapshot_replies;
    }
    std::vector<RecordedReply> expected_after;
    std::vector<std::uint64_t> seen(testing::kReplayOwners + 1, 0);
    for (const RecordedReply& reply : builder.replies()) {
        ++seen.at(reply.owner);
        if (seen.at(reply.owner) > skipped.at(reply.owner)) {
            expected_after.push_back(reply);
        }
    }
    EXPECT_EQ(run.replies.replies, expected_after);
}

void flip_byte(const std::string& path, std::streamoff offset) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(offset);
    char c = 0;
    file.get(c);
    file.seekp(offset);
    file.put(static_cast<char>(c ^ 0x5A));
}

TEST(DirectoryReplayTest, OneSegmentWithoutSnapshotsReplaysFromRecordOne) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(300);
    DirectoryRun r;
    run(dir.path(), StartFrom::NewestSnapshot, r);
    expect_matches_reference(r, builder);
    EXPECT_EQ(r.result.snapshot_as_of, 0U);
    EXPECT_TRUE(r.result.snapshot_replies.empty());
    EXPECT_EQ(r.result.setup.size(), testing::replay_setup_records().size());
    EXPECT_EQ(r.result.chain.segments.size(), 1U);
    EXPECT_EQ(r.replayer.stats().skipped, 0U);
}

TEST(DirectoryReplayTest, TheNewestUsableSnapshotIsUsedAndTheEarlierRecordsAreSkipped) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(200);
    write_snapshot(builder);
    builder.rotate();
    builder.append(200);
    write_snapshot(builder);
    const Sequence newest = builder.next_sequence() - 1;
    builder.rotate();
    builder.append(150);

    DirectoryRun r;
    run(dir.path(), StartFrom::NewestSnapshot, r);
    expect_matches_reference(r, builder);
    EXPECT_EQ(r.result.snapshot_as_of, newest);
    EXPECT_EQ(r.result.snapshot_replies.size(), testing::kReplayOwners + 1U);
    EXPECT_EQ(r.result.setup.size(), testing::replay_setup_records().size());
    EXPECT_EQ(r.replayer.stats().skipped, newest);
    ASSERT_EQ(r.result.snapshots.size(), 2U);
    EXPECT_TRUE(r.result.snapshots[0].usable());
    EXPECT_TRUE(r.result.snapshots[1].usable());
}

TEST(DirectoryReplayTest, FirstRecordIgnoresTheSnapshotsAndGivesTheSameState) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(250);
    write_snapshot(builder);
    builder.rotate();
    builder.append(250);

    DirectoryRun r;
    run(dir.path(), StartFrom::FirstRecord, r);
    expect_matches_reference(r, builder);
    EXPECT_EQ(r.result.snapshot_as_of, 0U);
    EXPECT_EQ(r.replies.replies.size(), builder.replies().size());
    EXPECT_EQ(r.replayer.stats().skipped, 0U);
}

TEST(DirectoryReplayTest, ADamagedNewestSnapshotFallsBackToTheOneBefore) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(200);
    write_snapshot(builder);
    const Sequence older = builder.next_sequence() - 1;
    builder.rotate();
    builder.append(200);
    write_snapshot(builder);
    const Sequence newer = builder.next_sequence() - 1;
    flip_byte(builder.paths().snapshot(newer), 60);

    DirectoryRun r;
    run(dir.path(), StartFrom::NewestSnapshot, r);
    expect_matches_reference(r, builder);
    EXPECT_EQ(r.result.snapshot_as_of, older);
    ASSERT_EQ(r.result.snapshots.size(), 2U);
    EXPECT_TRUE(r.result.snapshots[0].usable());
    EXPECT_EQ(r.result.snapshots[1].status, SnapshotStatus::BadChecksum);
    EXPECT_FALSE(r.result.snapshots[1].usable());
}

TEST(DirectoryReplayTest, ASnapshotBeyondTheEndOfTheSegmentsIsNotUsed) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(100);
    // A snapshot that claims records the segments do not hold, as if its segment had
    // been lost. It decodes, but nothing continues it.
    Snapshot ahead = builder.snapshot();
    ahead.as_of = builder.next_sequence() + 10;
    ASSERT_EQ(write_snapshot_file(builder.paths(), ahead.as_of, encoded(ahead), false), 0);

    DirectoryRun r;
    run(dir.path(), StartFrom::NewestSnapshot, r);
    expect_matches_reference(r, builder);
    EXPECT_EQ(r.result.snapshot_as_of, 0U);
    ASSERT_EQ(r.result.snapshots.size(), 1U);
    EXPECT_EQ(r.result.snapshots[0].status, SnapshotStatus::Ok);
    EXPECT_FALSE(r.result.snapshots[0].continued);
}

TEST(DirectoryReplayTest, RemovedOldSegmentsNeedTheSnapshot) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(200);
    builder.rotate();
    builder.append(100);
    write_snapshot(builder);
    builder.rotate();
    builder.append(100);
    // What retention does once the snapshot covers the first segment.
    std::filesystem::remove(builder.paths().segment(1));

    DirectoryRun from_snapshot;
    run(dir.path(), StartFrom::NewestSnapshot, from_snapshot);
    expect_matches_reference(from_snapshot, builder);

    DirectoryRun from_start;
    run(dir.path(), StartFrom::FirstRecord, from_start);
    EXPECT_EQ(from_start.result.status, DirectoryStatus::MissingStart);
    EXPECT_FALSE(from_start.replayer.started());

    // Without the snapshot nothing can rebuild the records of the first segment.
    std::filesystem::remove(builder.paths().snapshot(from_snapshot.result.snapshot_as_of));
    DirectoryRun without;
    run(dir.path(), StartFrom::NewestSnapshot, without);
    EXPECT_EQ(without.result.status, DirectoryStatus::MissingStart);
}

TEST(DirectoryReplayTest, AnEmptyDirectoryAndABrokenChainAreReported) {
    const litbook::testing::TempDir empty;
    DirectoryRun nothing;
    run(empty.path(), StartFrom::NewestSnapshot, nothing);
    EXPECT_EQ(nothing.result.status, DirectoryStatus::Empty);
    EXPECT_FALSE(nothing.replayer.started());

    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(100);
    const auto [first, second] = builder.rotate();
    builder.append(100);
    const auto [unused, third] = builder.rotate();
    builder.append(100);
    std::filesystem::remove(builder.paths().segment(second));  // A hole in the middle.
    DirectoryRun gap;
    run(dir.path(), StartFrom::NewestSnapshot, gap);
    EXPECT_EQ(gap.result.status, DirectoryStatus::BrokenChain);
    EXPECT_EQ(gap.result.chain.status, ChainStatus::Gap);
    EXPECT_FALSE(gap.replayer.started());
    EXPECT_EQ(first, 1U);
    EXPECT_GT(third, second);
    EXPECT_EQ(unused, second);
}

TEST(DirectoryReplayTest, ATornTailInTheLastSegmentIsLeftOut) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(120);
    builder.rotate();
    builder.append(80);
    const std::string last = builder.paths().segment(builder.next_sequence() - 80);
    {
        // Half of a record header, as a crash during a write leaves it.
        std::ofstream out(last, std::ios::app | std::ios::binary);
        const std::array<char, 6> half{'\x30', '\x00', '\x00', '\x00', 'F', '\x01'};
        out.write(half.data(), half.size());
    }
    DirectoryRun r;
    run(dir.path(), StartFrom::NewestSnapshot, r);
    expect_matches_reference(r, builder);
    EXPECT_EQ(r.result.chain.segments.back().scan.tail, TailStatus::Torn);
}

TEST(DirectoryReplayTest, ASnapshotTheReplayerRefusesIsReported) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(50);
    // Its setup has no Session Start, so the replayer cannot build an exchange from it.
    Snapshot broken = builder.snapshot();
    broken.setup.erase(broken.setup.begin());
    ASSERT_EQ(write_snapshot_file(builder.paths(), broken.as_of, encoded(broken), false), 0);

    DirectoryRun r;
    run(dir.path(), StartFrom::NewestSnapshot, r);
    EXPECT_EQ(r.result.status, DirectoryStatus::ReplayFailed);
    EXPECT_NE(r.result.replay, ReplayStatus::Ok);

    // Starting from record 1 does not read it.
    DirectoryRun from_start;
    run(dir.path(), StartFrom::FirstRecord, from_start);
    expect_matches_reference(from_start, builder);
}

}  // namespace
}  // namespace litbook::journal
