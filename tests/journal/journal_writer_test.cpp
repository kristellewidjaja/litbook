#include "litbook/journal/journal_writer.hpp"

#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "journal/journal_test_support.hpp"
#include "support/temp_dir.hpp"

namespace litbook::journal {
namespace {

using litbook::testing::TempDir;
using testing::sample_listing;

// What the fake system calls do. Plain function pointers cannot capture anything, so the
// fakes read this one object, which every test resets in SetUp.
struct Fakes {
    std::size_t chunk = 0;  // If not 0, a write takes at most this many bytes.
    bool interrupt_every_other = false;
    bool interrupted_last = false;
    std::size_t fail_after = 0;  // If not 0, writes fail with ENOSPC once this many bytes went.
    std::size_t written = 0;
    bool return_zero = false;  // Every write takes 0 bytes.
    int syncs = 0;
    int fail_syncs = 0;  // The next this many syncs fail with EIO.
};

Fakes fakes;

ssize_t fake_write(int fd, const void* data, std::size_t length) {
    if (fakes.return_zero) {
        return 0;
    }
    if (fakes.interrupt_every_other) {
        fakes.interrupted_last = !fakes.interrupted_last;
        if (fakes.interrupted_last) {
            errno = EINTR;
            return -1;
        }
    }
    if (fakes.fail_after != 0 && fakes.written >= fakes.fail_after) {
        errno = ENOSPC;
        return -1;
    }
    std::size_t n = length;
    if (fakes.chunk != 0) {
        n = std::min(n, fakes.chunk);
    }
    if (fakes.fail_after != 0) {
        n = std::min(n, fakes.fail_after - fakes.written);
    }
    const ssize_t taken = ::write(fd, data, n);
    if (taken > 0) {
        fakes.written += static_cast<std::size_t>(taken);
    }
    return taken;
}

int fake_sync(int fd) {
    ++fakes.syncs;
    if (fakes.fail_syncs > 0) {
        --fakes.fail_syncs;
        errno = EIO;
        return -1;
    }
    return ::fdatasync(fd);
}

JournalWriterConfig config_with(SyncPolicy sync, std::size_t capacity = 4096) {
    JournalWriterConfig config;
    config.buffer_capacity = capacity;
    config.sync = sync;
    config.ops.write = &fake_write;
    config.ops.sync = &fake_sync;
    return config;
}

std::vector<std::byte> read_file(const std::string& path) {
    const MappedFile file(path);
    return {file.bytes().begin(), file.bytes().end()};
}

std::size_t file_size(const std::string& path) {
    return static_cast<std::size_t>(std::filesystem::file_size(path));
}

// The records of a journal file, decoded.
std::vector<JournalRecord> records_of(const std::string& path) {
    const std::vector<std::byte> bytes = read_file(path);
    std::vector<JournalRecord> out;
    RecordReader reader(std::span<const std::byte>(bytes).subspan(kFileHeaderLength));
    JournalRecord r;
    while (reader.next(r)) {
        out.push_back(r);
    }
    return out;
}

// A request for test number i: frames of growing length, with a cancel and a stop now
// and then.
pipeline::Request request_number(int i) {
    const Timestamp now = 10'000 + static_cast<Timestamp>(i);
    if (i % 9 == 4) {
        return pipeline::make_cancel_all_request(static_cast<OwnerId>(1 + i % 3), now);
    }
    std::vector<std::byte> frame(1 + static_cast<std::size_t>(i) % kMaxPayload,
                                 static_cast<std::byte>(i));
    return pipeline::make_frame_request(static_cast<OwnerId>(1 + i % 3), frame, now);
}

class JournalWriterTest : public ::testing::Test {
protected:
    void SetUp() override { fakes = Fakes{}; }
    TempDir dir_;
    std::string path_ = dir_.file("test.journal");
};

TEST_F(JournalWriterTest, CreateWritesOnlyTheHeader) {
    const JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write), 7);
    EXPECT_EQ(file_size(path_), kFileHeaderLength);
    const JournalScan s = scan_journal(read_file(path_));
    EXPECT_EQ(s.tail, TailStatus::Clean);
    EXPECT_EQ(s.first_sequence, 7U);
    EXPECT_EQ(writer.next_sequence(), 7U);
    EXPECT_EQ(writer.pending_bytes(), 0U);
    EXPECT_EQ(writer.stats().bytes_written, kFileHeaderLength);
}

TEST_F(JournalWriterTest, ARestartRecordTakesTheNextNumberAndHasNoOwner) {
    {
        JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
        ASSERT_EQ(writer.append(request_number(1)), JournalStatus::Ok);
        ASSERT_EQ(writer.append_restart(55), JournalStatus::Ok);
        EXPECT_EQ(writer.next_sequence(), 3U);
        ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    }
    const std::vector<JournalRecord> list = records_of(path_);
    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list[1].type, RecordType::Restart);
    EXPECT_EQ(list[1].sequence, 2U);
    EXPECT_EQ(list[1].owner, kNoOwner);
    EXPECT_EQ(list[1].now, 55U);
}

TEST_F(JournalWriterTest, CreateNeverReplacesAnExistingFile) {
    {
        std::ofstream(path_) << "an old journal";
    }
    EXPECT_THROW(JournalWriter::create(path_, config_with(SyncPolicy::Write)), std::system_error);
    std::ifstream in(path_);
    std::string contents;
    std::getline(in, contents);
    EXPECT_EQ(contents, "an old journal");
}

TEST_F(JournalWriterTest, CreateRefusesABadConfiguration) {
    EXPECT_THROW(JournalWriter::create(path_, config_with(SyncPolicy::Write, kMaxRecordLength - 1)),
                 std::invalid_argument);
    EXPECT_THROW(JournalWriter::create(path_, config_with(SyncPolicy::Write), 0),
                 std::invalid_argument);
    JournalWriterConfig no_ops = config_with(SyncPolicy::Write);
    no_ops.ops.write = nullptr;
    EXPECT_THROW(JournalWriter::create(path_, no_ops), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path_));
    EXPECT_THROW(
        JournalWriter::create(dir_.file("no/such/dir.journal"), config_with(SyncPolicy::Write)),
        std::system_error);
}

TEST_F(JournalWriterTest, CreateUnderSyncSyncsTheHeader) {
    const JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Sync));
    EXPECT_EQ(fakes.syncs, 1);
    EXPECT_EQ(writer.stats().syncs, 1U);
}

TEST_F(JournalWriterTest, RecordsGetConsecutiveNumbersAndExactlyTheirRequests) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write), 1);
    protocol::OrderEntryConfig config;
    config.max_owners = 3;
    ASSERT_EQ(writer.append_session_start(1, config), JournalStatus::Ok);
    ASSERT_EQ(writer.append_add_symbol(2, sample_listing()), JournalStatus::Ok);
    ASSERT_EQ(writer.append_risk_limits(3, 2, {.max_order_qty = 9, .max_open_orders = 8}),
              JournalStatus::Ok);
    for (int i = 0; i < 200; ++i) {
        ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
        if (i % 10 == 9) {
            ASSERT_EQ(writer.commit(), JournalStatus::Ok);
        }
    }
    ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    EXPECT_EQ(writer.next_sequence(), 204U);
    EXPECT_EQ(writer.stats().records, 203U);

    const std::vector<JournalRecord> records = records_of(path_);
    ASSERT_EQ(records.size(), 203U);
    for (std::size_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(records[i].sequence, i + 1);
    }
    EXPECT_EQ(testing::unwrap(session_config_of(records[0])).max_owners, 3U);
    EXPECT_EQ(testing::unwrap(symbol_listing_of(records[1])).ticker, sample_listing().ticker);
    EXPECT_EQ(testing::unwrap(risk_limits_of(records[2])).max_order_qty, 9U);
    for (int i = 0; i < 200; ++i) {
        const pipeline::Request expected = request_number(i);
        const pipeline::Request got =
            testing::unwrap(request_of(records[static_cast<std::size_t>(i) + 3]));
        EXPECT_EQ(got.kind, expected.kind);
        EXPECT_EQ(got.owner, expected.owner);
        EXPECT_EQ(got.now, expected.now);
        EXPECT_TRUE(std::ranges::equal(got.bytes(), expected.bytes()));
    }
}

TEST_F(JournalWriterTest, AppendOnlyTouchesMemory) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
    const std::uint64_t writes = writer.stats().writes;
    ASSERT_EQ(writer.append(request_number(1)), JournalStatus::Ok);
    ASSERT_EQ(writer.append(request_number(2)), JournalStatus::Ok);
    EXPECT_EQ(file_size(path_), kFileHeaderLength);
    EXPECT_EQ(writer.stats().writes, writes);
    EXPECT_GT(writer.pending_bytes(), 0U);
    // One commit writes both records with one write() call. That is group commit.
    ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    EXPECT_EQ(writer.stats().writes, writes + 1);
    EXPECT_EQ(writer.pending_bytes(), 0U);
    EXPECT_EQ(records_of(path_).size(), 2U);
}

TEST_F(JournalWriterTest, AFullBufferRefusesTheRecordUntilACommit) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write, 200));
    int appended = 0;
    while (writer.append(request_number(40)) == JournalStatus::Ok) {
        ++appended;
    }
    EXPECT_GT(appended, 0);
    const Sequence next = writer.next_sequence();
    const std::size_t pending = writer.pending_bytes();
    EXPECT_EQ(writer.append(request_number(40)), JournalStatus::BufferFull);
    EXPECT_EQ(writer.next_sequence(), next);
    EXPECT_EQ(writer.pending_bytes(), pending);
    ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    EXPECT_EQ(writer.append(request_number(40)), JournalStatus::Ok);
    ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    const std::vector<JournalRecord> records = records_of(path_);
    ASSERT_EQ(records.size(), static_cast<std::size_t>(appended) + 1);
    EXPECT_EQ(records.back().sequence, next);
}

TEST_F(JournalWriterTest, ARecordTheFormatCannotHoldIsRefused) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
    EXPECT_EQ(writer.append(pipeline::make_frame_request(1, {}, 0)), JournalStatus::Refused);
    EXPECT_EQ(writer.append_risk_limits(0, kNoOwner, {}), JournalStatus::Refused);
    protocol::OrderEntryConfig bad;
    bad.max_owners = 0;
    EXPECT_EQ(writer.append_session_start(0, bad), JournalStatus::Refused);
    EXPECT_EQ(writer.next_sequence(), 1U);
    EXPECT_EQ(writer.pending_bytes(), 0U);
    EXPECT_FALSE(writer.stopped());
}

TEST_F(JournalWriterTest, BufferedWaitsUntilHalfTheBufferIsUsed) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Buffered, 1000));
    ASSERT_EQ(writer.append(request_number(1)), JournalStatus::Ok);
    ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    EXPECT_EQ(file_size(path_), kFileHeaderLength);
    while (writer.pending_bytes() < 500) {
        ASSERT_EQ(writer.append(request_number(40)), JournalStatus::Ok);
    }
    ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    EXPECT_GT(file_size(path_), kFileHeaderLength);
    EXPECT_EQ(writer.pending_bytes(), 0U);
    // flush() writes whatever is there, half full or not.
    ASSERT_EQ(writer.append(request_number(2)), JournalStatus::Ok);
    ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    EXPECT_EQ(writer.pending_bytes(), 0U);
    EXPECT_EQ(fakes.syncs, 0);
}

TEST_F(JournalWriterTest, WriteCommitsEveryTimeAndNeverSyncs) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
        ASSERT_EQ(writer.commit(), JournalStatus::Ok);
        EXPECT_EQ(records_of(path_).size(), static_cast<std::size_t>(i) + 1);
    }
    EXPECT_EQ(fakes.syncs, 0);
    EXPECT_EQ(writer.stats().syncs, 0U);
}

TEST_F(JournalWriterTest, SyncSyncsEveryCommitThatHadRecords) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Sync));
    const int after_create = fakes.syncs;
    ASSERT_EQ(writer.append(request_number(1)), JournalStatus::Ok);
    ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    EXPECT_EQ(fakes.syncs, after_create + 1);
    // A commit with nothing to write makes no system call at all.
    const std::uint64_t writes = writer.stats().writes;
    ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    EXPECT_EQ(fakes.syncs, after_create + 1);
    EXPECT_EQ(writer.stats().writes, writes);
}

TEST_F(JournalWriterTest, PartialWritesAreContinuedUntilEverythingIsWritten) {
    {
        JournalWriter normal =
            JournalWriter::create(dir_.file("normal.journal"), config_with(SyncPolicy::Write));
        for (int i = 0; i < 30; ++i) {
            ASSERT_EQ(normal.append(request_number(i)), JournalStatus::Ok);
        }
        ASSERT_EQ(normal.flush(), JournalStatus::Ok);
    }
    fakes.chunk = 3;
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
    for (int i = 0; i < 30; ++i) {
        ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
    }
    ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    EXPECT_GT(writer.stats().partial_writes, 100U);
    EXPECT_EQ(read_file(path_), read_file(dir_.file("normal.journal")));
}

TEST_F(JournalWriterTest, InterruptedWritesAreRetried) {
    fakes.interrupt_every_other = true;
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
        ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    }
    EXPECT_GE(writer.stats().interrupted, 10U);
    EXPECT_EQ(records_of(path_).size(), 10U);
}

TEST_F(JournalWriterTest, AWriteErrorStopsTheWriterForGood) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
    // The disk fills up 50 bytes into the next write, in the middle of a record.
    fakes.fail_after = fakes.written + 50;
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
    }
    EXPECT_EQ(writer.commit(), JournalStatus::WriteFailed);
    EXPECT_EQ(writer.stats().last_error, ENOSPC);
    EXPECT_TRUE(writer.stopped());
    fakes.fail_after = 0;  // Even once there is room again, the writer stays stopped.
    EXPECT_EQ(writer.append(request_number(9)), JournalStatus::Stopped);
    EXPECT_EQ(writer.commit(), JournalStatus::Stopped);
    EXPECT_EQ(writer.flush(), JournalStatus::Stopped);
    // What did reach the file is a good prefix with a torn tail, which recovery handles.
    const JournalScan s = scan_journal(read_file(path_));
    // The first record is 29 bytes and fits in the 50, the second is cut.
    EXPECT_EQ(s.tail, TailStatus::Torn);
    EXPECT_EQ(s.records, 1U);
    EXPECT_EQ(s.valid_length, kFileHeaderLength + kMinRecordLength + 1);
    EXPECT_EQ(s.file_length, kFileHeaderLength + 50);
}

TEST_F(JournalWriterTest, AFailedSyncStopsTheWriterEvenIfTheNextSyncWouldWork) {
    // After a failed fsync the kernel may have dropped the unwritten pages and cleared
    // the error, so trying again could report success for data that is gone.
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Sync));
    fakes.fail_syncs = 1;
    ASSERT_EQ(writer.append(request_number(1)), JournalStatus::Ok);
    EXPECT_EQ(writer.commit(), JournalStatus::SyncFailed);
    EXPECT_EQ(writer.stats().last_error, EIO);
    EXPECT_TRUE(writer.stopped());
    EXPECT_EQ(writer.append(request_number(2)), JournalStatus::Stopped);
    EXPECT_EQ(writer.commit(), JournalStatus::Stopped);
}

TEST_F(JournalWriterTest, AWriteThatTakesNothingIsAFailure) {
    JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
    fakes.return_zero = true;
    ASSERT_EQ(writer.append(request_number(1)), JournalStatus::Ok);
    EXPECT_EQ(writer.commit(), JournalStatus::WriteFailed);
    EXPECT_EQ(writer.stats().last_error, EIO);
}

TEST_F(JournalWriterTest, TheDestructorWritesWhatIsStillBuffered) {
    {
        JournalWriter writer =
            JournalWriter::create(path_, config_with(SyncPolicy::Buffered, 100'000));
        for (int i = 0; i < 7; ++i) {
            ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
            ASSERT_EQ(writer.commit(), JournalStatus::Ok);
        }
        EXPECT_EQ(file_size(path_), kFileHeaderLength);
    }
    EXPECT_EQ(records_of(path_).size(), 7U);
}

TEST_F(JournalWriterTest, AMovedWriterKeepsGoing) {
    JournalWriter first = JournalWriter::create(path_, config_with(SyncPolicy::Write));
    ASSERT_EQ(first.append(request_number(1)), JournalStatus::Ok);
    JournalWriter second(std::move(first));
    ASSERT_EQ(second.append(request_number(2)), JournalStatus::Ok);
    JournalWriter third =
        JournalWriter::create(dir_.file("other.journal"), config_with(SyncPolicy::Write));
    third = std::move(second);
    ASSERT_EQ(third.flush(), JournalStatus::Ok);
    EXPECT_EQ(third.next_sequence(), 3U);
    EXPECT_EQ(records_of(path_).size(), 2U);
}

TEST_F(JournalWriterTest, ResumeCutsATornTailAndContinuesTheNumbers) {
    {
        JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
        for (int i = 0; i < 10; ++i) {
            ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
        }
        ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    }
    // Cut 5 bytes off the last record, as a crash during its write would.
    std::filesystem::resize_file(path_, file_size(path_) - 5);
    JournalScan scan = scan_journal(read_file(path_));
    ASSERT_EQ(scan.tail, TailStatus::Torn);
    ASSERT_EQ(scan.records, 9U);
    {
        JournalWriter writer = JournalWriter::resume(path_, scan, config_with(SyncPolicy::Write));
        EXPECT_EQ(file_size(path_), scan.valid_length);
        EXPECT_EQ(writer.next_sequence(), 10U);
        for (int i = 20; i < 23; ++i) {
            ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
        }
        ASSERT_EQ(writer.flush(), JournalStatus::Ok);
    }
    scan = scan_journal(read_file(path_));
    EXPECT_EQ(scan.tail, TailStatus::Clean);
    EXPECT_EQ(scan.records, 12U);
    const std::vector<JournalRecord> records = records_of(path_);
    EXPECT_EQ(records[9].now, request_number(20).now);
}

TEST_F(JournalWriterTest, ResumeOfACleanJournalAppends) {
    {
        JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write), 50);
        ASSERT_EQ(writer.append(request_number(1)), JournalStatus::Ok);
    }
    const JournalScan scan = scan_journal(read_file(path_));
    {
        JournalWriter writer = JournalWriter::resume(path_, scan, config_with(SyncPolicy::Sync));
        EXPECT_EQ(fakes.syncs, 0);  // Nothing was cut, so nothing needed a sync yet.
        ASSERT_EQ(writer.append(request_number(2)), JournalStatus::Ok);
        ASSERT_EQ(writer.commit(), JournalStatus::Ok);
    }
    const std::vector<JournalRecord> records = records_of(path_);
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[1].sequence, 51U);
}

TEST_F(JournalWriterTest, ResumeRefusesWhatCannotBeContinued) {
    {
        JournalWriter writer = JournalWriter::create(path_, config_with(SyncPolicy::Write));
        for (int i = 0; i < 4; ++i) {
            ASSERT_EQ(writer.append(request_number(i)), JournalStatus::Ok);
        }
    }
    const JournalScan scan = scan_journal(read_file(path_));
    JournalScan corrupt = scan;
    corrupt.tail = TailStatus::Corrupt;
    EXPECT_THROW(JournalWriter::resume(path_, corrupt, config_with(SyncPolicy::Write)),
                 std::invalid_argument);
    JournalScan unreadable = scan;
    unreadable.tail = TailStatus::BadHeader;
    EXPECT_THROW(JournalWriter::resume(path_, unreadable, config_with(SyncPolicy::Write)),
                 std::invalid_argument);
    // The file grew after the scan, so the scan no longer describes it.
    {
        std::ofstream(path_, std::ios::app) << "x";
    }
    EXPECT_THROW(JournalWriter::resume(path_, scan, config_with(SyncPolicy::Write)),
                 std::logic_error);
    EXPECT_THROW(JournalWriter::resume(dir_.file("missing"), scan, config_with(SyncPolicy::Write)),
                 std::system_error);
}

}  // namespace
}  // namespace litbook::journal
