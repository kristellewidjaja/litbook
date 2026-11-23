#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/journal/snapshot_format.hpp"
#include "litbook/journal/snapshot_store.hpp"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace litbook::journal {

// Replaying a whole journal directory offline, the way recovery does it at start but
// without an exchange. Tools and tests use it to check a directory, and it does not
// compare the setup records with anything, since there is no command line to compare
// them with. Nothing in the directory is changed.

// What one snapshot file was found to be.
struct SnapshotCheck {
    SnapshotFile file;
    SnapshotStatus status = SnapshotStatus::Ok;  // What decoding the file said.
    // True if the segments hold every record after the snapshot (snapshot_is_continued).
    bool continued = false;

    [[nodiscard]] bool usable() const noexcept { return status == SnapshotStatus::Ok && continued; }
};

// Every snapshot of a directory checked against its chain of segments.
struct SnapshotChoice {
    std::vector<SnapshotCheck> checks;  // One per snapshot file, oldest first.
    // The newest usable snapshot, decoded. Empty if no snapshot is usable.
    std::optional<Snapshot> newest;
};

// Reads and decodes every snapshot of `listing`. Throws std::system_error if a file
// cannot be read.
[[nodiscard]] SnapshotChoice check_snapshots(const DirectoryListing& listing,
                                             const ChainScan& chain);

// Where replay_directory starts.
enum class StartFrom : std::uint8_t {
    NewestSnapshot,  // The newest usable snapshot, or record 1 if there is none.
    FirstRecord,     // Always record 1, which needs the first segment.
};

enum class DirectoryStatus : std::uint8_t {
    Ok,
    Empty,         // The directory holds no segment.
    BrokenChain,   // The segments do not form one chain (chain.status says why).
    MissingStart,  // The first segment starts after record 1, and no snapshot can be used.
    ReplayFailed,  // The replayer refused a record or the snapshot (replay says why).
};

std::ostream& operator<<(std::ostream& os, DirectoryStatus status);

// What replay_directory found and did.
struct DirectoryReplay {
    DirectoryStatus status = DirectoryStatus::Empty;
    ReplayStatus replay = ReplayStatus::Ok;
    ChainScan chain;
    std::vector<SnapshotCheck> snapshots;  // Every snapshot file, oldest first.
    // The snapshot the replay started from (its as_of), or 0 if it started at record 1.
    Sequence snapshot_as_of = 0;
    // That snapshot's reply count for each account (entry 0 unused). Empty without one.
    // A replay from a snapshot only produces the replies after it, so these are needed to
    // know each account's next sequence number.
    std::vector<std::uint64_t> snapshot_replies;
    // The setup records the replay started with, from the snapshot or from the journal.
    std::vector<JournalRecord> setup;
};

// Lists `directory`, checks its chain and its snapshots, starts `replayer` from the newest
// usable snapshot (or from record 1), and applies every later good record of every
// segment. A torn tail in the last segment is left out, as recovery would cut it. The
// replayer must be fresh. Throws std::system_error if a file cannot be read, and
// std::runtime_error if a segment got shorter after it was scanned.
template <OrderBook Book>
DirectoryReplay replay_directory(const std::string& directory, Replayer<Book>& replayer,
                                 StartFrom start) {
    DirectoryReplay out;
    const DirectoryListing listing = list_journal_directory(directory);
    out.chain = scan_segment_chain(listing);
    SnapshotChoice choice = check_snapshots(listing, out.chain);
    out.snapshots = choice.checks;
    if (out.chain.status == ChainStatus::Empty) {
        out.status = DirectoryStatus::Empty;
        return out;
    }
    if (out.chain.status != ChainStatus::Ok) {
        out.status = DirectoryStatus::BrokenChain;
        return out;
    }

    const bool from_snapshot = start == StartFrom::NewestSnapshot && choice.newest.has_value();
    if (from_snapshot) {
        const Snapshot& snapshot = choice.newest.value();
        out.snapshot_as_of = snapshot.as_of;
        out.snapshot_replies = snapshot.replies;
        out.setup = snapshot.setup;
        out.replay = replayer.start_from(snapshot);
        if (out.replay != ReplayStatus::Ok) {
            out.status = DirectoryStatus::ReplayFailed;
            return out;
        }
    } else if (out.chain.segments.front().scan.first_sequence != 1) {
        out.status = DirectoryStatus::MissingStart;
        return out;
    }

    // The replayer skips the records a snapshot already holds, so every segment can be
    // given whole.
    for (const SegmentScan& segment : out.chain.segments) {
        const MappedFile file(segment.file.path);
        const std::size_t valid_length = segment.scan.valid_length;
        if (file.bytes().size() < valid_length) {
            throw std::runtime_error(segment.file.path + " got shorter while it was read");
        }
        RecordReader reader(
            file.bytes().subspan(kFileHeaderLength, valid_length - kFileHeaderLength));
        JournalRecord record;
        while (reader.next(record)) {
            if (!from_snapshot && is_setup_record(record.type)) {
                out.setup.push_back(record);
            }
            out.replay = replayer.apply(record);
            if (out.replay != ReplayStatus::Ok) {
                out.status = DirectoryStatus::ReplayFailed;
                return out;
            }
        }
    }
    out.status = DirectoryStatus::Ok;
    return out;
}

}  // namespace litbook::journal
