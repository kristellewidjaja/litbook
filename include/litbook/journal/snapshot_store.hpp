#pragma once

#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/snapshot_format.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace litbook::journal {

// Writes the bytes of a finished snapshot into the journal directory so that a crash at
// any moment leaves either the old set of files or the new one, never half a snapshot
// under a real name. The bytes go to "<as_of>.snapshot.tmp", which is renamed to
// "<as_of>.snapshot" once it is complete. rename() replaces the name in one step. With
// `sync` the file is synced before the rename and the directory after it, so the new
// name survives a power loss too (fsync(2) says a file's directory entry needs its own
// sync). Returns 0, or the errno of the step that failed, in which case the temporary
// file is removed. Never allocates.
int write_snapshot_file(PathBuffer& paths, Sequence as_of, std::span<const std::byte> bytes,
                        bool sync) noexcept;

// A snapshot file read back and decoded. `snapshot` is only meaningful when status is Ok.
struct LoadedSnapshot {
    SnapshotStatus status = SnapshotStatus::TooShort;
    Snapshot snapshot;
};

// Reads and decodes one snapshot file. A name that disagrees with the as_of inside the
// file counts as BadHeader. Throws std::system_error if the file cannot be read.
[[nodiscard]] LoadedSnapshot read_snapshot_file(const SnapshotFile& file);

// Which files can go once `good` holds the snapshots known to be usable (sorted by
// as_of). The newest `keep` good snapshots stay, and every older good snapshot can be
// removed. A segment can be removed once every record in it is in the oldest snapshot
// that stays, which the next segment's first sequence number shows, so the last (open)
// segment always stays. With keep 0 or no good snapshot nothing is removed. Snapshots
// that are not in `good` (damaged ones) are never removed, so an operator can look at
// them.
// True if the segments of `chain` hold every record after a snapshot as of `as_of`, so
// recovery can start from that snapshot. The chain must be whole, its first segment must
// start at or before as_of + 1, and it must reach at least as far as as_of.
[[nodiscard]] bool snapshot_is_continued(const ChainScan& chain, Sequence as_of) noexcept;

[[nodiscard]] std::vector<std::string> files_to_remove(const DirectoryListing& listing,
                                                       const std::vector<SnapshotFile>& good,
                                                       std::size_t keep);

}  // namespace litbook::journal
