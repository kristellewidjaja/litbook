#pragma once

#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/order_entry_config.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace litbook::journal {

// How hard commit() tries to make the records it writes survive a crash.
enum class SyncPolicy : std::uint8_t {
    // commit() only writes once the buffer is at least half full, so the records of a
    // small commit stay in this process's memory and a crash of the process loses them.
    // It shows what the journal costs without system calls, for benchmarks only.
    Buffered,
    // commit() hands every record to the kernel with write(). They survive a crash of the
    // process (kill -9 included), because the kernel's page cache still writes them out,
    // but not a crash of the whole machine or a power loss.
    Write,
    // commit() also calls fdatasync(), which returns only once the disk has the data. On
    // real hardware that survives a power loss. Inside a virtual machine it is only as
    // good as the hypervisor's promise (see docs/JOURNAL.md).
    Sync,
};

std::ostream& operator<<(std::ostream& os, SyncPolicy policy);

enum class JournalStatus : std::uint8_t {
    Ok,
    BufferFull,   // The record does not fit in what is left of the buffer. Commit first.
    Refused,      // The record cannot be stored (see the encode functions). Nothing changed.
    WriteFailed,  // write() failed. last_error has errno. The writer is now stopped.
    SyncFailed,   // fdatasync() failed. last_error has errno. The writer is now stopped.
    Stopped,      // An earlier write or sync failed, so the writer refuses everything.
};

std::ostream& operator<<(std::ostream& os, JournalStatus status);

// The system calls the writer makes. Tests swap them for fakes that write a few bytes at
// a time, get interrupted or fail. Plain function pointers, so nothing is allocated.
struct FileOps {
    ssize_t (*write)(int fd, const void* data, std::size_t length) = &::write;
    int (*sync)(int fd) = &::fdatasync;
};

struct JournalWriterConfig {
    // The bytes of records that can wait for one commit. At least kMaxRecordLength.
    std::size_t buffer_capacity = std::size_t{1} << 20U;
    SyncPolicy sync = SyncPolicy::Write;
    FileOps ops{};
};

struct JournalWriterStats {
    std::uint64_t records = 0;         // Records appended, the file's earlier ones not counted.
    std::uint64_t commits = 0;         // commit() and flush() calls that had bytes to write.
    std::uint64_t writes = 0;          // write() calls, including retries.
    std::uint64_t partial_writes = 0;  // write() calls that took only part of the bytes.
    std::uint64_t interrupted = 0;     // write() calls interrupted by a signal (EINTR).
    std::uint64_t syncs = 0;           // fdatasync() calls.
    std::uint64_t bytes_written = 0;   // Bytes the kernel took, the file header included.
    int last_error = 0;                // errno of the failure that stopped the writer.
};

// Appends records to a journal file. Appending only encodes the record into a buffer that
// was allocated at startup, and gives it the next sequence number. commit() then writes
// every record appended since the last commit with as few write() calls as the kernel
// allows, and syncs if the policy says so. One commit for a whole batch of records is
// called group commit. It is what makes a journal cheap when many records arrive at once.
//
// Partial writes and EINTR are retried. Any other failure stops the writer for good, and
// every later call returns Stopped. After a failed fdatasync this is the only safe
// choice, because the kernel may already have thrown the unwritten pages away and cleared
// the error, so a second fdatasync could report success for data that is gone. The
// exchange must then stop and recover from what is on disk.
//
// Invariants. The file holds a valid header followed by good records with consecutive
// sequence numbers, possibly ending in a torn record if a write failed or the process was
// killed during one. The buffer holds exactly the records appended since the last write,
// and next_sequence() is one more than the last of them. Only one thread uses a writer.
// Nothing allocates after construction.
class JournalWriter {
public:
    // Creates a new journal and writes its header (and syncs it under Sync, together with
    // the directory, so the new file's name survives too). It refuses a path that already
    // exists, so a journal is never overwritten by accident. Throws std::invalid_argument
    // for a buffer smaller than one record or a first sequence of 0, and
    // std::system_error if the file cannot be created or written.
    static JournalWriter create(const std::string& path, const JournalWriterConfig& config,
                                Sequence first_sequence = 1);

    // Opens an existing journal to keep appending to it, as recovery does. `scan` must be
    // the scan of this file as it is now. A torn tail is cut off first, so the next record
    // follows the last good one. Throws std::invalid_argument if the scan found a bad
    // header or corruption, std::logic_error if the file's length no longer matches the
    // scan, and std::system_error if the file cannot be opened, cut or synced.
    static JournalWriter resume(const std::string& path, const JournalScan& scan,
                                const JournalWriterConfig& config);

    // Writes out whatever is still buffered (even under Buffered) and closes the file.
    // A failure here cannot be reported, so call flush() first to see it.
    ~JournalWriter();

    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;
    JournalWriter(JournalWriter&& other) noexcept;
    JournalWriter& operator=(JournalWriter&& other) noexcept;

    // Setup records, written before the exchange takes any request.
    JournalStatus append_session_start(Timestamp now, const protocol::OrderEntryConfig& config);
    JournalStatus append_add_symbol(Timestamp now, const SymbolListing& listing);
    JournalStatus append_risk_limits(Timestamp now, OwnerId owner,
                                     const protocol::RiskLimits& limits);
    // A Frame, CancelAll or Stop request, exactly as the matching thread will see it.
    JournalStatus append(const pipeline::Request& request);

    // Writes the records appended since the last commit, following the sync policy.
    JournalStatus commit();
    // Writes every buffered record whatever the policy, and syncs under Sync.
    JournalStatus flush();

    [[nodiscard]] Sequence next_sequence() const noexcept { return next_sequence_; }
    [[nodiscard]] std::size_t pending_bytes() const noexcept { return pending_; }
    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    [[nodiscard]] SyncPolicy policy() const noexcept { return config_.sync; }
    [[nodiscard]] const JournalWriterStats& stats() const noexcept { return stats_; }

private:
    JournalWriter(int fd, const JournalWriterConfig& config, Sequence next_sequence);

    // Gives the encoded record its room in the buffer. `encode` writes it there.
    template <class Encode>
    JournalStatus append_with(Encode encode);
    JournalStatus write_pending(bool force);
    void close_file() noexcept;

    int fd_ = -1;
    JournalWriterConfig config_;
    std::vector<std::byte> buffer_;
    std::size_t pending_ = 0;  // Bytes at the front of buffer_ waiting to be written.
    Sequence next_sequence_ = 1;
    bool stopped_ = false;
    JournalWriterStats stats_;
};

}  // namespace litbook::journal
