#include "litbook/journal/snapshot_store.hpp"

#include "litbook/journal/journal_reader.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>

namespace litbook::journal {

namespace {

// Writes all of `bytes`, continuing after partial writes and signals.
int write_all(int fd, std::span<const std::byte> bytes) noexcept {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        if (n == 0) {
            return EIO;
        }
        done += static_cast<std::size_t>(n);
    }
    return 0;
}

int sync_directory(const char* directory) noexcept {
    const int fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return errno;
    }
    const int rc = ::fsync(fd);
    const int error = errno;
    ::close(fd);
    return rc == 0 ? 0 : error;
}

}  // namespace

int write_snapshot_file(PathBuffer& paths, Sequence as_of, std::span<const std::byte> bytes,
                        bool sync) noexcept {
    // PathBuffer reuses one buffer, so the temporary name is copied before the final one
    // is built.
    std::array<char, PATH_MAX> temporary{};
    std::snprintf(temporary.data(), temporary.size(), "%s", paths.temporary_snapshot(as_of));

    const int fd = ::open(temporary.data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return errno;
    }
    int error = write_all(fd, bytes);
    if (error == 0 && sync && ::fdatasync(fd) != 0) {
        error = errno;
    }
    if (::close(fd) != 0 && error == 0) {
        error = errno;
    }
    if (error == 0 && ::rename(temporary.data(), paths.snapshot(as_of)) != 0) {
        error = errno;
    }
    if (error != 0) {
        ::unlink(temporary.data());
        return error;
    }
    return sync ? sync_directory(paths.directory()) : 0;
}

LoadedSnapshot read_snapshot_file(const SnapshotFile& file) {
    const MappedFile mapped(file.path);
    LoadedSnapshot loaded;
    loaded.status = decode_snapshot(mapped.bytes(), loaded.snapshot);
    if (loaded.status == SnapshotStatus::Ok && loaded.snapshot.as_of != file.as_of) {
        loaded.status = SnapshotStatus::BadHeader;
    }
    return loaded;
}

std::vector<std::string> files_to_remove(const DirectoryListing& listing,
                                         const std::vector<SnapshotFile>& good, std::size_t keep) {
    std::vector<std::string> remove;
    if (keep == 0 || good.empty()) {
        return remove;
    }
    const std::size_t first_kept = good.size() > keep ? good.size() - keep : 0;
    for (std::size_t i = 0; i < first_kept; ++i) {
        remove.push_back(good[i].path);
    }
    const Sequence oldest_kept = good[first_kept].as_of;
    // Segment i holds the records from its first up to the next segment's first minus 1.
    for (std::size_t i = 0; i + 1 < listing.segments.size(); ++i) {
        if (listing.segments[i + 1].first <= oldest_kept + 1) {
            remove.push_back(listing.segments[i].path);
        }
    }
    return remove;
}

bool snapshot_is_continued(const ChainScan& chain, Sequence as_of) noexcept {
    if (chain.status != ChainStatus::Ok || chain.segments.empty()) {
        return false;
    }
    const Sequence first = chain.segments.front().scan.first_sequence;
    const Sequence next_after_snapshot = as_of + 1;
    return first <= next_after_snapshot && next_after_snapshot <= chain.next_sequence;
}

}  // namespace litbook::journal
