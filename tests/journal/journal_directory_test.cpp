// Tests for journal segments. JournalWriter::rotate() moves on to a new segment file, the
// directory helpers name and list the files, and scan_segment_chain() checks that the
// segments of a directory form one unbroken chain.

#include "litbook/journal/journal_directory.hpp"

#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/pipeline/messages.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "journal/journal_test_support.hpp"
#include "support/temp_dir.hpp"

namespace litbook::journal {
namespace {

using litbook::testing::TempDir;
using testing::sample_frame;

JournalWriterConfig small_config(SyncPolicy sync = SyncPolicy::Write) {
    JournalWriterConfig config;
    config.buffer_capacity = 4096;
    config.sync = sync;
    return config;
}

JournalScan scan_file(const std::string& path) {
    const MappedFile file(path);
    return scan_journal(file.bytes());
}

void append_frames(JournalWriter& writer, int count) {
    for (int i = 0; i < count; ++i) {
        ASSERT_EQ(writer.append(sample_frame(1, 1000 + static_cast<Timestamp>(i),
                                             {0x58, 0, 0, 0, 1, 0, 0, 0, 0})),
                  JournalStatus::Ok);
    }
}

// Writes a chain of segments of `per_segment` records each into `dir`.
void write_chain(const TempDir& dir, const std::vector<int>& per_segment) {
    PathBuffer paths(dir.path());
    JournalWriter writer = JournalWriter::create(paths.segment(1), small_config());
    for (std::size_t i = 0; i < per_segment.size(); ++i) {
        if (i > 0) {
            ASSERT_EQ(writer.rotate(paths.segment(writer.next_sequence()), paths.directory()),
                      JournalStatus::Ok);
        }
        append_frames(writer, per_segment[i]);
        ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    }
}

void write_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT: byte buffer to ostream
              static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) {
        bytes[i] = static_cast<std::byte>(chars[i]);
    }
    return bytes;
}

void cut_file(const std::string& path, std::size_t length) {
    std::filesystem::resize_file(path, length);
}

// Names and paths.

TEST(JournalDirectoryTest, NamesHaveTwentyDigitsSoTheySortInOrder) {
    EXPECT_EQ(segment_file_name(1), "00000000000000000001.journal");
    EXPECT_EQ(snapshot_file_name(1'000'000), "00000000000001000000.snapshot");
    EXPECT_EQ(segment_file_name(18'446'744'073'709'551'615ULL), "18446744073709551615.journal");
    EXPECT_LT(segment_file_name(9), segment_file_name(10));
}

TEST(JournalDirectoryTest, ThePathBufferBuildsEveryKindOfPath) {
    PathBuffer paths("/tmp/journal/");
    EXPECT_STREQ(paths.directory(), "/tmp/journal");
    EXPECT_STREQ(paths.segment(42), "/tmp/journal/00000000000000000042.journal");
    EXPECT_STREQ(paths.snapshot(7), "/tmp/journal/00000000000000000007.snapshot");
    EXPECT_STREQ(paths.temporary_snapshot(7), "/tmp/journal/00000000000000000007.snapshot.tmp");
}

TEST(JournalDirectoryTest, ThePathBufferRefusesAnEmptyOrHugeDirectory) {
    EXPECT_THROW(PathBuffer(""), std::length_error);
    EXPECT_THROW(PathBuffer(std::string(PATH_MAX, 'a')), std::length_error);
}

// The longest directory the constructor accepts still leaves room for every name whole,
// and one character more is refused.
TEST(JournalDirectoryTest, TheLongestAcceptedDirectoryStillGivesWholeNames) {
    const std::size_t room = 1 + kSequenceDigits + std::strlen(kTemporarySuffix) + 1;
    const std::string longest(PATH_MAX - room, 'd');
    PathBuffer paths(longest);
    const std::string temporary = paths.temporary_snapshot(42);
    EXPECT_EQ(temporary, longest + "/" + snapshot_file_name(42) + ".tmp");
    EXPECT_EQ(temporary.size(), PATH_MAX - 1U);
    EXPECT_EQ(std::string(paths.segment(42)), longest + "/" + segment_file_name(42));
    EXPECT_THROW(PathBuffer(longest + "d"), std::length_error);
}

TEST(JournalDirectoryTest, ListingSortsTheFilesAndIgnoresEverythingElse) {
    const TempDir dir;
    for (const char* name : {"00000000000000000010.journal", "00000000000000000001.journal",
                             "00000000000000000009.snapshot", "00000000000000000003.snapshot",
                             "00000000000000000004.snapshot.tmp", "notes.txt",
                             "0000000000000000001.journal", "0000000000000000000a.journal",
                             "99999999999999999999.journal", "00000000000000000002.journalx"}) {
        write_bytes(dir.file(name), {});
    }
    std::filesystem::create_directory(dir.file("00000000000000000005.journal"));
    const DirectoryListing listing = list_journal_directory(dir.path());
    ASSERT_EQ(listing.segments.size(), 2U);
    EXPECT_EQ(listing.segments[0].first, 1U);
    EXPECT_EQ(listing.segments[1].first, 10U);
    EXPECT_EQ(listing.segments[1].path, dir.file("00000000000000000010.journal"));
    ASSERT_EQ(listing.snapshots.size(), 2U);
    EXPECT_EQ(listing.snapshots[0].as_of, 3U);
    EXPECT_EQ(listing.snapshots[1].as_of, 9U);
    ASSERT_EQ(listing.temporary.size(), 1U);
    EXPECT_EQ(listing.temporary[0], dir.file("00000000000000000004.snapshot.tmp"));
}

TEST(JournalDirectoryTest, ListingAMissingDirectoryThrows) {
    const TempDir dir;
    EXPECT_THROW(static_cast<void>(list_journal_directory(dir.file("missing"))),
                 std::filesystem::filesystem_error);
}

// Rotation.

TEST(JournalRotateTest, TheNewSegmentStartsWithTheNextSequenceNumber) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    JournalWriter writer = JournalWriter::create(paths.segment(1), small_config());
    append_frames(writer, 3);
    ASSERT_EQ(writer.rotate(paths.segment(4), paths.directory()), JournalStatus::Ok);
    append_frames(writer, 2);
    ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    EXPECT_EQ(writer.stats().rotations, 1U);
    EXPECT_EQ(writer.next_sequence(), 6U);

    const JournalScan first = scan_file(paths.segment(1));
    EXPECT_EQ(first.tail, TailStatus::Clean);
    EXPECT_EQ(first.records, 3U);
    EXPECT_EQ(first.next_sequence, 4U);
    const JournalScan second = scan_file(paths.segment(4));
    EXPECT_EQ(second.tail, TailStatus::Clean);
    EXPECT_EQ(second.first_sequence, 4U);
    EXPECT_EQ(second.records, 2U);
    EXPECT_EQ(second.next_sequence, 6U);
}

TEST(JournalRotateTest, BufferedRecordsGoToTheOldSegmentEvenUnderBuffered) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    JournalWriter writer =
        JournalWriter::create(paths.segment(1), small_config(SyncPolicy::Buffered));
    append_frames(writer, 2);
    ASSERT_EQ(writer.commit(), JournalStatus::Ok);  // Buffered keeps them in memory.
    ASSERT_EQ(writer.rotate(paths.segment(3), paths.directory()), JournalStatus::Ok);
    EXPECT_EQ(scan_file(paths.segment(1)).records, 2U);
    EXPECT_EQ(writer.pending_bytes(), 0U);
}

TEST(JournalRotateTest, AnExistingFileIsNeverOverwrittenAndTheWriterStops) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    write_bytes(paths.segment(2), {std::byte{1}});
    JournalWriter writer = JournalWriter::create(paths.segment(1), small_config());
    append_frames(writer, 1);
    EXPECT_EQ(writer.rotate(paths.segment(2), paths.directory()), JournalStatus::WriteFailed);
    EXPECT_TRUE(writer.stopped());
    EXPECT_EQ(writer.stats().last_error, EEXIST);
    EXPECT_EQ(read_bytes(paths.segment(2)), std::vector<std::byte>{std::byte{1}});
    // The record appended before the rotation was flushed into the first segment.
    EXPECT_EQ(scan_file(paths.segment(1)).records, 1U);
    EXPECT_EQ(writer.append(sample_frame(1, 5, {0x58, 0, 0, 0, 1, 0, 0, 0, 0})),
              JournalStatus::Stopped);
}

TEST(JournalRotateTest, UnderSyncTheOldFileTheNewFileAndTheDirectoryAreSynced) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    JournalWriter writer = JournalWriter::create(paths.segment(1), small_config(SyncPolicy::Sync));
    const std::uint64_t before = writer.stats().syncs;
    append_frames(writer, 1);
    ASSERT_EQ(writer.rotate(paths.segment(2), paths.directory()), JournalStatus::Ok);
    // One sync for the old segment's last record, one for the new header, one for the
    // directory.
    EXPECT_EQ(writer.stats().syncs - before, 3U);
}

TEST(JournalRotateTest, ADirectoryThatCannotBeSyncedStopsTheWriter) {
    const TempDir dir;
    JournalWriter writer =
        JournalWriter::create(dir.file("first.journal"), small_config(SyncPolicy::Sync));
    append_frames(writer, 1);
    EXPECT_EQ(writer.rotate(dir.file("next.journal").c_str(), dir.file("missing").c_str()),
              JournalStatus::SyncFailed);
    EXPECT_TRUE(writer.stopped());
    EXPECT_EQ(writer.stats().last_error, ENOENT);
}

// Chains.

TEST(SegmentChainTest, AnEmptyDirectoryIsAnEmptyChain) {
    const TempDir dir;
    const ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
    EXPECT_EQ(chain.status, ChainStatus::Empty);
    EXPECT_TRUE(chain.torn_rotation.empty());
}

TEST(SegmentChainTest, SegmentsWrittenByRotationFormAChain) {
    const TempDir dir;
    write_chain(dir, {3, 4, 2, 0});
    const ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
    ASSERT_EQ(chain.status, ChainStatus::Ok);
    ASSERT_EQ(chain.segments.size(), 4U);
    EXPECT_EQ(chain.segments[1].file.first, 4U);
    EXPECT_EQ(chain.segments[2].file.first, 8U);
    EXPECT_EQ(chain.segments[3].file.first, 10U);
    EXPECT_EQ(chain.segments[3].scan.records, 0U);  // A segment with only its header.
    EXPECT_EQ(chain.records, 9U);
    EXPECT_EQ(chain.next_sequence, 10U);
}

TEST(JournalRotateTest, AFileWithNoRecordIsNotRotated) {
    const TempDir dir;
    PathBuffer paths(dir.path());
    JournalWriter writer = JournalWriter::create(paths.segment(1), small_config());
    EXPECT_EQ(writer.rotate(paths.segment(1), paths.directory()), JournalStatus::Ok);
    EXPECT_EQ(writer.stats().rotations, 0U);
    append_frames(writer, 2);
    ASSERT_EQ(writer.rotate(paths.segment(3), paths.directory()), JournalStatus::Ok);
    EXPECT_EQ(writer.file_first_sequence(), 3U);
    // Rotating again right away does nothing, because segment 3 is still empty.
    EXPECT_EQ(writer.rotate(paths.segment(3), paths.directory()), JournalStatus::Ok);
    EXPECT_EQ(writer.stats().rotations, 1U);
    EXPECT_FALSE(writer.stopped());
}

TEST(SegmentChainTest, TheFirstSegmentMayStartLaterAfterOldOnesWereDeleted) {
    const TempDir dir;
    write_chain(dir, {3, 4, 2});
    std::filesystem::remove(dir.file(segment_file_name(1)));
    const ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
    ASSERT_EQ(chain.status, ChainStatus::Ok);
    EXPECT_EQ(chain.segments.front().file.first, 4U);
    EXPECT_EQ(chain.records, 6U);
}

TEST(SegmentChainTest, AMissingSegmentInTheMiddleIsAGap) {
    const TempDir dir;
    write_chain(dir, {3, 4, 2});
    std::filesystem::remove(dir.file(segment_file_name(4)));
    const ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
    EXPECT_EQ(chain.status, ChainStatus::Gap);
    EXPECT_EQ(chain.problem_at, 1U);
}

TEST(SegmentChainTest, ASegmentNamedForAnotherSequenceIsRefused) {
    const TempDir dir;
    write_chain(dir, {3, 4});
    std::filesystem::rename(dir.file(segment_file_name(4)), dir.file(segment_file_name(5)));
    const ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
    EXPECT_EQ(chain.status, ChainStatus::NameMismatch);
    EXPECT_EQ(chain.problem_at, 1U);
}

TEST(SegmentChainTest, OnlyTheLastSegmentMayBeTorn) {
    const TempDir dir;
    write_chain(dir, {3, 4, 2});
    const std::string last = dir.file(segment_file_name(8));
    cut_file(last, std::filesystem::file_size(last) - 5);
    ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
    ASSERT_EQ(chain.status, ChainStatus::Ok);
    EXPECT_EQ(chain.segments.back().scan.tail, TailStatus::Torn);
    EXPECT_EQ(chain.next_sequence, 9U);

    const std::string middle = dir.file(segment_file_name(4));
    cut_file(middle, std::filesystem::file_size(middle) - 5);
    chain = scan_segment_chain(list_journal_directory(dir.path()));
    EXPECT_EQ(chain.status, ChainStatus::TornInside);
    EXPECT_EQ(chain.problem_at, 1U);
}

TEST(SegmentChainTest, DamageInsideASegmentIsRefused) {
    const TempDir dir;
    write_chain(dir, {3, 4, 2});
    const std::string middle = dir.file(segment_file_name(4));
    std::vector<std::byte> bytes = read_bytes(middle);
    bytes[kFileHeaderLength + 30] ^= std::byte{0xFF};  // Inside the first of its records.
    write_bytes(middle, bytes);
    const ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
    EXPECT_EQ(chain.status, ChainStatus::BadSegment);
    EXPECT_EQ(chain.problem_at, 1U);
}

// The header a segment whose first record is `first` starts with, from a real writer.
std::vector<std::byte> header_for(Sequence first) {
    const TempDir scratch;
    {
        const JournalWriter writer =
            JournalWriter::create(scratch.file("header"), small_config(), first);
    }
    return read_bytes(scratch.file("header"));
}

TEST(SegmentChainTest, ASegmentCutInsideItsHeaderAtTheEndIsATornRotation) {
    for (const std::size_t length : {std::size_t{0}, std::size_t{1}, std::size_t{31}}) {
        const TempDir dir;
        write_chain(dir, {3, 4});
        // The crash came while segment 8 was being created, after `length` bytes.
        std::vector<std::byte> header = header_for(8);
        ASSERT_EQ(header.size(), kFileHeaderLength);
        header.resize(length);
        write_bytes(dir.file(segment_file_name(8)), header);

        const ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
        ASSERT_EQ(chain.status, ChainStatus::Ok) << "length " << length;
        EXPECT_EQ(chain.torn_rotation, dir.file(segment_file_name(8)));
        EXPECT_EQ(chain.segments.size(), 2U);
        EXPECT_EQ(chain.next_sequence, 8U);
    }
}

TEST(SegmentChainTest, ACutHeaderThatIsNotTheExpectedOneIsRefused) {
    const TempDir dir;
    write_chain(dir, {3, 4});
    // A short file named for record 8 whose bytes are not a litbook header.
    write_bytes(dir.file(segment_file_name(8)), {std::byte{'X'}, std::byte{'Y'}});
    EXPECT_EQ(scan_segment_chain(list_journal_directory(dir.path())).status,
              ChainStatus::BadSegment);
    // A cut header in a segment named for the wrong number is not a torn rotation either.
    std::filesystem::remove(dir.file(segment_file_name(8)));
    write_bytes(dir.file(segment_file_name(9)), {});
    EXPECT_EQ(scan_segment_chain(list_journal_directory(dir.path())).status,
              ChainStatus::BadSegment);
}

TEST(SegmentChainTest, ACutHeaderInTheOnlySegmentLeavesAnEmptyChain) {
    const TempDir dir;
    write_bytes(dir.file(segment_file_name(1)), {});
    const ChainScan chain = scan_segment_chain(list_journal_directory(dir.path()));
    EXPECT_EQ(chain.status, ChainStatus::Empty);
    EXPECT_EQ(chain.torn_rotation, dir.file(segment_file_name(1)));
}

TEST(SegmentChainTest, StatusesPrintTheirNames) {
    std::ostringstream os;
    os << ChainStatus::Gap << ' ' << ChainStatus::TornInside;
    EXPECT_EQ(os.str(), "Gap TornInside");
}

}  // namespace
}  // namespace litbook::journal
