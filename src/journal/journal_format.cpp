#include "litbook/journal/journal_format.hpp"

#include "litbook/journal/crc32c.hpp"
#include "litbook/protocol/byte_order.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <ostream>

namespace litbook::journal {

using protocol::load_le;
using protocol::store_le;

namespace {

// Offsets inside the file header.
constexpr std::size_t kHeaderMagic = 0;
constexpr std::size_t kHeaderVersion = 8;
constexpr std::size_t kHeaderLengthField = 10;
constexpr std::size_t kHeaderFlags = 12;
constexpr std::size_t kHeaderFirstSequence = 16;
constexpr std::size_t kHeaderReserved = 24;
constexpr std::size_t kHeaderChecksum = 28;

// Offsets inside every record header.
constexpr std::size_t kLength = 0;
constexpr std::size_t kType = 2;
constexpr std::size_t kReserved = 3;
constexpr std::size_t kOwner = 4;
constexpr std::size_t kSequence = 8;
constexpr std::size_t kTime = 16;
constexpr std::size_t kPayload = kRecordHeaderLength;

// Offsets inside the SessionStart payload.
constexpr std::size_t kMaxOwners = 0;
constexpr std::size_t kMaxLiveOrders = 4;
constexpr std::size_t kMaxSymbols = 8;

// Offsets inside the AddSymbol payload.
constexpr std::size_t kSymbolId = 0;
constexpr std::size_t kTicker = 2;
constexpr std::size_t kSelfTrade = 10;
constexpr std::size_t kSymbolReserved = 11;
constexpr std::size_t kLotSize = 12;
constexpr std::size_t kMaxOrders = 16;
constexpr std::size_t kTickSize = 20;
constexpr std::size_t kMinPrice = 28;
constexpr std::size_t kMaxPrice = 36;

// Offsets inside the RiskLimits payload.
constexpr std::size_t kMaxOrderQty = 0;
constexpr std::size_t kMaxOpenOrders = 4;

// The engine supports at most this many stocks (Engine::kMaxSymbols), one per SymbolId.
constexpr std::size_t kMaxSymbolCount = std::numeric_limits<SymbolId>::max();

constexpr std::uint8_t kLastSelfTradePolicy = static_cast<std::uint8_t>(SelfTradePolicy::Decrement);

// Prices are signed. They are stored as the unsigned number with the same bits, which
// C++20 defines as two's complement, so the conversion is exact both ways.
std::uint64_t price_bits(Price price) noexcept {
    return static_cast<std::uint64_t>(price);
}
Price price_from_bits(std::uint64_t bits) noexcept {
    return static_cast<Price>(bits);
}

bool is_known_type(std::uint8_t letter) noexcept {
    switch (static_cast<RecordType>(letter)) {
        case RecordType::SessionStart:
        case RecordType::AddSymbol:
        case RecordType::RiskLimits:
        case RecordType::Frame:
        case RecordType::CancelAll:
        case RecordType::Stop:
        case RecordType::Restart:
            return true;
    }
    return false;
}

// True if a payload of `length` bytes is right for the type.
bool payload_length_fits(RecordType type, std::size_t length) noexcept {
    switch (type) {
        case RecordType::SessionStart:
            return length == kSessionStartPayload;
        case RecordType::AddSymbol:
            return length == kAddSymbolPayload;
        case RecordType::RiskLimits:
            return length == kRiskLimitsPayload;
        case RecordType::Frame:
            return length >= 1 && length <= kMaxPayload;
        case RecordType::CancelAll:
        case RecordType::Stop:
        case RecordType::Restart:
            return length == 0;
    }
    return false;
}

// Setup records, Stop and Restart belong to no account, risk limits belong to a real account, and
// frames and cancels may name any owner, because replay must see exactly what the
// matching thread saw (the order entry layer counts a bad owner itself).
bool owner_fits(RecordType type, OwnerId owner) noexcept {
    switch (type) {
        case RecordType::SessionStart:
        case RecordType::AddSymbol:
        case RecordType::Stop:
        case RecordType::Restart:
            return owner == kNoOwner;
        case RecordType::RiskLimits:
            return owner != kNoOwner;
        case RecordType::Frame:
        case RecordType::CancelAll:
            return true;
    }
    return false;
}

bool session_config_is_valid(const protocol::OrderEntryConfig& config) noexcept {
    return config.max_owners >= 1 && config.max_live_orders >= 1 && config.max_symbols >= 1 &&
           config.max_symbols <= kMaxSymbolCount;
}

void put_book_config(std::span<std::byte> payload, const SymbolListing& listing) noexcept {
    store_le(payload, kSymbolId, listing.id);
    const auto& ticker = listing.ticker.bytes();
    for (std::size_t i = 0; i < Ticker::kLength; ++i) {
        payload[kTicker + i] = static_cast<std::byte>(ticker[i]);
    }
    payload[kSelfTrade] = static_cast<std::byte>(listing.config.self_trade_policy);
    payload[kSymbolReserved] = std::byte{0};
    store_le(payload, kLotSize, listing.config.lot_size);
    store_le(payload, kMaxOrders, listing.config.max_orders);
    store_le(payload, kTickSize, price_bits(listing.config.tick_size));
    store_le(payload, kMinPrice, price_bits(listing.config.min_price));
    store_le(payload, kMaxPrice, price_bits(listing.config.max_price));
}

std::optional<SymbolListing> read_listing(std::span<const std::byte> payload) noexcept {
    std::array<char, Ticker::kLength> chars{};
    for (std::size_t i = 0; i < Ticker::kLength; ++i) {
        chars[i] = static_cast<char>(payload[kTicker + i]);
    }
    const std::optional<Ticker> ticker = parse_ticker(chars);
    if (!ticker) {
        return std::nullopt;
    }
    const auto policy = std::to_integer<std::uint8_t>(payload[kSelfTrade]);
    if (policy > kLastSelfTradePolicy || payload[kSymbolReserved] != std::byte{0}) {
        return std::nullopt;
    }
    BookConfig config;
    config.tick_size = price_from_bits(load_le<std::uint64_t>(payload, kTickSize));
    config.lot_size = load_le<std::uint32_t>(payload, kLotSize);
    config.self_trade_policy = static_cast<SelfTradePolicy>(policy);
    config.min_price = price_from_bits(load_le<std::uint64_t>(payload, kMinPrice));
    config.max_price = price_from_bits(load_le<std::uint64_t>(payload, kMaxPrice));
    config.max_orders = load_le<std::uint32_t>(payload, kMaxOrders);
    if (!book_config_is_valid(config)) {
        return std::nullopt;
    }
    return SymbolListing{
        .id = load_le<SymbolId>(payload, kSymbolId), .ticker = *ticker, .config = config};
}

protocol::OrderEntryConfig read_session_config(std::span<const std::byte> payload) noexcept {
    protocol::OrderEntryConfig config;
    config.max_owners = load_le<std::uint32_t>(payload, kMaxOwners);
    config.max_live_orders = load_le<std::uint32_t>(payload, kMaxLiveOrders);
    config.max_symbols = load_le<std::uint32_t>(payload, kMaxSymbols);
    return config;
}

// True if the payload fields of a record of this type are in range. Only called once
// the length is known to fit the type.
bool payload_is_valid(RecordType type, std::span<const std::byte> payload) noexcept {
    switch (type) {
        case RecordType::SessionStart:
            return session_config_is_valid(read_session_config(payload));
        case RecordType::AddSymbol:
            return read_listing(payload).has_value();
        case RecordType::RiskLimits:
        case RecordType::Frame:
        case RecordType::CancelAll:
        case RecordType::Stop:
        case RecordType::Restart:
            return true;  // Every value of their fields is allowed.
    }
    return false;
}

// Writes one whole record: the header, the payload and the checksum. Returns 0 if `out`
// is too small. The caller has already checked the payload.
std::size_t write_record(std::span<std::byte> out, RecordType type, OwnerId owner,
                         Sequence sequence, Timestamp now,
                         std::span<const std::byte> payload) noexcept {
    const std::size_t length = kMinRecordLength + payload.size();
    if (out.size() < length || payload.size() > kMaxPayload) {
        return 0;
    }
    store_le(out, kLength, static_cast<std::uint16_t>(length));
    out[kType] = static_cast<std::byte>(type);
    out[kReserved] = std::byte{0};
    store_le(out, kOwner, owner);
    store_le(out, kSequence, sequence);
    store_le(out, kTime, now);
    std::ranges::copy(payload, out.begin() + kPayload);
    const std::size_t checked = length - kChecksumLength;
    store_le(out, checked, crc32c(out.first(checked)));
    return length;
}

}  // namespace

bool book_config_is_valid(const BookConfig& config) noexcept {
    return config.tick_size >= 1 && config.lot_size >= 1 && config.min_price <= config.max_price &&
           config.max_orders >= 1;
}

std::size_t encode_file_header(std::span<std::byte> out, Sequence first_sequence) noexcept {
    if (out.size() < kFileHeaderLength || first_sequence == 0) {
        return 0;
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        out[kHeaderMagic + i] = static_cast<std::byte>(kMagic[i]);
    }
    store_le(out, kHeaderVersion, kVersion);
    store_le(out, kHeaderLengthField, static_cast<std::uint16_t>(kFileHeaderLength));
    store_le(out, kHeaderFlags, std::uint32_t{0});
    store_le(out, kHeaderFirstSequence, first_sequence);
    store_le(out, kHeaderReserved, std::uint32_t{0});
    store_le(out, kHeaderChecksum, crc32c(out.first(kHeaderChecksum)));
    return kFileHeaderLength;
}

DecodedHeader decode_file_header(std::span<const std::byte> bytes) noexcept {
    DecodedHeader out;
    if (bytes.size() < kFileHeaderLength) {
        out.status = HeaderStatus::Incomplete;
        return out;
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (bytes[kHeaderMagic + i] != static_cast<std::byte>(kMagic[i])) {
            out.status = HeaderStatus::BadMagic;
            return out;
        }
    }
    // The checksum is checked before the other fields, so a header that was only half
    // written, or damaged, is reported as such instead of as a strange version.
    if (load_le<std::uint32_t>(bytes, kHeaderChecksum) != crc32c(bytes.first(kHeaderChecksum))) {
        out.status = HeaderStatus::BadChecksum;
        return out;
    }
    if (load_le<std::uint16_t>(bytes, kHeaderVersion) != kVersion) {
        out.status = HeaderStatus::BadVersion;
        return out;
    }
    if (load_le<std::uint16_t>(bytes, kHeaderLengthField) != kFileHeaderLength) {
        out.status = HeaderStatus::BadLength;
        return out;
    }
    if (load_le<std::uint32_t>(bytes, kHeaderFlags) != 0 ||
        load_le<std::uint32_t>(bytes, kHeaderReserved) != 0) {
        out.status = HeaderStatus::BadReserved;
        return out;
    }
    const auto first = load_le<Sequence>(bytes, kHeaderFirstSequence);
    if (first == 0) {
        out.status = HeaderStatus::BadSequence;
        return out;
    }
    out.status = HeaderStatus::Ok;
    out.first_sequence = first;
    return out;
}

std::size_t encode_session_start(std::span<std::byte> out, Sequence sequence, Timestamp now,
                                 const protocol::OrderEntryConfig& config) noexcept {
    if (!session_config_is_valid(config)) {
        return 0;
    }
    std::array<std::byte, kSessionStartPayload> payload{};
    store_le(payload, kMaxOwners, config.max_owners);
    store_le(payload, kMaxLiveOrders, config.max_live_orders);
    store_le(payload, kMaxSymbols, static_cast<std::uint32_t>(config.max_symbols));
    return write_record(out, RecordType::SessionStart, kNoOwner, sequence, now, payload);
}

std::size_t encode_add_symbol(std::span<std::byte> out, Sequence sequence, Timestamp now,
                              const SymbolListing& listing) noexcept {
    if (!book_config_is_valid(listing.config)) {
        return 0;
    }
    std::array<std::byte, kAddSymbolPayload> payload{};
    put_book_config(payload, listing);
    return write_record(out, RecordType::AddSymbol, kNoOwner, sequence, now, payload);
}

std::size_t encode_risk_limits(std::span<std::byte> out, Sequence sequence, Timestamp now,
                               OwnerId owner, const protocol::RiskLimits& limits) noexcept {
    if (owner == kNoOwner) {
        return 0;
    }
    std::array<std::byte, kRiskLimitsPayload> payload{};
    store_le(payload, kMaxOrderQty, limits.max_order_qty);
    store_le(payload, kMaxOpenOrders, limits.max_open_orders);
    return write_record(out, RecordType::RiskLimits, owner, sequence, now, payload);
}

std::size_t encode_request(std::span<std::byte> out, Sequence sequence,
                           const pipeline::Request& request) noexcept {
    switch (request.kind) {
        case pipeline::RequestKind::Frame:
            if (request.length == 0 || request.length > kMaxPayload) {
                return 0;
            }
            return write_record(out, RecordType::Frame, request.owner, sequence, request.now,
                                request.bytes());
        case pipeline::RequestKind::CancelAll:
            return write_record(out, RecordType::CancelAll, request.owner, sequence, request.now,
                                {});
        case pipeline::RequestKind::Stop:
            // Stop belongs to no account, whatever the request's owner field says.
            return write_record(out, RecordType::Stop, kNoOwner, sequence, request.now, {});
    }
    return 0;
}

std::size_t encode_restart(std::span<std::byte> out, Sequence sequence, Timestamp now) noexcept {
    return write_record(out, RecordType::Restart, kNoOwner, sequence, now, {});
}

std::size_t encode_record(std::span<std::byte> out, const JournalRecord& record) noexcept {
    const std::span<const std::byte> payload = record.payload_bytes();
    if (!payload_length_fits(record.type, payload.size()) ||
        !owner_fits(record.type, record.owner) || !payload_is_valid(record.type, payload)) {
        return 0;
    }
    return write_record(out, record.type, record.owner, record.sequence, record.now, payload);
}

DecodedRecord decode_record(std::span<const std::byte> bytes) noexcept {
    DecodedRecord out;
    if (bytes.size() < sizeof(std::uint16_t)) {
        out.status = RecordStatus::Incomplete;
        return out;
    }
    const std::size_t length = load_le<std::uint16_t>(bytes, kLength);
    if (length < kMinRecordLength || length > kMaxRecordLength) {
        out.status = RecordStatus::BadLength;
        return out;
    }
    if (bytes.size() < length) {
        out.status = RecordStatus::Incomplete;
        return out;
    }
    // The checksum comes first, so random or half-written bytes are reported as a bad
    // checksum, which is what they are, rather than as some field that happens to be odd.
    const std::size_t checked = length - kChecksumLength;
    if (load_le<std::uint32_t>(bytes, checked) != crc32c(bytes.first(checked))) {
        out.status = RecordStatus::BadChecksum;
        return out;
    }
    const auto letter = std::to_integer<std::uint8_t>(bytes[kType]);
    if (!is_known_type(letter)) {
        out.status = RecordStatus::BadType;
        return out;
    }
    const auto type = static_cast<RecordType>(letter);
    const std::size_t payload_length = length - kMinRecordLength;
    if (!payload_length_fits(type, payload_length)) {
        out.status = RecordStatus::BadLength;
        return out;
    }
    if (bytes[kReserved] != std::byte{0}) {
        out.status = RecordStatus::BadReserved;
        return out;
    }
    const auto owner = load_le<OwnerId>(bytes, kOwner);
    if (!owner_fits(type, owner)) {
        out.status = RecordStatus::BadOwner;
        return out;
    }
    const std::span<const std::byte> payload = bytes.subspan(kPayload, payload_length);
    if (!payload_is_valid(type, payload)) {
        out.status = RecordStatus::BadPayload;
        return out;
    }
    out.status = RecordStatus::Ok;
    out.length = length;
    out.record.type = type;
    out.record.owner = owner;
    out.record.sequence = load_le<std::uint64_t>(bytes, kSequence);
    out.record.now = load_le<std::uint64_t>(bytes, kTime);
    out.record.payload_length = static_cast<std::uint8_t>(payload_length);
    std::ranges::copy(payload, out.record.payload.begin());
    return out;
}

std::optional<protocol::OrderEntryConfig> session_config_of(const JournalRecord& record) noexcept {
    if (record.type != RecordType::SessionStart || record.payload_length != kSessionStartPayload) {
        return std::nullopt;
    }
    return read_session_config(record.payload_bytes());
}

std::optional<SymbolListing> symbol_listing_of(const JournalRecord& record) noexcept {
    if (record.type != RecordType::AddSymbol || record.payload_length != kAddSymbolPayload) {
        return std::nullopt;
    }
    return read_listing(record.payload_bytes());
}

std::optional<protocol::RiskLimits> risk_limits_of(const JournalRecord& record) noexcept {
    if (record.type != RecordType::RiskLimits || record.payload_length != kRiskLimitsPayload) {
        return std::nullopt;
    }
    protocol::RiskLimits limits;
    limits.max_order_qty = load_le<std::uint32_t>(record.payload_bytes(), kMaxOrderQty);
    limits.max_open_orders = load_le<std::uint32_t>(record.payload_bytes(), kMaxOpenOrders);
    return limits;
}

std::optional<pipeline::Request> request_of(const JournalRecord& record) noexcept {
    switch (record.type) {
        case RecordType::Frame:
            return pipeline::make_frame_request(record.owner, record.payload_bytes(), record.now);
        case RecordType::CancelAll:
            return pipeline::make_cancel_all_request(record.owner, record.now);
        case RecordType::Stop:
            return pipeline::make_stop_request(record.now);
        case RecordType::SessionStart:
        case RecordType::AddSymbol:
        case RecordType::RiskLimits:
        case RecordType::Restart:
            return std::nullopt;
    }
    return std::nullopt;
}

std::ostream& operator<<(std::ostream& os, RecordType type) {
    switch (type) {
        case RecordType::SessionStart:
            return os << "SessionStart";
        case RecordType::AddSymbol:
            return os << "AddSymbol";
        case RecordType::RiskLimits:
            return os << "RiskLimits";
        case RecordType::Frame:
            return os << "Frame";
        case RecordType::CancelAll:
            return os << "CancelAll";
        case RecordType::Stop:
            return os << "Stop";
        case RecordType::Restart:
            return os << "Restart";
    }
    return os << "RecordType(" << static_cast<int>(type) << ")";
}

std::ostream& operator<<(std::ostream& os, RecordStatus status) {
    switch (status) {
        case RecordStatus::Ok:
            return os << "Ok";
        case RecordStatus::Incomplete:
            return os << "Incomplete";
        case RecordStatus::BadLength:
            return os << "BadLength";
        case RecordStatus::BadChecksum:
            return os << "BadChecksum";
        case RecordStatus::BadType:
            return os << "BadType";
        case RecordStatus::BadReserved:
            return os << "BadReserved";
        case RecordStatus::BadOwner:
            return os << "BadOwner";
        case RecordStatus::BadPayload:
            return os << "BadPayload";
    }
    return os << "RecordStatus(" << static_cast<int>(status) << ")";
}

std::ostream& operator<<(std::ostream& os, HeaderStatus status) {
    switch (status) {
        case HeaderStatus::Ok:
            return os << "Ok";
        case HeaderStatus::Incomplete:
            return os << "Incomplete";
        case HeaderStatus::BadMagic:
            return os << "BadMagic";
        case HeaderStatus::BadVersion:
            return os << "BadVersion";
        case HeaderStatus::BadLength:
            return os << "BadLength";
        case HeaderStatus::BadReserved:
            return os << "BadReserved";
        case HeaderStatus::BadChecksum:
            return os << "BadChecksum";
        case HeaderStatus::BadSequence:
            return os << "BadSequence";
    }
    return os << "HeaderStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook::journal
