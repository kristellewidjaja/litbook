#include "litbook/journal/snapshot_format.hpp"

#include "litbook/journal/crc32c.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/protocol/byte_order.hpp"

#include <algorithm>
#include <concepts>
#include <cstring>
#include <limits>
#include <ostream>

namespace litbook::journal {

namespace {

// The fixed parts of the body, in bytes.
constexpr std::size_t kSetupRecordOverhead = 6;  // type u8, owner u32, payload length u8.
constexpr std::size_t kCountLength = 4;
constexpr std::size_t kScalarsLength = 8 + 8 + 5 * 8;  // last ref, last match, five stats.
constexpr std::size_t kBookLength = 8 + 4;             // next match, order count.

constexpr char kBuyChar = 'B';
constexpr char kSellChar = 'S';

std::size_t payload_length_of(RecordType type) noexcept {
    switch (type) {
        case RecordType::SessionStart:
            return kSessionStartPayload;
        case RecordType::AddSymbol:
            return kAddSymbolPayload;
        case RecordType::RiskLimits:
            return kRiskLimitsPayload;
        default:
            return 0;
    }
}

bool setup_record_is_valid(const JournalRecord& record) noexcept {
    return is_setup_record(record.type) && record.payload_length == payload_length_of(record.type);
}

// Writes fields one after another into a buffer that the caller made big enough.
class Writer {
public:
    explicit Writer(std::span<std::byte> out) noexcept : out_(out) {}

    template <std::unsigned_integral T>
    void put(T value) noexcept {
        protocol::store_le<T>(out_, at_, value);
        at_ += sizeof(T);
    }
    void put_price(Price price) noexcept { put(static_cast<std::uint64_t>(price)); }
    void put_side(Side side) noexcept {
        put(static_cast<std::uint8_t>(side == Side::Buy ? kBuyChar : kSellChar));
    }
    void put_flag(bool flag) noexcept { put(static_cast<std::uint8_t>(flag ? 1 : 0)); }
    void put_bytes(std::span<const std::byte> bytes) noexcept {
        if (!bytes.empty()) {
            std::memcpy(out_.data() + at_, bytes.data(), bytes.size());
        }
        at_ += bytes.size();
    }
    [[nodiscard]] std::size_t at() const noexcept { return at_; }

private:
    std::span<std::byte> out_;
    std::size_t at_ = 0;
};

// Reads fields one after another. Every take() fails once the bytes run out, and the
// caller turns that into BadLength.
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    template <std::unsigned_integral T>
    bool take(T& value) noexcept {
        if (remaining() < sizeof(T)) {
            return false;
        }
        value = protocol::load_le<T>(bytes_, at_);
        at_ += sizeof(T);
        return true;
    }
    bool take_price(Price& price) noexcept {
        std::uint64_t raw = 0;
        if (!take(raw)) {
            return false;
        }
        price = static_cast<Price>(raw);
        return true;
    }
    bool take_bytes(std::span<std::byte> out) noexcept {
        if (remaining() < out.size()) {
            return false;
        }
        if (!out.empty()) {
            std::memcpy(out.data(), bytes_.data() + at_, out.size());
        }
        at_ += out.size();
        return true;
    }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - at_; }

private:
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
};

// Turns a side byte into a Side. Anything but 'B' or 'S' is refused.
bool side_from(std::uint8_t raw, Side& side) noexcept {
    if (raw == static_cast<std::uint8_t>(kBuyChar)) {
        side = Side::Buy;
        return true;
    }
    if (raw == static_cast<std::uint8_t>(kSellChar)) {
        side = Side::Sell;
        return true;
    }
    return false;
}

bool flag_from(std::uint8_t raw, bool& flag) noexcept {
    if (raw > 1) {
        return false;
    }
    flag = raw == 1;
    return true;
}

// The size of everything between the header and the checksum, or 0 if the snapshot
// breaks an invariant.
std::size_t body_size(const Snapshot& s) noexcept {
    const std::size_t accounts_plus_one = s.replies.size();
    if (accounts_plus_one < 2 || s.state.last_user_ref.size() != accounts_plus_one ||
        s.replies[0] != 0 || s.state.last_user_ref[0] != 0) {
        return 0;
    }
    constexpr std::size_t kMaxCount = std::numeric_limits<std::uint32_t>::max();
    if (s.setup.size() > kMaxCount || s.state.books.size() > kMaxCount ||
        s.state.orders.size() > kMaxCount || s.state.resting.size() > kMaxCount ||
        accounts_plus_one - 1 > kMaxCount) {
        return 0;
    }
    std::size_t size = kCountLength;
    for (const JournalRecord& record : s.setup) {
        if (!setup_record_is_valid(record)) {
            return 0;
        }
        size += kSetupRecordOverhead + record.payload_length;
    }
    const std::size_t accounts = accounts_plus_one - 1;
    size += accounts * (8 + 4);  // Replies u64 and last UserRefNum u32 per account.
    size += kScalarsLength;
    size += kCountLength + s.state.books.size() * kBookLength;
    size += kCountLength + s.state.orders.size() * kSnapshotOrderLength;
    size += kCountLength + s.state.resting.size() * kSnapshotRestingLength;
    return size;
}

void write_body(const Snapshot& s, Writer& w) noexcept {
    w.put(static_cast<std::uint32_t>(s.setup.size()));
    for (const JournalRecord& record : s.setup) {
        w.put(static_cast<std::uint8_t>(record.type));
        w.put(record.owner);
        w.put(record.payload_length);
        w.put_bytes(record.payload_bytes());
    }
    const std::size_t accounts = s.replies.size() - 1;
    for (std::size_t owner = 1; owner <= accounts; ++owner) {
        w.put(s.replies[owner]);
    }
    for (std::size_t owner = 1; owner <= accounts; ++owner) {
        w.put(s.state.last_user_ref[owner]);
    }
    const protocol::OrderEntryStats& stats = s.state.stats;
    w.put(s.state.last_order_ref);
    w.put(s.state.last_match_id);
    w.put(stats.malformed);
    w.put(stats.bad_owner);
    w.put(stats.stale_user_ref);
    w.put(stats.unknown_order);
    w.put(stats.no_effect_cancel);

    w.put(static_cast<std::uint32_t>(s.state.books.size()));
    for (const protocol::BookState& book : s.state.books) {
        w.put(book.next_match_id);
        w.put(book.order_count);
    }
    w.put(static_cast<std::uint32_t>(s.state.orders.size()));
    for (const protocol::LiveOrderState& order : s.state.orders) {
        w.put(order.ref);
        w.put(order.owner);
        w.put(order.user_ref);
        w.put(order.symbol);
        w.put_side(order.side);
        w.put_price(order.price);
        w.put(order.open);
        w.put(order.executed);
    }
    w.put(static_cast<std::uint32_t>(s.state.resting.size()));
    for (const RestingOrder& order : s.state.resting) {
        w.put(order.id);
        w.put(order.owner);
        w.put_side(order.side);
        w.put_price(order.price);
        w.put(order.remaining);
        w.put_flag(order.post_only);
    }
}

// Reads a count and checks that `count` entries of `entry_length` bytes can still fit,
// before anything is allocated for them.
bool take_count(Reader& r, std::size_t entry_length, std::uint32_t& count) noexcept {
    if (!r.take(count)) {
        return false;
    }
    return static_cast<std::size_t>(count) <= r.remaining() / entry_length;
}

SnapshotStatus read_setup(Reader& r, Snapshot& out) {
    std::uint32_t count = 0;
    if (!take_count(r, kSetupRecordOverhead, count)) {
        return SnapshotStatus::BadLength;
    }
    out.setup.assign(count, JournalRecord{});
    for (JournalRecord& record : out.setup) {
        std::uint8_t type = 0;
        if (!r.take(type) || !r.take(record.owner) || !r.take(record.payload_length)) {
            return SnapshotStatus::BadLength;
        }
        record.type = static_cast<RecordType>(type);
        if (record.payload_length > kMaxPayload) {
            return SnapshotStatus::BadSetupRecord;
        }
        if (!r.take_bytes(std::span<std::byte>(record.payload.data(), record.payload_length))) {
            return SnapshotStatus::BadLength;
        }
        if (!setup_record_is_valid(record)) {
            return SnapshotStatus::BadSetupRecord;
        }
    }
    return SnapshotStatus::Ok;
}

SnapshotStatus read_accounts_and_scalars(Reader& r, std::uint32_t accounts, Snapshot& out) {
    if (static_cast<std::size_t>(accounts) > r.remaining() / (8 + 4)) {
        return SnapshotStatus::BadLength;
    }
    out.replies.assign(static_cast<std::size_t>(accounts) + 1, 0);
    out.state.last_user_ref.assign(static_cast<std::size_t>(accounts) + 1, 0);
    for (std::size_t owner = 1; owner <= accounts; ++owner) {
        if (!r.take(out.replies[owner])) {
            return SnapshotStatus::BadLength;
        }
    }
    for (std::size_t owner = 1; owner <= accounts; ++owner) {
        if (!r.take(out.state.last_user_ref[owner])) {
            return SnapshotStatus::BadLength;
        }
    }
    protocol::OrderEntryStats& stats = out.state.stats;
    if (!r.take(out.state.last_order_ref) || !r.take(out.state.last_match_id) ||
        !r.take(stats.malformed) || !r.take(stats.bad_owner) || !r.take(stats.stale_user_ref) ||
        !r.take(stats.unknown_order) || !r.take(stats.no_effect_cancel)) {
        return SnapshotStatus::BadLength;
    }
    return SnapshotStatus::Ok;
}

SnapshotStatus read_books_and_orders(Reader& r, Snapshot& out) {
    std::uint32_t books = 0;
    if (!take_count(r, kBookLength, books)) {
        return SnapshotStatus::BadLength;
    }
    out.state.books.assign(books, protocol::BookState{});
    for (protocol::BookState& book : out.state.books) {
        if (!r.take(book.next_match_id) || !r.take(book.order_count)) {
            return SnapshotStatus::BadLength;
        }
    }

    std::uint32_t orders = 0;
    if (!take_count(r, kSnapshotOrderLength, orders)) {
        return SnapshotStatus::BadLength;
    }
    out.state.orders.assign(orders, protocol::LiveOrderState{});
    for (protocol::LiveOrderState& order : out.state.orders) {
        std::uint8_t side = 0;
        if (!r.take(order.ref) || !r.take(order.owner) || !r.take(order.user_ref) ||
            !r.take(order.symbol) || !r.take(side) || !r.take_price(order.price) ||
            !r.take(order.open) || !r.take(order.executed)) {
            return SnapshotStatus::BadLength;
        }
        if (!side_from(side, order.side)) {
            return SnapshotStatus::BadField;
        }
    }

    std::uint32_t resting = 0;
    if (!take_count(r, kSnapshotRestingLength, resting)) {
        return SnapshotStatus::BadLength;
    }
    out.state.resting.assign(resting, RestingOrder{});
    for (RestingOrder& order : out.state.resting) {
        std::uint8_t side = 0;
        std::uint8_t post_only = 0;
        if (!r.take(order.id) || !r.take(order.owner) || !r.take(side) ||
            !r.take_price(order.price) || !r.take(order.remaining) || !r.take(post_only)) {
            return SnapshotStatus::BadLength;
        }
        if (!side_from(side, order.side) || !flag_from(post_only, order.post_only)) {
            return SnapshotStatus::BadField;
        }
    }
    return SnapshotStatus::Ok;
}

}  // namespace

std::ostream& operator<<(std::ostream& os, SnapshotStatus status) {
    switch (status) {
        case SnapshotStatus::Ok:
            return os << "Ok";
        case SnapshotStatus::TooShort:
            return os << "TooShort";
        case SnapshotStatus::BadMagic:
            return os << "BadMagic";
        case SnapshotStatus::BadVersion:
            return os << "BadVersion";
        case SnapshotStatus::BadChecksum:
            return os << "BadChecksum";
        case SnapshotStatus::BadHeader:
            return os << "BadHeader";
        case SnapshotStatus::BadLength:
            return os << "BadLength";
        case SnapshotStatus::BadSetupRecord:
            return os << "BadSetupRecord";
        case SnapshotStatus::BadField:
            return os << "BadField";
    }
    return os << "SnapshotStatus(" << static_cast<int>(status) << ")";
}

std::size_t snapshot_size(const Snapshot& snapshot) noexcept {
    const std::size_t body = body_size(snapshot);
    if (body == 0 || body > std::numeric_limits<std::uint32_t>::max()) {
        return 0;
    }
    return kSnapshotHeaderLength + body + kChecksumLength;
}

std::size_t encode_snapshot(const Snapshot& snapshot, std::span<std::byte> out) noexcept {
    const std::size_t size = snapshot_size(snapshot);
    if (size == 0 || out.size() < size) {
        return 0;
    }
    const std::span<std::byte> file = out.first(size);
    Writer w(file);
    for (const char c : kSnapshotMagic) {
        w.put(static_cast<std::uint8_t>(c));
    }
    w.put(kSnapshotVersion);
    w.put(std::uint16_t{0});
    w.put(static_cast<std::uint32_t>(snapshot.replies.size() - 1));
    w.put(snapshot.as_of);
    w.put(snapshot.as_of_time);
    w.put(static_cast<std::uint32_t>(size - kSnapshotHeaderLength - kChecksumLength));
    w.put(std::uint32_t{0});
    write_body(snapshot, w);
    const std::uint32_t crc = crc32c(file.first(w.at()));
    w.put(crc);
    return size;
}

SnapshotStatus decode_snapshot(std::span<const std::byte> bytes, Snapshot& out) {
    if (bytes.size() < kSnapshotHeaderLength + kChecksumLength) {
        return SnapshotStatus::TooShort;
    }
    for (std::size_t i = 0; i < kSnapshotMagic.size(); ++i) {
        if (bytes[i] != static_cast<std::byte>(kSnapshotMagic[i])) {
            return SnapshotStatus::BadMagic;
        }
    }
    if (protocol::load_le<std::uint16_t>(bytes, 8) != kSnapshotVersion) {
        return SnapshotStatus::BadVersion;
    }
    const std::size_t crc_at = bytes.size() - kChecksumLength;
    if (protocol::load_le<std::uint32_t>(bytes, crc_at) != crc32c(bytes.first(crc_at))) {
        return SnapshotStatus::BadChecksum;
    }

    Reader header(bytes.first(kSnapshotHeaderLength));
    std::array<std::uint8_t, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t reserved16 = 0;
    std::uint32_t accounts = 0;
    std::uint32_t body_length = 0;
    std::uint32_t reserved32 = 0;
    for (std::uint8_t& c : magic) {
        header.take(c);
    }
    header.take(version);
    header.take(reserved16);
    header.take(accounts);
    header.take(out.as_of);
    header.take(out.as_of_time);
    header.take(body_length);
    header.take(reserved32);
    if (reserved16 != 0 || reserved32 != 0 || accounts == 0) {
        return SnapshotStatus::BadHeader;
    }
    if (body_length != crc_at - kSnapshotHeaderLength) {
        return SnapshotStatus::BadLength;
    }

    Reader body(bytes.subspan(kSnapshotHeaderLength, body_length));
    SnapshotStatus status = read_setup(body, out);
    if (status == SnapshotStatus::Ok) {
        status = read_accounts_and_scalars(body, accounts, out);
    }
    if (status == SnapshotStatus::Ok) {
        status = read_books_and_orders(body, out);
    }
    if (status == SnapshotStatus::Ok && body.remaining() != 0) {
        status = SnapshotStatus::BadLength;
    }
    return status;
}

}  // namespace litbook::journal
