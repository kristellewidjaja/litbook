#include "litbook/journal/journal_reader.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <ostream>
#include <system_error>
#include <utility>

namespace litbook::journal {

JournalScan scan_journal(std::span<const std::byte> bytes) noexcept {
    JournalScan scan;
    scan.file_length = bytes.size();
    const DecodedHeader header = decode_file_header(bytes);
    scan.header = header.status;
    if (header.status != HeaderStatus::Ok) {
        scan.tail = TailStatus::BadHeader;
        return scan;
    }
    scan.first_sequence = header.first_sequence;
    scan.next_sequence = header.first_sequence;

    // Read good records one after another.
    std::size_t offset = kFileHeaderLength;
    while (offset < bytes.size()) {
        const DecodedRecord decoded = decode_record(bytes.subspan(offset));
        if (decoded.status != RecordStatus::Ok) {
            scan.stop_reason = decoded.status;
            break;
        }
        if (decoded.record.sequence != scan.next_sequence) {
            scan.sequence_break = true;
            break;
        }
        offset += decoded.length;
        ++scan.records;
        ++scan.next_sequence;
    }
    scan.valid_length = offset;

    if (offset == bytes.size()) {
        scan.tail = TailStatus::Clean;
        return scan;
    }
    // A good record with the wrong number is never a torn write, because the writer only
    // ever appends the next number.
    if (scan.sequence_break) {
        scan.tail = TailStatus::Corrupt;
        scan.corrupt_at = offset;
        return scan;
    }
    // Look for a good record anywhere after the bad bytes. Finding one means the bad
    // bytes are in the middle of the journal, not at its end.
    for (std::size_t p = offset + 1; p + kMinRecordLength <= bytes.size(); ++p) {
        if (decode_record(bytes.subspan(p)).status == RecordStatus::Ok) {
            scan.tail = TailStatus::Corrupt;
            scan.corrupt_at = p;
            return scan;
        }
    }
    scan.tail = TailStatus::Torn;
    return scan;
}

bool RecordReader::next(JournalRecord& record) noexcept {
    if (done_) {
        return false;
    }
    if (offset_ == bytes_.size()) {
        done_ = true;
        return false;
    }
    const DecodedRecord decoded = decode_record(bytes_.subspan(offset_));
    if (decoded.status != RecordStatus::Ok) {
        status_ = decoded.status;
        done_ = true;
        return false;
    }
    record = decoded.record;
    offset_ += decoded.length;
    return true;
}

MappedFile::MappedFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + path);
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "fstat " + path);
    }
    size_ = static_cast<std::size_t>(info.st_size);
    // mmap refuses a length of 0, and an empty file has nothing to map anyway.
    if (size_ > 0) {
        const void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            const int error = errno;
            ::close(fd);
            size_ = 0;
            throw std::system_error(error, std::generic_category(), "mmap " + path);
        }
        data_ = static_cast<const std::byte*>(p);
    }
    // The mapping keeps its own reference to the file, so the descriptor can go.
    ::close(fd);
}

MappedFile::~MappedFile() {
    unmap();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        unmap();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void MappedFile::unmap() noexcept {
    if (data_ != nullptr) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) munmap takes a non-const pointer.
        ::munmap(const_cast<std::byte*>(data_), size_);
        data_ = nullptr;
        size_ = 0;
    }
}

std::ostream& operator<<(std::ostream& os, TailStatus status) {
    switch (status) {
        case TailStatus::Clean:
            return os << "clean";
        case TailStatus::Torn:
            return os << "torn";
        case TailStatus::Corrupt:
            return os << "corrupt";
        case TailStatus::BadHeader:
            return os << "bad header";
    }
    return os << "TailStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook::journal
