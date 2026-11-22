#include "litbook/journal/setup.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>

namespace litbook::journal {

namespace {

// Encodes a record with one of the encoders and decodes it again, so the result is
// exactly what a journal would hold.
template <class Encode>
JournalRecord encoded_record(Encode encode, const char* what) {
    std::array<std::byte, kMaxRecordLength> bytes{};
    const std::size_t n = encode(std::span<std::byte>(bytes));
    if (n == 0) {
        throw std::invalid_argument(what);
    }
    const DecodedRecord decoded = decode_record(std::span<const std::byte>(bytes.data(), n));
    if (decoded.status != RecordStatus::Ok) {
        throw std::invalid_argument(what);
    }
    return decoded.record;
}

bool same_setup(const JournalRecord& a, const JournalRecord& b) noexcept {
    if (a.type != b.type || a.owner != b.owner || a.payload_length != b.payload_length) {
        return false;
    }
    const auto x = a.payload_bytes();
    const auto y = b.payload_bytes();
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i] != y[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

JournalRecord session_start_record(const protocol::OrderEntryConfig& config) {
    return encoded_record(
        [&](std::span<std::byte> out) { return encode_session_start(out, 0, 0, config); },
        "the order entry configuration cannot be journaled");
}

JournalRecord add_symbol_record(const SymbolListing& listing) {
    return encoded_record(
        [&](std::span<std::byte> out) { return encode_add_symbol(out, 0, 0, listing); },
        "the stock's book configuration cannot be journaled");
}

JournalRecord risk_limits_record(OwnerId owner, const protocol::RiskLimits& limits) {
    return encoded_record(
        [&](std::span<std::byte> out) { return encode_risk_limits(out, 0, 0, owner, limits); },
        "risk limits need a real account");
}

bool is_setup_record(RecordType type) noexcept {
    return type == RecordType::SessionStart || type == RecordType::AddSymbol ||
           type == RecordType::RiskLimits;
}

JournalStatus append_setup_record(JournalWriter& writer, Timestamp now,
                                  const JournalRecord& record) {
    switch (record.type) {
        case RecordType::SessionStart: {
            const auto config = session_config_of(record);
            return config ? writer.append_session_start(now, *config) : JournalStatus::Refused;
        }
        case RecordType::AddSymbol: {
            const auto listing = symbol_listing_of(record);
            return listing ? writer.append_add_symbol(now, *listing) : JournalStatus::Refused;
        }
        case RecordType::RiskLimits: {
            const auto limits = risk_limits_of(record);
            return limits ? writer.append_risk_limits(now, record.owner, *limits)
                          : JournalStatus::Refused;
        }
        case RecordType::Frame:
        case RecordType::CancelAll:
        case RecordType::Stop:
        case RecordType::Restart:
            break;
    }
    return JournalStatus::Refused;
}

std::string describe_record(const JournalRecord& record) {
    std::ostringstream os;
    switch (record.type) {
        case RecordType::SessionStart: {
            const auto config = session_config_of(record);
            os << "Session Start";
            if (config) {
                os << " (max owners " << config->max_owners << ", max live orders "
                   << config->max_live_orders << ", max symbols " << config->max_symbols << ")";
            }
            return os.str();
        }
        case RecordType::AddSymbol: {
            const auto listing = symbol_listing_of(record);
            os << "Add Symbol";
            if (listing) {
                const BookConfig& c = listing->config;
                os << " " << listing->ticker.name() << " (id " << listing->id << ", tick "
                   << c.tick_size << ", lot " << c.lot_size << ", max orders " << c.max_orders
                   << ", prices " << c.min_price << " to " << c.max_price << ", policy "
                   << static_cast<int>(c.self_trade_policy) << ")";
            }
            return os.str();
        }
        case RecordType::RiskLimits: {
            const auto limits = risk_limits_of(record);
            os << "Risk Limits for account " << record.owner;
            if (limits) {
                os << " (max order quantity " << limits->max_order_qty << ", max open orders "
                   << limits->max_open_orders << ")";
            }
            return os.str();
        }
        case RecordType::Frame:
        case RecordType::CancelAll:
        case RecordType::Stop:
        case RecordType::Restart:
            break;
    }
    os << record.type;
    return os.str();
}

std::string setup_difference(std::span<const JournalRecord> journal,
                             std::span<const JournalRecord> expected) {
    const std::size_t common = std::min(journal.size(), expected.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (!same_setup(journal[i], expected[i])) {
            return "Setup record " + std::to_string(i + 1) + " differs. The journal has " +
                   describe_record(journal[i]) + ", and this start has " +
                   describe_record(expected[i]) + ".";
        }
    }
    if (journal.size() > common) {
        return "The journal has " + std::to_string(journal.size()) +
               " setup records and this start has " + std::to_string(expected.size()) +
               ". The first extra one is " + describe_record(journal[common]) + ".";
    }
    if (expected.size() > common) {
        return "The journal has " + std::to_string(journal.size()) +
               " setup records and this start has " + std::to_string(expected.size()) +
               ". The first one missing from the journal is " + describe_record(expected[common]) +
               ".";
    }
    return {};
}

}  // namespace litbook::journal
