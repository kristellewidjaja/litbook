#pragma once

#include "litbook/journal/journal_format.hpp"
#include "litbook/protocol/order_entry_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <vector>

namespace litbook::journal {

// A snapshot is the whole state of the exchange as of one journal record, so recovery
// can start from it and replay only the records after it. The exact byte layout is in
// docs/JOURNAL.md ("Snapshots"). Every number is little-endian, written field by field,
// and a CRC32C of everything before it ends the file.

inline constexpr std::array<char, 8> kSnapshotMagic{'L', 'I', 'T', 'B', 'O', 'O', 'K', 'S'};
inline constexpr std::uint16_t kSnapshotVersion = 1;
inline constexpr std::size_t kSnapshotHeaderLength = 40;
// One live order is 35 bytes in the file and one resting order 26 bytes.
inline constexpr std::size_t kSnapshotOrderLength = 35;
inline constexpr std::size_t kSnapshotRestingLength = 26;

enum class SnapshotStatus : std::uint8_t {
    Ok,
    TooShort,        // Shorter than a header and a checksum.
    BadMagic,        // Not a litbook snapshot.
    BadVersion,      // A version this build cannot read.
    BadChecksum,     // The bytes were damaged (or the file was cut short).
    BadHeader,       // A reserved field is not 0, or the snapshot has no accounts.
    BadLength,       // The lengths and counts do not add up to the file's length.
    BadSetupRecord,  // A setup record has another type or the wrong payload length.
    BadField,        // A side that is not 'B' or 'S', or a flag that is not 0 or 1.
};

std::ostream& operator<<(std::ostream& os, SnapshotStatus status);

// Everything a snapshot file holds.
//
// Invariants (encode_snapshot refuses a snapshot that breaks one). replies and
// state.last_user_ref both have one entry per account plus an unused entry 0, which is 0,
// and every setup record is a Session Start, Add Symbol or Risk Limits record with the
// payload length of its type. Whether the state itself is consistent is checked by
// OrderEntry::restore(), not here.
struct Snapshot {
    Sequence as_of = 0;        // The sequence number of the last journal record it includes.
    Timestamp as_of_time = 0;  // That record's timestamp.
    // The setup records of the journal, so a snapshot can be checked against a start's
    // setup after the journal segments that held them were deleted. Their sequence numbers
    // and timestamps are not kept and read back as 0.
    std::vector<JournalRecord> setup;
    // How many replies each account had been sent, which gives its next sequence number.
    std::vector<std::uint64_t> replies;  // Indexed by OwnerId.
    protocol::OrderEntryState state;
    bool operator==(const Snapshot&) const = default;
};

// The number of bytes encode_snapshot needs, or 0 if it would refuse the snapshot.
[[nodiscard]] std::size_t snapshot_size(const Snapshot& snapshot) noexcept;

// Writes the snapshot into `out` and returns its length. Returns 0 and writes nothing if
// the snapshot breaks an invariant or `out` is too small. Never allocates, so a thread
// can encode into a buffer it reserved at startup.
std::size_t encode_snapshot(const Snapshot& snapshot, std::span<std::byte> out) noexcept;

// Reads a whole snapshot file. The checksum is checked right after the header's magic and
// version, then every field, and the file must end exactly where the snapshot does. So
// decoding is canonical: encoding a decoded snapshot gives back the same bytes. `out` is
// only meaningful when the result is Ok. Allocates the vectors of `out`.
SnapshotStatus decode_snapshot(std::span<const std::byte> bytes, Snapshot& out);

}  // namespace litbook::journal
