#include "litbook/journal/journal_directory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace litbook::journal {

namespace {

std::string numbered_name(Sequence number, const char* suffix) {
    std::array<char, kSequenceDigits + 1> digits{};
    std::snprintf(digits.data(), digits.size(), "%020llu", static_cast<unsigned long long>(number));
    return std::string(digits.data()) + suffix;
}

// Reads the number of a name like "00000000000000000042.journal". Returns false if the
// name is not exactly 20 digits followed by `suffix`.
bool parse_numbered_name(std::string_view name, std::string_view suffix, Sequence& number) {
    if (name.size() != kSequenceDigits + suffix.size() || !name.ends_with(suffix)) {
        return false;
    }
    Sequence value = 0;
    for (std::size_t i = 0; i < kSequenceDigits; ++i) {
        const char c = name[i];
        if (c < '0' || c > '9') {
            return false;
        }
        const auto digit = static_cast<Sequence>(c - '0');
        // 20 digits can hold more than a 64-bit number, so check before multiplying.
        if (value > (UINT64_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    number = value;
    return true;
}

// True if `bytes` is shorter than a file header and is the start of the header a segment
// whose first record is `first` would have.
bool is_cut_header(std::span<const std::byte> bytes, Sequence first) {
    if (bytes.size() >= kFileHeaderLength) {
        return false;
    }
    std::array<std::byte, kFileHeaderLength> expected{};
    encode_file_header(expected, first);
    return std::equal(bytes.begin(), bytes.end(), expected.begin());
}

}  // namespace

std::string segment_file_name(Sequence first) {
    return numbered_name(first, kSegmentSuffix);
}

std::string snapshot_file_name(Sequence as_of) {
    return numbered_name(as_of, kSnapshotSuffix);
}

PathBuffer::PathBuffer(const std::string& directory) {
    std::string_view dir(directory);
    while (dir.size() > 1 && dir.ends_with('/')) {
        dir.remove_suffix(1);
    }
    // Room for the slash, the digits, the longest suffix and the final zero.
    const std::size_t longest = 1 + kSequenceDigits + std::strlen(kTemporarySuffix) + 1;
    if (dir.empty() || dir.size() + longest > path_.size()) {
        throw std::length_error("the journal directory name is empty or too long");
    }
    std::memcpy(directory_.data(), dir.data(), dir.size());
    directory_[dir.size()] = '\0';
    directory_length_ = dir.size();
}

const char* PathBuffer::segment(Sequence first) noexcept {
    return build(first, kSegmentSuffix);
}

const char* PathBuffer::snapshot(Sequence as_of) noexcept {
    return build(as_of, kSnapshotSuffix);
}

const char* PathBuffer::temporary_snapshot(Sequence as_of) noexcept {
    return build(as_of, kTemporarySuffix);
}

const char* PathBuffer::build(Sequence number, const char* suffix) noexcept {
    const int written = std::snprintf(path_.data(), path_.size(), "%s/%020llu%s", directory_.data(),
                                      static_cast<unsigned long long>(number), suffix);
    // The constructor made sure every name fits. If one ever did not, an empty path fails
    // to open, which is safer than a name that was cut short and names another file.
    if (written < 0 || static_cast<std::size_t>(written) >= path_.size()) {
        path_[0] = '\0';
    }
    return path_.data();
}

DirectoryListing list_journal_directory(const std::string& directory) {
    DirectoryListing listing;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        Sequence number = 0;
        if (parse_numbered_name(name, kSegmentSuffix, number)) {
            listing.segments.push_back(SegmentFile{.first = number, .path = entry.path().string()});
        } else if (parse_numbered_name(name, kSnapshotSuffix, number)) {
            listing.snapshots.push_back(
                SnapshotFile{.as_of = number, .path = entry.path().string()});
        } else if (parse_numbered_name(name, kTemporarySuffix, number)) {
            listing.temporary.push_back(entry.path().string());
        }
    }
    std::ranges::sort(listing.segments, {}, &SegmentFile::first);
    std::ranges::sort(listing.snapshots, {}, &SnapshotFile::as_of);
    std::ranges::sort(listing.temporary);
    return listing;
}

std::ostream& operator<<(std::ostream& os, ChainStatus status) {
    switch (status) {
        case ChainStatus::Ok:
            return os << "Ok";
        case ChainStatus::Empty:
            return os << "Empty";
        case ChainStatus::BadSegment:
            return os << "BadSegment";
        case ChainStatus::NameMismatch:
            return os << "NameMismatch";
        case ChainStatus::Gap:
            return os << "Gap";
        case ChainStatus::TornInside:
            return os << "TornInside";
    }
    return os << "ChainStatus(" << static_cast<int>(status) << ")";
}

ChainScan scan_segment_chain(const DirectoryListing& listing) {
    ChainScan chain;
    const std::size_t count = listing.segments.size();
    for (std::size_t i = 0; i < count; ++i) {
        const SegmentFile& file = listing.segments[i];
        const bool last = i + 1 == count;
        const MappedFile mapped(file.path);
        const JournalScan scan = scan_journal(mapped.bytes());

        // The segment this chain expects next starts where the last one ended, or for the
        // very first segment where its name says.
        const Sequence expected = chain.segments.empty() ? file.first : chain.next_sequence;
        if (last && scan.tail == TailStatus::BadHeader && file.first == expected &&
            is_cut_header(mapped.bytes(), expected)) {
            chain.torn_rotation = file.path;
            break;
        }

        chain.problem_at = i;
        chain.problem = SegmentScan{.file = file, .scan = scan};
        if (scan.tail == TailStatus::BadHeader || scan.tail == TailStatus::Corrupt) {
            chain.status = ChainStatus::BadSegment;
            return chain;
        }
        if (scan.first_sequence != file.first) {
            chain.status = ChainStatus::NameMismatch;
            return chain;
        }
        if (!chain.segments.empty() && scan.first_sequence != chain.next_sequence) {
            chain.status = ChainStatus::Gap;
            return chain;
        }
        if (!last && scan.tail == TailStatus::Torn) {
            chain.status = ChainStatus::TornInside;
            return chain;
        }
        chain.segments.push_back(SegmentScan{.file = file, .scan = scan});
        chain.next_sequence = scan.next_sequence;
        chain.records += scan.records;
    }
    chain.problem_at = 0;
    chain.problem = SegmentScan{};
    chain.status = chain.segments.empty() ? ChainStatus::Empty : ChainStatus::Ok;
    return chain;
}

}  // namespace litbook::journal
