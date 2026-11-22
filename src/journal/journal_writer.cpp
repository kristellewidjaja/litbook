#include "litbook/journal/journal_writer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <ostream>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace litbook::journal {

namespace {

[[noreturn]] void throw_errno(int error, const std::string& what) {
    throw std::system_error(error, std::generic_category(), what);
}

// Makes a new file's name durable. A file created and synced can still vanish after a
// power loss if the directory entry that names it was never synced.
void sync_directory_of(const std::string& path) {
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    if (dir.empty()) {
        dir = ".";
    }
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        throw_errno(errno, "open directory " + dir.string());
    }
    const int rc = ::fsync(fd);
    const int error = errno;
    ::close(fd);
    if (rc != 0) {
        throw_errno(error, "fsync directory " + dir.string());
    }
}

void check_config(const JournalWriterConfig& config) {
    if (config.buffer_capacity < kMaxRecordLength) {
        throw std::invalid_argument("the journal buffer must hold at least one record");
    }
    if (config.ops.write == nullptr || config.ops.sync == nullptr) {
        throw std::invalid_argument("the journal file operations must be set");
    }
}

}  // namespace

JournalWriter::JournalWriter(int fd, const JournalWriterConfig& config, Sequence next_sequence)
    : fd_(fd), config_(config), buffer_(config.buffer_capacity), next_sequence_(next_sequence) {}

JournalWriter JournalWriter::create(const std::string& path, const JournalWriterConfig& config,
                                    Sequence first_sequence) {
    check_config(config);
    if (first_sequence == 0) {
        throw std::invalid_argument("journal sequence numbers start at 1 or above");
    }
    // O_EXCL makes open fail if the file exists, so an old journal is never replaced.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw_errno(errno, "create journal " + path);
    }
    JournalWriter writer(fd, config, first_sequence);
    writer.pending_ = encode_file_header(writer.buffer_, first_sequence);
    if (writer.flush() != JournalStatus::Ok) {
        throw_errno(writer.stats_.last_error, "write journal header " + path);
    }
    if (config.sync == SyncPolicy::Sync) {
        sync_directory_of(path);
    }
    return writer;
}

JournalWriter JournalWriter::resume(const std::string& path, const JournalScan& scan,
                                    const JournalWriterConfig& config) {
    check_config(config);
    if (scan.tail != TailStatus::Clean && scan.tail != TailStatus::Torn) {
        throw std::invalid_argument("only a journal with a clean or torn end can be continued");
    }
    // O_APPEND puts every write at the current end of the file, so a write can never
    // land in the middle even if something moved the file offset.
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd < 0) {
        throw_errno(errno, "open journal " + path);
    }
    JournalWriter writer(fd, config, scan.next_sequence);
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        throw_errno(errno, "fstat journal " + path);
    }
    if (std::cmp_not_equal(info.st_size, scan.file_length)) {
        throw std::logic_error("the journal changed after it was scanned");
    }
    if (scan.valid_length < scan.file_length) {
        if (::ftruncate(fd, static_cast<off_t>(scan.valid_length)) != 0) {
            throw_errno(errno, "cut the torn tail of " + path);
        }
        if (config.sync == SyncPolicy::Sync && config.ops.sync(fd) != 0) {
            throw_errno(errno, "sync journal " + path);
        }
    }
    return writer;
}

JournalWriter::~JournalWriter() {
    if (fd_ >= 0 && !stopped_ && pending_ > 0) {
        // A destructor cannot report the result. Callers that care call flush() first.
        static_cast<void>(flush());
    }
    close_file();
}

JournalWriter::JournalWriter(JournalWriter&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      config_(other.config_),
      buffer_(std::move(other.buffer_)),
      pending_(std::exchange(other.pending_, 0)),
      next_sequence_(other.next_sequence_),
      stopped_(other.stopped_),
      stats_(other.stats_) {}

JournalWriter& JournalWriter::operator=(JournalWriter&& other) noexcept {
    if (this != &other) {
        close_file();
        fd_ = std::exchange(other.fd_, -1);
        config_ = other.config_;
        buffer_ = std::move(other.buffer_);
        pending_ = std::exchange(other.pending_, 0);
        next_sequence_ = other.next_sequence_;
        stopped_ = other.stopped_;
        stats_ = other.stats_;
    }
    return *this;
}

template <class Encode>
JournalStatus JournalWriter::append_with(Encode encode) {
    if (stopped_) {
        return JournalStatus::Stopped;
    }
    const std::span<std::byte> room = std::span<std::byte>(buffer_).subspan(pending_);
    // The encoders return 0 both when the room is too small and when the record cannot
    // be stored at all. Encoding into a scratch record of the largest size tells the two
    // apart without touching the buffer.
    const std::size_t n = encode(room);
    if (n == 0) {
        std::array<std::byte, kMaxRecordLength> scratch{};
        return encode(std::span<std::byte>(scratch)) == 0 ? JournalStatus::Refused
                                                          : JournalStatus::BufferFull;
    }
    pending_ += n;
    ++next_sequence_;
    ++stats_.records;
    return JournalStatus::Ok;
}

JournalStatus JournalWriter::append_session_start(Timestamp now,
                                                  const protocol::OrderEntryConfig& config) {
    const Sequence sequence = next_sequence_;
    return append_with(
        [&](std::span<std::byte> out) { return encode_session_start(out, sequence, now, config); });
}

JournalStatus JournalWriter::append_add_symbol(Timestamp now, const SymbolListing& listing) {
    const Sequence sequence = next_sequence_;
    return append_with(
        [&](std::span<std::byte> out) { return encode_add_symbol(out, sequence, now, listing); });
}

JournalStatus JournalWriter::append_risk_limits(Timestamp now, OwnerId owner,
                                                const protocol::RiskLimits& limits) {
    const Sequence sequence = next_sequence_;
    return append_with([&](std::span<std::byte> out) {
        return encode_risk_limits(out, sequence, now, owner, limits);
    });
}

JournalStatus JournalWriter::append(const pipeline::Request& request) {
    const Sequence sequence = next_sequence_;
    return append_with(
        [&](std::span<std::byte> out) { return encode_request(out, sequence, request); });
}

JournalStatus JournalWriter::append_restart(Timestamp now) {
    const Sequence sequence = next_sequence_;
    return append_with(
        [&](std::span<std::byte> out) { return encode_restart(out, sequence, now); });
}

JournalStatus JournalWriter::commit() {
    return write_pending(false);
}

JournalStatus JournalWriter::flush() {
    return write_pending(true);
}

JournalStatus JournalWriter::write_pending(bool force) {
    if (stopped_) {
        return JournalStatus::Stopped;
    }
    if (pending_ == 0) {
        return JournalStatus::Ok;
    }
    // Under Buffered a commit waits until half the buffer is used, so that small commits
    // cost no system call at all.
    if (!force && config_.sync == SyncPolicy::Buffered && pending_ < buffer_.size() / 2) {
        return JournalStatus::Ok;
    }
    ++stats_.commits;
    std::size_t done = 0;
    while (done < pending_) {
        ++stats_.writes;
        const ssize_t n = config_.ops.write(fd_, buffer_.data() + done, pending_ - done);
        if (n < 0) {
            if (errno == EINTR) {
                ++stats_.interrupted;
                continue;
            }
            stats_.last_error = errno;
            stopped_ = true;
            return JournalStatus::WriteFailed;
        }
        if (n == 0) {
            // A regular file never takes 0 of a non-empty write unless something is
            // badly wrong, and retrying would loop forever.
            stats_.last_error = EIO;
            stopped_ = true;
            return JournalStatus::WriteFailed;
        }
        const auto taken = static_cast<std::size_t>(n);
        if (taken < pending_ - done) {
            ++stats_.partial_writes;
        }
        done += taken;
        stats_.bytes_written += taken;
    }
    pending_ = 0;
    if (config_.sync == SyncPolicy::Sync) {
        ++stats_.syncs;
        if (config_.ops.sync(fd_) != 0) {
            stats_.last_error = errno;
            stopped_ = true;
            return JournalStatus::SyncFailed;
        }
    }
    return JournalStatus::Ok;
}

void JournalWriter::close_file() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::ostream& operator<<(std::ostream& os, SyncPolicy policy) {
    switch (policy) {
        case SyncPolicy::Buffered:
            return os << "buffered";
        case SyncPolicy::Write:
            return os << "write";
        case SyncPolicy::Sync:
            return os << "sync";
    }
    return os << "SyncPolicy(" << static_cast<int>(policy) << ")";
}

std::ostream& operator<<(std::ostream& os, JournalStatus status) {
    switch (status) {
        case JournalStatus::Ok:
            return os << "Ok";
        case JournalStatus::BufferFull:
            return os << "BufferFull";
        case JournalStatus::Refused:
            return os << "Refused";
        case JournalStatus::WriteFailed:
            return os << "WriteFailed";
        case JournalStatus::SyncFailed:
            return os << "SyncFailed";
        case JournalStatus::Stopped:
            return os << "Stopped";
    }
    return os << "JournalStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook::journal
