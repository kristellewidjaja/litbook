#pragma once

#include "litbook/journal/journal_format.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>

namespace litbook::journal {

// How a journal ends.
enum class TailStatus : std::uint8_t {
    Clean,      // Every byte after the header belongs to a good record.
    Torn,       // The journal ends with bytes that are not a good record, and no good
                // record follows them. A crash in the middle of a write leaves this, and
                // recovery cuts those bytes off.
    Corrupt,    // A good record follows the bad bytes, or a record has the wrong sequence
                // number. Something in the middle was lost or damaged, so recovery must
                // refuse to start instead of silently dropping records.
    BadHeader,  // The file header is missing or wrong, so nothing can be read.
};

std::ostream& operator<<(std::ostream& os, TailStatus status);

// What scan_journal found.
struct JournalScan {
    HeaderStatus header = HeaderStatus::Incomplete;
    TailStatus tail = TailStatus::BadHeader;
    Sequence first_sequence = 0;   // From the header.
    Sequence next_sequence = 0;    // The number the next appended record must have.
    std::uint64_t records = 0;     // Good records, all before valid_length.
    std::size_t valid_length = 0;  // Header plus good records, in bytes.
    std::size_t file_length = 0;
    // Why reading stopped at valid_length. Ok means it reached the end of the file, or
    // that the record there decoded but had the wrong sequence number.
    RecordStatus stop_reason = RecordStatus::Ok;
    bool sequence_break = false;  // The record at valid_length had the wrong number.
    // For Corrupt, the offset of the first good record found after the bad bytes, or of
    // the record with the wrong sequence number.
    std::size_t corrupt_at = 0;
};

// Checks a whole journal held in memory. It reads the header, then records one after
// another, each of which must decode and carry the next sequence number, and stops at the
// first one that does not. If it stopped early, it then looks for a good record at every
// later byte offset to tell a torn tail from corruption in the middle. That search only
// runs when something is wrong, and after a crash the bad tail is at most one write.
[[nodiscard]] JournalScan scan_journal(std::span<const std::byte> bytes) noexcept;

// Walks the records of a journal one by one. Give it the bytes after the file header up
// to the scan's valid_length (or any bytes; it stops at the first record that does not
// decode, and status() says why).
//
// Invariants. offset() is the start of the next record to read and never passes the end.
// After next() returns false it keeps returning false.
class RecordReader {
public:
    explicit RecordReader(std::span<const std::byte> records) noexcept : bytes_(records) {}

    // Decodes the next record into `record`. Returns false at the end or at a record that
    // does not decode.
    bool next(JournalRecord& record) noexcept;

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    // Ok after a clean end, otherwise why the last record did not decode.
    [[nodiscard]] RecordStatus status() const noexcept { return status_; }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
    RecordStatus status_ = RecordStatus::Ok;
    bool done_ = false;
};

// The bytes of a whole journal file, from a read-only memory map. The pages are read from
// disk when they are first touched, so a large journal is not copied into memory first.
// Movable, not copyable. The mapping is removed in the destructor.
//
// Invariants. bytes() is the file's contents as they were when it was mapped (the
// journal is only ever appended to, and nothing appends while it is being read). An empty
// file gives an empty span and no mapping.
class MappedFile {
public:
    // Throws std::system_error if the file cannot be opened, examined or mapped.
    explicit MappedFile(const std::string& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

private:
    void unmap() noexcept;

    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace litbook::journal
