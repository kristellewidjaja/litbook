#include "litbook/journal/journal_reader.hpp"

#include "litbook/journal/journal_format.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "journal/journal_test_support.hpp"
#include "support/temp_dir.hpp"

namespace litbook::journal {
namespace {

using testing::encoded;
using testing::sample_listing;

// A journal in memory: a header and `records` records numbered from `first`, mixing
// setup records, frames of every length, cancels and a stop. It also remembers where each
// record starts.
struct MemoryJournal {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> starts;
};

MemoryJournal make_journal(int records, Sequence first = 1) {
    MemoryJournal j;
    j.bytes.resize(kFileHeaderLength);
    encode_file_header(j.bytes, first);
    for (int i = 0; i < records; ++i) {
        const Sequence seq = first + static_cast<Sequence>(i);
        const Timestamp now = 1000 + static_cast<Timestamp>(i);
        std::vector<std::byte> r;
        if (i == 0) {
            r = encoded([&](auto o) {
                return encode_session_start(o, seq, now, protocol::OrderEntryConfig{});
            });
        } else if (i == 1) {
            r = encoded([&](auto o) { return encode_add_symbol(o, seq, now, sample_listing()); });
        } else if (i % 7 == 0) {
            r = encoded([&](auto o) {
                return encode_request(o, seq, pipeline::make_cancel_all_request(2, now));
            });
        } else if (i % 11 == 0) {
            r = encoded(
                [&](auto o) { return encode_request(o, seq, pipeline::make_stop_request(now)); });
        } else {
            std::vector<std::byte> frame(1 + static_cast<std::size_t>(i) % kMaxPayload,
                                         static_cast<std::byte>(i));
            r = encoded([&](auto o) {
                return encode_request(o, seq, pipeline::make_frame_request(1, frame, now));
            });
        }
        j.starts.push_back(j.bytes.size());
        j.bytes.insert(j.bytes.end(), r.begin(), r.end());
    }
    return j;
}

TEST(JournalScanTest, ACleanJournal) {
    const MemoryJournal j = make_journal(40, 5);
    const JournalScan s = scan_journal(j.bytes);
    EXPECT_EQ(s.header, HeaderStatus::Ok);
    EXPECT_EQ(s.tail, TailStatus::Clean);
    EXPECT_EQ(s.first_sequence, 5U);
    EXPECT_EQ(s.records, 40U);
    EXPECT_EQ(s.next_sequence, 45U);
    EXPECT_EQ(s.valid_length, j.bytes.size());
    EXPECT_EQ(s.file_length, j.bytes.size());
    EXPECT_EQ(s.stop_reason, RecordStatus::Ok);
    EXPECT_FALSE(s.sequence_break);
}

TEST(JournalScanTest, AHeaderWithNoRecordsIsClean) {
    const MemoryJournal j = make_journal(0, 3);
    const JournalScan s = scan_journal(j.bytes);
    EXPECT_EQ(s.tail, TailStatus::Clean);
    EXPECT_EQ(s.records, 0U);
    EXPECT_EQ(s.next_sequence, 3U);
    EXPECT_EQ(s.valid_length, kFileHeaderLength);
}

TEST(JournalScanTest, AnEmptyOrDamagedHeaderCannotBeRead) {
    EXPECT_EQ(scan_journal({}).tail, TailStatus::BadHeader);
    EXPECT_EQ(scan_journal({}).header, HeaderStatus::Incomplete);
    MemoryJournal j = make_journal(3);
    j.bytes[0] = std::byte{'X'};
    const JournalScan s = scan_journal(j.bytes);
    EXPECT_EQ(s.tail, TailStatus::BadHeader);
    EXPECT_EQ(s.header, HeaderStatus::BadMagic);
    EXPECT_EQ(s.valid_length, 0U);
}

TEST(JournalScanTest, AJournalCutInsideItsLastRecordHasATornTail) {
    // This is what a crash in the middle of the last write leaves behind. Every cut point
    // inside the last record gives back exactly the records before it.
    const MemoryJournal j = make_journal(30);
    const std::size_t last = j.starts.back();
    for (std::size_t cut = last + 1; cut < j.bytes.size(); ++cut) {
        const JournalScan s = scan_journal(std::span<const std::byte>(j.bytes).first(cut));
        ASSERT_EQ(s.tail, TailStatus::Torn) << "cut at " << cut;
        EXPECT_EQ(s.valid_length, last);
        EXPECT_EQ(s.records, 29U);
        EXPECT_EQ(s.next_sequence, 30U);
        EXPECT_EQ(s.stop_reason, RecordStatus::Incomplete);
    }
    // A cut exactly between records is just a shorter clean journal.
    const JournalScan s = scan_journal(std::span<const std::byte>(j.bytes).first(last));
    EXPECT_EQ(s.tail, TailStatus::Clean);
}

TEST(JournalScanTest, GarbageAfterTheLastRecordIsATornTail) {
    // Zeros, as a file preallocated past its end would hold, and a record whose bytes
    // all arrived but whose checksum is wrong (a block that never reached the disk).
    MemoryJournal zeros = make_journal(10);
    const std::size_t good = zeros.bytes.size();
    zeros.bytes.resize(good + 300);
    JournalScan s = scan_journal(zeros.bytes);
    EXPECT_EQ(s.tail, TailStatus::Torn);
    EXPECT_EQ(s.valid_length, good);
    EXPECT_EQ(s.stop_reason, RecordStatus::BadLength);

    MemoryJournal damaged = make_journal(10);
    damaged.bytes.back() ^= std::byte{0xFF};
    s = scan_journal(damaged.bytes);
    EXPECT_EQ(s.tail, TailStatus::Torn);
    EXPECT_EQ(s.valid_length, damaged.starts.back());
    EXPECT_EQ(s.stop_reason, RecordStatus::BadChecksum);
}

TEST(JournalScanTest, DamageFollowedByGoodRecordsIsCorruption) {
    // Recovery must not cut off good records, so damage anywhere but the end is refused.
    const MemoryJournal original = make_journal(25);
    for (const int damaged_record : {0, 1, 7, 12, 23}) {
        MemoryJournal j = original;
        const std::size_t at = j.starts[static_cast<std::size_t>(damaged_record)];
        j.bytes[at + 10] ^= std::byte{0x40};
        const JournalScan s = scan_journal(j.bytes);
        EXPECT_EQ(s.tail, TailStatus::Corrupt) << "record " << damaged_record;
        EXPECT_EQ(s.valid_length, at);
        EXPECT_EQ(s.records, static_cast<std::uint64_t>(damaged_record));
        EXPECT_EQ(s.stop_reason, RecordStatus::BadChecksum);
        EXPECT_EQ(s.corrupt_at, j.starts[static_cast<std::size_t>(damaged_record) + 1]);
    }
}

TEST(JournalScanTest, AWrongSequenceNumberIsCorruption) {
    // Two journals glued together, or a record copied twice, both show up as a good
    // record with the wrong number.
    MemoryJournal j = make_journal(5);
    const auto extra = encoded(
        [&](auto o) { return encode_request(o, 9, pipeline::make_cancel_all_request(1, 0)); });
    j.bytes.insert(j.bytes.end(), extra.begin(), extra.end());
    const JournalScan s = scan_journal(j.bytes);
    EXPECT_EQ(s.tail, TailStatus::Corrupt);
    EXPECT_TRUE(s.sequence_break);
    EXPECT_EQ(s.records, 5U);
    EXPECT_EQ(s.corrupt_at, s.valid_length);

    // The same record repeated.
    MemoryJournal twice = make_journal(5);
    twice.bytes.insert(twice.bytes.end(),
                       twice.bytes.begin() + static_cast<std::ptrdiff_t>(twice.starts.back()),
                       twice.bytes.end());
    EXPECT_EQ(scan_journal(twice.bytes).tail, TailStatus::Corrupt);
}

TEST(JournalScanTest, ARecordHiddenInsideAFrameMakesATornTailLookCorrupt) {
    // A frame's payload is whatever the client sent, so it can hold the bytes of a whole
    // record with a good checksum (a checksum proves nothing about who wrote the bytes).
    // If such a frame is the record a crash cut short, the scan finds that hidden record
    // after the cut and calls the journal corrupt. That is the safe mistake to make.
    // Recovery refuses to start and nothing good is ever cut off. The journal fuzzer
    // found this (fuzz/journal_regressions/record-inside-a-frame.bin).
    MemoryJournal j = make_journal(5);
    const auto hidden = encoded(
        [&](auto o) { return encode_request(o, 77, pipeline::make_cancel_all_request(1, 0)); });
    std::vector<std::byte> payload(3, std::byte{'x'});
    payload.insert(payload.end(), hidden.begin(), hidden.end());
    payload.resize(kMaxPayload, std::byte{'y'});
    const auto frame = encoded(
        [&](auto o) { return encode_request(o, 6, pipeline::make_frame_request(1, payload, 0)); });
    const std::size_t frame_start = j.bytes.size();
    j.bytes.insert(j.bytes.end(), frame.begin(), frame.end());

    // Whole, the frame is just a good record.
    EXPECT_EQ(scan_journal(j.bytes).tail, TailStatus::Clean);
    // Cut after the hidden record, the tail is torn but looks corrupt.
    const std::size_t hidden_start = frame_start + kRecordHeaderLength + 3;
    const auto cut = std::span<const std::byte>(j.bytes).first(hidden_start + hidden.size() + 2);
    const JournalScan s = scan_journal(cut);
    EXPECT_EQ(s.tail, TailStatus::Corrupt);
    EXPECT_EQ(s.valid_length, frame_start);
    EXPECT_EQ(s.corrupt_at, hidden_start);
    // Cut before the hidden record ends, it is an ordinary torn tail.
    EXPECT_EQ(scan_journal(std::span<const std::byte>(j.bytes).first(hidden_start + 10)).tail,
              TailStatus::Torn);
}

TEST(JournalScanTest, TheFirstRecordMustCarryTheHeaderSequence) {
    MemoryJournal j = make_journal(3, 1);
    // Claim the records start at 2, which they do not.
    protocol::store_le(std::span<std::byte>(j.bytes), 16, std::uint64_t{2});
    testing::reseal_header(j.bytes);
    const JournalScan s = scan_journal(j.bytes);
    EXPECT_EQ(s.tail, TailStatus::Corrupt);
    EXPECT_TRUE(s.sequence_break);
    EXPECT_EQ(s.records, 0U);
}

TEST(RecordReaderTest, ReadsEveryRecordInOrder) {
    const MemoryJournal j = make_journal(20, 100);
    RecordReader reader(std::span<const std::byte>(j.bytes).subspan(kFileHeaderLength));
    JournalRecord r;
    Sequence expected = 100;
    while (reader.next(r)) {
        EXPECT_EQ(r.sequence, expected);
        ++expected;
    }
    EXPECT_EQ(expected, 120U);
    EXPECT_EQ(reader.status(), RecordStatus::Ok);
    EXPECT_EQ(reader.offset(), j.bytes.size() - kFileHeaderLength);
    EXPECT_FALSE(reader.next(r));
}

TEST(RecordReaderTest, StopsAtABadRecordAndStaysStopped) {
    MemoryJournal j = make_journal(6);
    j.bytes[j.starts[3] + 5] ^= std::byte{1};
    RecordReader reader(std::span<const std::byte>(j.bytes).subspan(kFileHeaderLength));
    JournalRecord r;
    int read = 0;
    while (reader.next(r)) {
        ++read;
    }
    EXPECT_EQ(read, 3);
    EXPECT_EQ(reader.status(), RecordStatus::BadChecksum);
    EXPECT_EQ(reader.offset(), j.starts[3] - kFileHeaderLength);
    EXPECT_FALSE(reader.next(r));
    EXPECT_EQ(reader.status(), RecordStatus::BadChecksum);
}

TEST(RecordReaderTest, NoBytesMeansNoRecords) {
    RecordReader reader({});
    JournalRecord r;
    EXPECT_FALSE(reader.next(r));
    EXPECT_EQ(reader.status(), RecordStatus::Ok);
}

void write_file(const std::string& path, const std::vector<std::byte>& bytes) {
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::fopen(path.c_str(), "wb"),
                                                            &std::fclose);
    ASSERT_NE(f, nullptr);
    if (!bytes.empty()) {
        ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), f.get()), bytes.size());
    }
}

TEST(MappedFileTest, MapsTheWholeFile) {
    const litbook::testing::TempDir dir;
    const MemoryJournal j = make_journal(50);
    write_file(dir.file("a.journal"), j.bytes);
    const MappedFile file(dir.file("a.journal"));
    ASSERT_EQ(file.bytes().size(), j.bytes.size());
    EXPECT_TRUE(std::equal(file.bytes().begin(), file.bytes().end(), j.bytes.begin()));
    EXPECT_EQ(scan_journal(file.bytes()).tail, TailStatus::Clean);
}

TEST(MappedFileTest, AnEmptyFileGivesNoBytes) {
    const litbook::testing::TempDir dir;
    write_file(dir.file("empty"), {});
    const MappedFile file(dir.file("empty"));
    EXPECT_TRUE(file.bytes().empty());
    EXPECT_EQ(scan_journal(file.bytes()).tail, TailStatus::BadHeader);
}

TEST(MappedFileTest, AMissingFileThrows) {
    const litbook::testing::TempDir dir;
    EXPECT_THROW(MappedFile(dir.file("missing")), std::system_error);
}

TEST(MappedFileTest, MovingHandsTheMappingOver) {
    const litbook::testing::TempDir dir;
    const MemoryJournal j = make_journal(4);
    write_file(dir.file("a.journal"), j.bytes);
    MappedFile first(dir.file("a.journal"));
    const std::byte* data = first.bytes().data();
    MappedFile second(std::move(first));
    EXPECT_EQ(second.bytes().data(), data);
    EXPECT_EQ(second.bytes().size(), j.bytes.size());
    MappedFile third(dir.file("a.journal"));
    third = std::move(second);
    EXPECT_EQ(third.bytes().data(), data);
}

}  // namespace
}  // namespace litbook::journal
