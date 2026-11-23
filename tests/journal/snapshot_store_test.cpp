// Tests for writing and reading snapshot files and for choosing which old files can go.

#include "litbook/journal/snapshot_store.hpp"

#include "litbook/journal/setup.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/temp_dir.hpp"

namespace litbook::journal {
namespace {

using litbook::testing::TempDir;

Snapshot sample(Sequence as_of) {
    protocol::OrderEntryConfig config;
    config.max_owners = 2;
    Snapshot s;
    s.as_of = as_of;
    s.as_of_time = 5'000;
    s.setup.push_back(session_start_record(config));
    s.replies = {0, 3, 4};
    s.state.last_user_ref = {0, 10, 20};
    return s;
}

std::vector<std::byte> encoded(const Snapshot& s) {
    std::vector<std::byte> out(snapshot_size(s));
    EXPECT_EQ(encode_snapshot(s, out), out.size());
    return out;
}

void touch(const std::string& path) {
    std::ofstream out(path);
    out << "x";
}

TEST(SnapshotStoreTest, AWrittenSnapshotReadsBackTheSame) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    for (const bool sync : {false, true}) {
        const Snapshot s = sample(sync ? 20 : 10);
        ASSERT_EQ(write_snapshot_file(paths, s.as_of, encoded(s), sync), 0);
        const LoadedSnapshot loaded =
            read_snapshot_file(SnapshotFile{.as_of = s.as_of, .path = paths.snapshot(s.as_of)});
        ASSERT_EQ(loaded.status, SnapshotStatus::Ok);
        EXPECT_EQ(loaded.snapshot, s);
    }
    const DirectoryListing listing = list_journal_directory(dir.path());
    EXPECT_EQ(listing.snapshots.size(), 2U);
    EXPECT_TRUE(listing.temporary.empty());
}

TEST(SnapshotStoreTest, ANewerWriteOfTheSameNumberReplacesTheFile) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    Snapshot s = sample(7);
    ASSERT_EQ(write_snapshot_file(paths, 7, encoded(s), false), 0);
    s.replies[1] = 99;
    ASSERT_EQ(write_snapshot_file(paths, 7, encoded(s), false), 0);
    EXPECT_EQ(
        read_snapshot_file(SnapshotFile{.as_of = 7, .path = paths.snapshot(7)}).snapshot.replies[1],
        99U);
}

TEST(SnapshotStoreTest, AFailedRenameLeavesNoTemporaryFile) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    // A directory with the final name makes the rename fail.
    std::filesystem::create_directory(paths.snapshot(3));
    std::filesystem::create_directory(std::string(paths.snapshot(3)) + "/inside");
    EXPECT_NE(write_snapshot_file(paths, 3, encoded(sample(3)), false), 0);
    EXPECT_FALSE(std::filesystem::exists(paths.temporary_snapshot(3)));
}

TEST(SnapshotStoreTest, AMissingDirectoryGivesItsErrno) {
    const TempDir dir;
    PathBuffer paths(dir.file("missing"));
    EXPECT_EQ(write_snapshot_file(paths, 3, encoded(sample(3)), false), ENOENT);
}

TEST(SnapshotStoreTest, AFileWhoseNameDisagreesWithItsContentsIsRefused) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    ASSERT_EQ(write_snapshot_file(paths, 8, encoded(sample(8)), false), 0);
    std::filesystem::rename(paths.snapshot(8), paths.snapshot(9));
    EXPECT_EQ(read_snapshot_file(SnapshotFile{.as_of = 9, .path = paths.snapshot(9)}).status,
              SnapshotStatus::BadHeader);
}

TEST(SnapshotStoreTest, ADamagedFileIsRefused) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    std::vector<std::byte> bytes = encoded(sample(8));
    bytes[50] ^= std::byte{1};
    ASSERT_EQ(write_snapshot_file(paths, 8, bytes, false), 0);
    EXPECT_EQ(read_snapshot_file(SnapshotFile{.as_of = 8, .path = paths.snapshot(8)}).status,
              SnapshotStatus::BadChecksum);
}

// A directory with segments starting at 1, 101, 201 and 301, and snapshots as of 100, 200
// and 300, plus a damaged snapshot as of 250 that is not in the good list.
class RetentionTest : public ::testing::Test {
protected:
    void SetUp() override {
        PathBuffer paths(dir_.path());
        for (const Sequence first : {Sequence{1}, Sequence{101}, Sequence{201}, Sequence{301}}) {
            touch(paths.segment(first));
        }
        for (const Sequence as_of : {Sequence{100}, Sequence{200}, Sequence{250}, Sequence{300}}) {
            touch(paths.snapshot(as_of));
        }
        listing_ = list_journal_directory(dir_.path());
        for (const SnapshotFile& file : listing_.snapshots) {
            if (file.as_of != 250) {
                good_.push_back(file);
            }
        }
    }

    [[nodiscard]] std::string segment(Sequence first) {
        PathBuffer paths(dir_.path());
        return paths.segment(first);
    }
    [[nodiscard]] std::string snapshot(Sequence as_of) {
        PathBuffer paths(dir_.path());
        return paths.snapshot(as_of);
    }

    TempDir dir_;
    DirectoryListing listing_;
    std::vector<SnapshotFile> good_;
};

TEST_F(RetentionTest, OlderSnapshotsAndTheSegmentsTheOldestKeptOneHoldsGo) {
    // Keeping 200 and 300, the snapshot as of 100 goes, and so do the segments that end
    // at 100 and at 200. Segment 201 holds records after 200 and stays.
    EXPECT_EQ(files_to_remove(listing_, good_, 2),
              (std::vector<std::string>{snapshot(100), segment(1), segment(101)}));
}

TEST_F(RetentionTest, KeepingOneSnapshotLeavesOnlyTheOpenSegment) {
    EXPECT_EQ(files_to_remove(listing_, good_, 1),
              (std::vector<std::string>{snapshot(100), snapshot(200), segment(1), segment(101),
                                        segment(201)}));
}

TEST_F(RetentionTest, KeepingMoreSnapshotsThanThereAreRemovesOnlyCoveredSegments) {
    EXPECT_EQ(files_to_remove(listing_, good_, 10), (std::vector<std::string>{segment(1)}));
}

TEST_F(RetentionTest, NothingGoesWithoutAGoodSnapshotOrWithKeepZero) {
    EXPECT_TRUE(files_to_remove(listing_, {}, 2).empty());
    EXPECT_TRUE(files_to_remove(listing_, good_, 0).empty());
}

TEST(RetentionEdgeTest, TheLastSegmentStaysEvenIfASnapshotHoldsAllOfIt) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    touch(paths.segment(1));
    touch(paths.snapshot(500));
    const DirectoryListing listing = list_journal_directory(dir.path());
    EXPECT_TRUE(files_to_remove(listing, listing.snapshots, 1).empty());
}

// The segment that holds the record right after the oldest kept snapshot is still needed,
// even when that record is its very last one.
TEST(RetentionEdgeTest, ASegmentWhoseLastRecordFollowsTheSnapshotStays) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    touch(paths.segment(1));   // Records 1 to 11.
    touch(paths.segment(12));  // Records 12 onwards.
    touch(paths.snapshot(10));
    const DirectoryListing listing = list_journal_directory(dir.path());
    EXPECT_TRUE(files_to_remove(listing, listing.snapshots, 1).empty());

    // As of record 11 the first segment is covered and can go.
    touch(paths.snapshot(11));
    const DirectoryListing later = list_journal_directory(dir.path());
    const std::vector<std::string> removed = files_to_remove(later, later.snapshots, 1);
    // Each PathBuffer call reuses one buffer, so every path is copied before the next.
    const std::string old_snapshot = paths.snapshot(10);
    const std::string first_segment = paths.segment(1);
    EXPECT_EQ(removed, (std::vector<std::string>{old_snapshot, first_segment}));
}

// A whole chain whose segments hold the records from `first` up to the one before `next`.
ChainScan chain_from(Sequence first, Sequence next) {
    ChainScan chain;
    chain.status = ChainStatus::Ok;
    SegmentScan segment;
    segment.scan.first_sequence = first;
    chain.segments.push_back(segment);
    chain.next_sequence = next;
    return chain;
}

TEST(SnapshotStoreTest, ASnapshotIsContinuedOnlyIfTheSegmentsHoldEveryLaterRecord) {
    // Records 11 to 30, as if the segment with records 1 to 10 had been removed.
    const ChainScan chain = chain_from(11, 31);
    EXPECT_FALSE(snapshot_is_continued(chain, 9));  // Record 10 is missing.
    EXPECT_TRUE(snapshot_is_continued(chain, 10));  // The first segment starts right after it.
    EXPECT_TRUE(snapshot_is_continued(chain, 20));
    EXPECT_TRUE(snapshot_is_continued(chain, 30));   // As of the very last record.
    EXPECT_FALSE(snapshot_is_continued(chain, 31));  // Claims a record nobody wrote.

    ChainScan broken = chain;
    broken.status = ChainStatus::Gap;
    EXPECT_FALSE(snapshot_is_continued(broken, 20));
    EXPECT_FALSE(snapshot_is_continued(ChainScan{}, 0));
}

}  // namespace
}  // namespace litbook::journal
