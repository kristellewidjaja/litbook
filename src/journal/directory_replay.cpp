#include "litbook/journal/directory_replay.hpp"

#include <ostream>
#include <utility>

namespace litbook::journal {

SnapshotChoice check_snapshots(const DirectoryListing& listing, const ChainScan& chain) {
    SnapshotChoice choice;
    for (const SnapshotFile& file : listing.snapshots) {
        LoadedSnapshot loaded = read_snapshot_file(file);
        SnapshotCheck check;
        check.file = file;
        check.status = loaded.status;
        check.continued = snapshot_is_continued(chain, file.as_of);
        // The listing is sorted oldest first, so a later usable snapshot replaces an
        // earlier one.
        if (check.usable()) {
            choice.newest = std::move(loaded.snapshot);
        }
        choice.checks.push_back(check);
    }
    return choice;
}

std::ostream& operator<<(std::ostream& os, DirectoryStatus status) {
    switch (status) {
        case DirectoryStatus::Ok:
            return os << "Ok";
        case DirectoryStatus::Empty:
            return os << "Empty";
        case DirectoryStatus::BrokenChain:
            return os << "BrokenChain";
        case DirectoryStatus::MissingStart:
            return os << "MissingStart";
        case DirectoryStatus::ReplayFailed:
            return os << "ReplayFailed";
    }
    return os << "DirectoryStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook::journal
