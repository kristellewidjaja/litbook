#include "litbook/feed/feed_codec.hpp"

#include "litbook/protocol/byte_order.hpp"

#include <algorithm>
#include <array>
#include <ostream>
#include <utility>

namespace litbook::feed {

namespace {

using protocol::load_be;
using protocol::store_be;

// Field offsets of every message, the same tables as docs/FEED.md. Every message starts
// with its type byte and an 8-byte timestamp, and every message except System Event has
// the locate right after that. Each block ends with a static_assert that the last field
// ends exactly at the message length.
constexpr std::size_t kTimestamp = 1;
constexpr std::size_t kLocate = 9;

namespace system_event {
constexpr std::size_t kCode = 9;
static_assert(kCode + 1 == kSystemEventLength);
}  // namespace system_event

namespace directory {
constexpr std::size_t kSymbol = 11;
constexpr std::size_t kTickSize = 19;
constexpr std::size_t kLotSize = 27;
static_assert(kLotSize + 4 == kSymbolDirectoryLength);
}  // namespace directory

namespace level {
constexpr std::size_t kSide = 11;
constexpr std::size_t kPrice = 12;
constexpr std::size_t kTotalQty = 20;
constexpr std::size_t kOrderCount = 28;
static_assert(kOrderCount + 4 == kLevelUpdateLength);
}  // namespace level

namespace trade {
constexpr std::size_t kSide = 11;
constexpr std::size_t kPrice = 12;
constexpr std::size_t kQty = 20;
constexpr std::size_t kMatch = 24;
static_assert(kMatch + 8 == kTradeLength);
}  // namespace trade

namespace snapshot_start {
constexpr std::size_t kAsOf = 11;
constexpr std::size_t kLevelCount = 19;
constexpr std::size_t kSymbolCount = 23;
static_assert(kSymbolCount + 2 == kSnapshotStartLength);
}  // namespace snapshot_start

namespace snapshot_end {
constexpr std::size_t kAsOf = 11;
static_assert(kAsOf + 8 == kSnapshotEndLength);
}  // namespace snapshot_end

constexpr std::byte as_byte(char c) noexcept {
    return static_cast<std::byte>(c);
}
constexpr char as_char(std::byte b) noexcept {
    return static_cast<char>(b);
}

// Small helpers for the fields that several messages share.

std::optional<Side> decode_side(std::byte b) noexcept {
    switch (as_char(b)) {
        case 'B':
            return Side::Buy;
        case 'S':
            return Side::Sell;
        default:
            return std::nullopt;
    }
}

std::byte encode_side(Side side) noexcept {
    return as_byte(side == Side::Buy ? 'B' : 'S');
}

// A price fits on the wire when it is between 0 and kMaxWirePrice.
bool price_fits(Price price) noexcept {
    return price >= 0 && std::cmp_less_equal(price, kMaxWirePrice);
}

// A level is either empty (both 0) or has both a quantity and at least one order.
bool level_is_consistent(AggregateQty qty, std::uint32_t count) noexcept {
    return (qty == 0) == (count == 0);
}

void write_header(std::span<std::byte> out, char type, Timestamp timestamp) noexcept {
    out[0] = as_byte(type);
    store_be<std::uint64_t>(out, kTimestamp, timestamp);
}

FeedDecoded failed(FeedDecodeStatus status) noexcept {
    return {.status = status, .message = std::nullopt};
}

FeedDecoded decode_system_event(std::span<const std::byte> in) noexcept {
    const char code = as_char(in[system_event::kCode]);
    if (code != 'O' && code != 'C') {
        return failed(FeedDecodeStatus::BadEventCode);
    }
    SystemEvent m;
    m.timestamp = load_be<std::uint64_t>(in, kTimestamp);
    m.code = static_cast<SystemEventCode>(code);
    return {.status = FeedDecodeStatus::Ok, .message = m};
}

FeedDecoded decode_directory(std::span<const std::byte> in) noexcept {
    std::array<char, Ticker::kLength> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw.at(i) = as_char(in[directory::kSymbol + i]);
    }
    const std::optional<Ticker> symbol = parse_ticker(raw);
    if (!symbol) {
        return failed(FeedDecodeStatus::BadSymbol);
    }
    const auto tick = load_be<std::uint64_t>(in, directory::kTickSize);
    if (tick == 0 || tick > kMaxWirePrice) {
        return failed(FeedDecodeStatus::BadPrice);
    }
    const auto lot = load_be<std::uint32_t>(in, directory::kLotSize);
    if (lot == 0) {
        return failed(FeedDecodeStatus::BadLotSize);
    }
    const SymbolDirectory m{.timestamp = load_be<std::uint64_t>(in, kTimestamp),
                            .locate = load_be<std::uint16_t>(in, kLocate),
                            .symbol = *symbol,
                            .tick_size = static_cast<Price>(tick),
                            .lot_size = lot};
    return {.status = FeedDecodeStatus::Ok, .message = m};
}

FeedDecoded decode_level(std::span<const std::byte> in) noexcept {
    const std::optional<Side> side = decode_side(in[level::kSide]);
    if (!side) {
        return failed(FeedDecodeStatus::BadSide);
    }
    const auto price = load_be<std::uint64_t>(in, level::kPrice);
    if (price > kMaxWirePrice) {
        return failed(FeedDecodeStatus::BadPrice);
    }
    const auto qty = load_be<std::uint64_t>(in, level::kTotalQty);
    const auto count = load_be<std::uint32_t>(in, level::kOrderCount);
    if (!level_is_consistent(qty, count)) {
        return failed(FeedDecodeStatus::BadLevel);
    }
    LevelUpdateMessage m;
    m.timestamp = load_be<std::uint64_t>(in, kTimestamp);
    m.locate = load_be<std::uint16_t>(in, kLocate);
    m.side = *side;
    m.price = static_cast<Price>(price);
    m.total_qty = qty;
    m.order_count = count;
    return {.status = FeedDecodeStatus::Ok, .message = m};
}

FeedDecoded decode_trade(std::span<const std::byte> in) noexcept {
    const std::optional<Side> side = decode_side(in[trade::kSide]);
    if (!side) {
        return failed(FeedDecodeStatus::BadSide);
    }
    const auto price = load_be<std::uint64_t>(in, trade::kPrice);
    if (price > kMaxWirePrice) {
        return failed(FeedDecodeStatus::BadPrice);
    }
    const auto qty = load_be<std::uint32_t>(in, trade::kQty);
    if (qty == 0) {
        return failed(FeedDecodeStatus::BadQuantity);
    }
    TradeMessage m;
    m.timestamp = load_be<std::uint64_t>(in, kTimestamp);
    m.locate = load_be<std::uint16_t>(in, kLocate);
    m.aggressor_side = *side;
    m.price = static_cast<Price>(price);
    m.qty = qty;
    m.match_number = load_be<std::uint64_t>(in, trade::kMatch);
    return {.status = FeedDecodeStatus::Ok, .message = m};
}

FeedDecoded decode_snapshot_start(std::span<const std::byte> in) noexcept {
    SnapshotStart m;
    m.timestamp = load_be<std::uint64_t>(in, kTimestamp);
    m.locate = load_be<std::uint16_t>(in, kLocate);
    m.as_of = load_be<std::uint64_t>(in, snapshot_start::kAsOf);
    m.level_count = load_be<std::uint32_t>(in, snapshot_start::kLevelCount);
    m.symbol_count = load_be<std::uint16_t>(in, snapshot_start::kSymbolCount);
    if (m.locate >= m.symbol_count) {
        return failed(FeedDecodeStatus::BadLocate);
    }
    return {.status = FeedDecodeStatus::Ok, .message = m};
}

FeedDecoded decode_snapshot_end(std::span<const std::byte> in) noexcept {
    SnapshotEnd m;
    m.timestamp = load_be<std::uint64_t>(in, kTimestamp);
    m.locate = load_be<std::uint16_t>(in, kLocate);
    m.as_of = load_be<std::uint64_t>(in, snapshot_end::kAsOf);
    return {.status = FeedDecodeStatus::Ok, .message = m};
}

}  // namespace

std::size_t message_length(std::byte type) noexcept {
    switch (as_char(type)) {
        case kSystemEventType:
            return kSystemEventLength;
        case kSymbolDirectoryType:
            return kSymbolDirectoryLength;
        case kLevelUpdateType:
            return kLevelUpdateLength;
        case kTradeType:
            return kTradeLength;
        case kSnapshotStartType:
            return kSnapshotStartLength;
        case kSnapshotEndType:
            return kSnapshotEndLength;
        default:
            return 0;
    }
}

FeedDecoded decode_message(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return failed(FeedDecodeStatus::Empty);
    }
    const std::size_t expected = message_length(bytes[0]);
    if (expected == 0) {
        return failed(FeedDecodeStatus::UnknownType);
    }
    if (bytes.size() != expected) {
        return failed(FeedDecodeStatus::BadLength);
    }
    switch (as_char(bytes[0])) {
        case kSystemEventType:
            return decode_system_event(bytes);
        case kSymbolDirectoryType:
            return decode_directory(bytes);
        case kLevelUpdateType:
            return decode_level(bytes);
        case kTradeType:
            return decode_trade(bytes);
        case kSnapshotStartType:
            return decode_snapshot_start(bytes);
        default:
            return decode_snapshot_end(bytes);
    }
}

std::size_t encode(const SystemEvent& m, std::span<std::byte> out) noexcept {
    const bool known_code =
        m.code == SystemEventCode::StartOfMessages || m.code == SystemEventCode::EndOfMessages;
    if (out.size() < kSystemEventLength || !known_code) {
        return 0;
    }
    write_header(out, kSystemEventType, m.timestamp);
    out[system_event::kCode] = as_byte(static_cast<char>(m.code));
    return kSystemEventLength;
}

std::size_t encode(const SymbolDirectory& m, std::span<std::byte> out) noexcept {
    const bool tick_ok = m.tick_size > 0 && price_fits(m.tick_size);
    if (out.size() < kSymbolDirectoryLength || !tick_ok || m.lot_size == 0) {
        return 0;
    }
    write_header(out, kSymbolDirectoryType, m.timestamp);
    store_be<std::uint16_t>(out, kLocate, m.locate);
    const auto& raw = m.symbol.bytes();
    for (std::size_t i = 0; i < raw.size(); ++i) {
        out[directory::kSymbol + i] = as_byte(raw.at(i));
    }
    store_be<std::uint64_t>(out, directory::kTickSize, static_cast<std::uint64_t>(m.tick_size));
    store_be<std::uint32_t>(out, directory::kLotSize, m.lot_size);
    return kSymbolDirectoryLength;
}

std::size_t encode(const LevelUpdateMessage& m, std::span<std::byte> out) noexcept {
    if (out.size() < kLevelUpdateLength || !price_fits(m.price) ||
        !level_is_consistent(m.total_qty, m.order_count)) {
        return 0;
    }
    write_header(out, kLevelUpdateType, m.timestamp);
    store_be<std::uint16_t>(out, kLocate, m.locate);
    out[level::kSide] = encode_side(m.side);
    store_be<std::uint64_t>(out, level::kPrice, static_cast<std::uint64_t>(m.price));
    store_be<std::uint64_t>(out, level::kTotalQty, m.total_qty);
    store_be<std::uint32_t>(out, level::kOrderCount, m.order_count);
    return kLevelUpdateLength;
}

std::size_t encode(const TradeMessage& m, std::span<std::byte> out) noexcept {
    if (out.size() < kTradeLength || !price_fits(m.price) || m.qty == 0) {
        return 0;
    }
    write_header(out, kTradeType, m.timestamp);
    store_be<std::uint16_t>(out, kLocate, m.locate);
    out[trade::kSide] = encode_side(m.aggressor_side);
    store_be<std::uint64_t>(out, trade::kPrice, static_cast<std::uint64_t>(m.price));
    store_be<std::uint32_t>(out, trade::kQty, m.qty);
    store_be<std::uint64_t>(out, trade::kMatch, m.match_number);
    return kTradeLength;
}

std::size_t encode(const SnapshotStart& m, std::span<std::byte> out) noexcept {
    if (out.size() < kSnapshotStartLength || m.locate >= m.symbol_count) {
        return 0;
    }
    write_header(out, kSnapshotStartType, m.timestamp);
    store_be<std::uint16_t>(out, kLocate, m.locate);
    store_be<std::uint64_t>(out, snapshot_start::kAsOf, m.as_of);
    store_be<std::uint32_t>(out, snapshot_start::kLevelCount, m.level_count);
    store_be<std::uint16_t>(out, snapshot_start::kSymbolCount, m.symbol_count);
    return kSnapshotStartLength;
}

std::size_t encode(const SnapshotEnd& m, std::span<std::byte> out) noexcept {
    if (out.size() < kSnapshotEndLength) {
        return 0;
    }
    write_header(out, kSnapshotEndType, m.timestamp);
    store_be<std::uint16_t>(out, kLocate, m.locate);
    store_be<std::uint64_t>(out, snapshot_end::kAsOf, m.as_of);
    return kSnapshotEndLength;
}

// std::visit could throw std::bad_variant_access, which does not fit a noexcept function,
// so each alternative is tried in turn with get_if.
std::size_t encode(const FeedMessage& m, std::span<std::byte> out) noexcept {
    if (const auto* p = std::get_if<SystemEvent>(&m)) {
        return encode(*p, out);
    }
    if (const auto* p = std::get_if<SymbolDirectory>(&m)) {
        return encode(*p, out);
    }
    if (const auto* p = std::get_if<LevelUpdateMessage>(&m)) {
        return encode(*p, out);
    }
    if (const auto* p = std::get_if<TradeMessage>(&m)) {
        return encode(*p, out);
    }
    if (const auto* p = std::get_if<SnapshotStart>(&m)) {
        return encode(*p, out);
    }
    if (const auto* p = std::get_if<SnapshotEnd>(&m)) {
        return encode(*p, out);
    }
    return 0;
}

std::ostream& operator<<(std::ostream& os, FeedDecodeStatus status) {
    switch (status) {
        case FeedDecodeStatus::Ok:
            return os << "Ok";
        case FeedDecodeStatus::Empty:
            return os << "Empty";
        case FeedDecodeStatus::UnknownType:
            return os << "UnknownType";
        case FeedDecodeStatus::BadLength:
            return os << "BadLength";
        case FeedDecodeStatus::BadEventCode:
            return os << "BadEventCode";
        case FeedDecodeStatus::BadSymbol:
            return os << "BadSymbol";
        case FeedDecodeStatus::BadPrice:
            return os << "BadPrice";
        case FeedDecodeStatus::BadSide:
            return os << "BadSide";
        case FeedDecodeStatus::BadLotSize:
            return os << "BadLotSize";
        case FeedDecodeStatus::BadLevel:
            return os << "BadLevel";
        case FeedDecodeStatus::BadQuantity:
            return os << "BadQuantity";
        case FeedDecodeStatus::BadLocate:
            return os << "BadLocate";
    }
    return os << "FeedDecodeStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook::feed
