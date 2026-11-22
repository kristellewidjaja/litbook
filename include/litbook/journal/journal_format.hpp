#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/order_entry_config.hpp"
#include "litbook/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>

namespace litbook::journal {

// The byte layout of the write-ahead journal, described field by field in
// docs/JOURNAL.md. A journal file is one file header followed by records. Every number is
// little-endian, the raw OUCH bytes inside a Frame record stay exactly as the client sent
// them, and every record ends with the CRC32C of its other bytes.
//
// The functions here only turn values into bytes and back. They never touch a file. Each
// encode function returns the number of bytes written, or 0 if the output is too small or
// the value cannot be stored (for example a frame longer than 47 bytes). Each decode
// function checks every field, so a decoded record can always be encoded back into exactly
// the same bytes, and no two different byte strings decode to the same record.

using Sequence = std::uint64_t;
using Timestamp = pipeline::Timestamp;

// The file header.
inline constexpr std::size_t kFileHeaderLength = 32;
inline constexpr std::array<char, 8> kMagic{'L', 'I', 'T', 'B', 'O', 'O', 'K', 'J'};
inline constexpr std::uint16_t kVersion = 1;

// Every record has a 24-byte header, then its payload, then a 4-byte CRC32C.
inline constexpr std::size_t kRecordHeaderLength = 24;
inline constexpr std::size_t kChecksumLength = 4;
inline constexpr std::size_t kMaxPayload = pipeline::kMaxFrame;  // 47, the longest OUCH message.
inline constexpr std::size_t kMinRecordLength = kRecordHeaderLength + kChecksumLength;
inline constexpr std::size_t kMaxRecordLength = kMinRecordLength + kMaxPayload;

// Payload lengths of the record types that have a fixed one.
inline constexpr std::size_t kSessionStartPayload = 12;
inline constexpr std::size_t kAddSymbolPayload = 44;
inline constexpr std::size_t kRiskLimitsPayload = 8;

// Each record type is one ASCII letter, which makes a hex dump easy to read.
enum class RecordType : std::uint8_t {
    SessionStart = 'B',  // The order entry configuration. Always the first record.
    AddSymbol = 'S',     // One stock listed at startup, with its book configuration.
    RiskLimits = 'R',    // One account's risk limits, set at startup.
    Frame = 'F',         // One inbound OUCH message from an account.
    CancelAll = 'C',     // Cancel every live order of an account (cancel on disconnect).
    Stop = 'E',          // The exchange shut down cleanly here.
    Restart = 'A',       // The exchange started again from this journal (recovery).
};

std::ostream& operator<<(std::ostream& os, RecordType type);

enum class RecordStatus : std::uint8_t {
    Ok,
    Incomplete,   // The bytes end before the record does.
    BadLength,    // The length is below 28, above 75, or wrong for the record type.
    BadChecksum,  // The CRC32C does not match the other bytes.
    BadType,      // The type byte is not one of the letters above.
    BadReserved,  // The reserved byte is not 0.
    BadOwner,     // The owner is not allowed for this type (see docs/JOURNAL.md).
    BadPayload,   // A payload field is out of range (a bad ticker, a tick size of 0, ...).
};

std::ostream& operator<<(std::ostream& os, RecordStatus status);

enum class HeaderStatus : std::uint8_t {
    Ok,
    Incomplete,   // Fewer than 32 bytes.
    BadMagic,     // Not a litbook journal.
    BadVersion,   // A version this program does not read.
    BadLength,    // The header length field is not 32.
    BadReserved,  // A reserved field is not 0.
    BadChecksum,  // The CRC32C does not match.
    BadSequence,  // The first sequence number is 0.
};

std::ostream& operator<<(std::ostream& os, HeaderStatus status);

struct DecodedHeader {
    HeaderStatus status = HeaderStatus::Incomplete;
    Sequence first_sequence = 0;  // The sequence number of the first record.
};

// One record, decoded. The payload is copied out, so the record does not point into the
// bytes it came from.
struct JournalRecord {
    RecordType type = RecordType::Frame;
    OwnerId owner = kNoOwner;
    Sequence sequence = 0;
    Timestamp now = 0;
    std::uint8_t payload_length = 0;
    std::array<std::byte, kMaxPayload> payload{};

    [[nodiscard]] std::span<const std::byte> payload_bytes() const noexcept {
        return {payload.data(), payload_length};
    }
    bool operator==(const JournalRecord&) const = default;
};

struct DecodedRecord {
    RecordStatus status = RecordStatus::Incomplete;
    std::size_t length = 0;  // The bytes the record takes up, when status is Ok.
    JournalRecord record;
};

// One stock as an AddSymbol record stores it. `id` is the SymbolId the engine gave the
// stock, which replay checks it gets again.
struct SymbolListing {
    SymbolId id = 0;
    Ticker ticker;
    BookConfig config;
};

// The file header.
std::size_t encode_file_header(std::span<std::byte> out, Sequence first_sequence) noexcept;
[[nodiscard]] DecodedHeader decode_file_header(std::span<const std::byte> bytes) noexcept;

// Records. A Request becomes a Frame, CancelAll or Stop record.
std::size_t encode_session_start(std::span<std::byte> out, Sequence sequence, Timestamp now,
                                 const protocol::OrderEntryConfig& config) noexcept;
std::size_t encode_add_symbol(std::span<std::byte> out, Sequence sequence, Timestamp now,
                              const SymbolListing& listing) noexcept;
std::size_t encode_risk_limits(std::span<std::byte> out, Sequence sequence, Timestamp now,
                               OwnerId owner, const protocol::RiskLimits& limits) noexcept;
std::size_t encode_request(std::span<std::byte> out, Sequence sequence,
                           const pipeline::Request& request) noexcept;
// The record recovery appends each time the exchange starts again from a journal.
std::size_t encode_restart(std::span<std::byte> out, Sequence sequence, Timestamp now) noexcept;

// Encodes a record that is already decoded, which gives back its exact bytes.
std::size_t encode_record(std::span<std::byte> out, const JournalRecord& record) noexcept;

// Decodes the record at the start of `bytes`. Bytes after it are ignored.
[[nodiscard]] DecodedRecord decode_record(std::span<const std::byte> bytes) noexcept;

// The typed contents of a decoded record. Each returns nothing if the record has another
// type. A record that decoded with status Ok always converts.
[[nodiscard]] std::optional<protocol::OrderEntryConfig> session_config_of(
    const JournalRecord& record) noexcept;
[[nodiscard]] std::optional<SymbolListing> symbol_listing_of(const JournalRecord& record) noexcept;
[[nodiscard]] std::optional<protocol::RiskLimits> risk_limits_of(
    const JournalRecord& record) noexcept;
// For Frame, CancelAll and Stop records.
[[nodiscard]] std::optional<pipeline::Request> request_of(const JournalRecord& record) noexcept;

// True if `config` passes the same checks as validate_config, without throwing.
[[nodiscard]] bool book_config_is_valid(const BookConfig& config) noexcept;

}  // namespace litbook::journal
