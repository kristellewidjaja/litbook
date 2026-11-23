#pragma once

#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace litbook::journal {

// A journal lives in a directory as a chain of segment files plus snapshot files.
//
//   00000000000000000001.journal    records 1 to 1,000,000
//   00000000000001000001.journal    records 1,000,001 onwards (the open segment)
//   00000000000001000000.snapshot   the state as of record 1,000,000
//
// A segment is named by the sequence number of its first record, which its file header
// also holds, and a snapshot by the sequence number of the last record it includes. The
// numbers have 20 digits, enough for any 64-bit number, so the names sort in sequence
// order. A snapshot is written under a name ending in ".snapshot.tmp" and renamed when it
// is complete. Any other file in the directory is left alone.

inline constexpr std::size_t kSequenceDigits = 20;
inline constexpr const char* kSegmentSuffix = ".journal";
inline constexpr const char* kSnapshotSuffix = ".snapshot";
inline constexpr const char* kTemporarySuffix = ".snapshot.tmp";

[[nodiscard]] std::string segment_file_name(Sequence first);
[[nodiscard]] std::string snapshot_file_name(Sequence as_of);

// Builds the full path of a segment or snapshot in one directory without allocating, so
// the journal thread and the snapshot thread can name new files after startup. Each call
// overwrites the same buffer, so a path must be used before the next call.
//
// Invariants. directory() is the directory given to the constructor, without a trailing
// slash, and every path returned starts with it.
class PathBuffer {
public:
    // Throws std::length_error if the directory name is too long for a path.
    explicit PathBuffer(const std::string& directory);

    [[nodiscard]] const char* directory() const noexcept { return directory_.data(); }
    [[nodiscard]] const char* segment(Sequence first) noexcept;
    [[nodiscard]] const char* snapshot(Sequence as_of) noexcept;
    [[nodiscard]] const char* temporary_snapshot(Sequence as_of) noexcept;

private:
    const char* build(Sequence number, const char* suffix) noexcept;

    std::array<char, PATH_MAX> directory_{};
    std::array<char, PATH_MAX> path_{};
    std::size_t directory_length_ = 0;
};

struct SegmentFile {
    Sequence first = 0;  // From the file name.
    std::string path;
};

struct SnapshotFile {
    Sequence as_of = 0;  // From the file name.
    std::string path;
};

// The litbook files of a journal directory, each list sorted by its number.
struct DirectoryListing {
    std::vector<SegmentFile> segments;
    std::vector<SnapshotFile> snapshots;
    std::vector<std::string> temporary;  // Snapshots a crash left half written.
};

// Lists a journal directory. Throws std::system_error if it cannot be read.
[[nodiscard]] DirectoryListing list_journal_directory(const std::string& directory);

// How a chain of segments fits together.
enum class ChainStatus : std::uint8_t {
    Ok,
    Empty,         // No segment at all.
    BadSegment,    // A segment has a bad header or is corrupt inside.
    NameMismatch,  // A segment's name does not match the first sequence in its header.
    Gap,           // A segment does not start where the one before it ended.
    TornInside,    // A segment that is not the last one ends with a torn record.
};

std::ostream& operator<<(std::ostream& os, ChainStatus status);

struct SegmentScan {
    SegmentFile file;
    JournalScan scan;
};

// What scan_segment_chain found.
struct ChainScan {
    ChainStatus status = ChainStatus::Empty;
    std::vector<SegmentScan> segments;  // Every usable segment, in order.
    // The segment where the problem is and its scan, for a status other than Ok and Empty.
    std::size_t problem_at = 0;
    SegmentScan problem;
    // A last segment whose header was cut short, because a crash came while a new segment
    // was being created. It holds no record, so recovery deletes it. Empty if none.
    std::string torn_rotation;
    Sequence next_sequence = 0;  // The number the next record must have.
    std::uint64_t records = 0;   // Good records in every usable segment together.
};

// Maps and scans every segment of a listing and checks that they form one chain. The
// first segment may start anywhere (older ones may have been deleted after a snapshot),
// each later one must start at the sequence number where the one before it ended, and
// only the last one may end torn. A last segment that holds only the start of the file
// header the chain expects next is a torn rotation, and is left out of the chain.
// Throws std::system_error if a file cannot be read.
[[nodiscard]] ChainScan scan_segment_chain(const DirectoryListing& listing);

}  // namespace litbook::journal
