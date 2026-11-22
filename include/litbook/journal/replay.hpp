#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <stdexcept>

namespace litbook::journal {

enum class ReplayStatus : std::uint8_t {
    Ok,
    MissingSessionStart,  // The first record is not SessionStart.
    SecondSessionStart,   // A journal has exactly one SessionStart.
    SetupAfterRequests,   // A stock or a risk limit came after the first request.
    ConfigRefused,        // The order entry layer refused the SessionStart configuration.
    SymbolRefused,        // The engine refused a stock, or gave it another SymbolId.
    RiskLimitsRefused,    // The account of a RiskLimits record does not exist.
    JournalCorrupt,       // The scan found corruption, so nothing was replayed.
    JournalUnreadable,    // The file header is missing or wrong.
};

std::ostream& operator<<(std::ostream& os, ReplayStatus status);

struct ReplayStats {
    std::uint64_t records = 0;        // Records applied, setup records included.
    std::uint64_t setup_records = 0;  // SessionStart, AddSymbol and RiskLimits.
    std::uint64_t frames = 0;
    std::uint64_t malformed_frames = 0;  // Frames the order entry layer could not decode.
    std::uint64_t cancel_alls = 0;
    std::uint64_t stops = 0;
    Sequence last_sequence = 0;  // The sequence number of the last record applied.
};

// Rebuilds the exchange's state from journal records. It makes the same calls into the
// order entry layer, in the same order and with the same timestamps, as the matching
// thread made when the requests first arrived (see MatchingWorker::handle). The order
// entry layer reads no clock and nothing else outside its inputs, so it ends up in the
// same state and sends the same replies and market events, byte for byte.
//
// The SessionStart record builds the order entry layer, which is why it must come first.
// AddSymbol and RiskLimits records are its startup calls, and may only come before the
// first Frame or CancelAll, as they do in the live exchange. A Stop record marks a clean
// shutdown. Replay just counts it, because the exchange may have been started again
// from the same journal and kept going.
//
// Invariants. Once apply() returns anything but Ok, every later call returns the same
// status and changes nothing. Applying a Frame or CancelAll never allocates.
template <OrderBook Book>
class Replayer {
public:
    // The sinks receive what the order entry layer sends, and must outlive the replayer.
    // `market_data` may be nullptr.
    explicit Replayer(protocol::OutboundSink& replies,
                      protocol::MarketDataSink* market_data = nullptr) noexcept
        : replies_(&replies), market_data_(market_data) {}

    ReplayStatus apply(const JournalRecord& record) {
        if (status_ != ReplayStatus::Ok) {
            return status_;
        }
        now_ = record.now;
        if (!order_entry_ && record.type != RecordType::SessionStart) {
            return fail(ReplayStatus::MissingSessionStart);
        }
        switch (record.type) {
            case RecordType::SessionStart:
                return start(record);
            case RecordType::AddSymbol:
                return add_symbol(record);
            case RecordType::RiskLimits:
                return set_risk_limits(record);
            case RecordType::Frame:
                requests_seen_ = true;
                ++stats_.frames;
                if (order_entry_->on_frame(record.owner, record.payload_bytes(), record.now) !=
                    protocol::DecodeStatus::Ok) {
                    ++stats_.malformed_frames;
                }
                break;
            case RecordType::CancelAll:
                requests_seen_ = true;
                ++stats_.cancel_alls;
                order_entry_->cancel_all(record.owner, record.now);
                break;
            case RecordType::Stop:
                ++stats_.stops;
                break;
        }
        applied(record);
        return ReplayStatus::Ok;
    }

    [[nodiscard]] bool started() const noexcept { return order_entry_ != nullptr; }
    [[nodiscard]] ReplayStatus status() const noexcept { return status_; }
    [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }
    // The timestamp of the record being applied, for sinks that want it.
    [[nodiscard]] Timestamp now() const noexcept { return now_; }

    // The rebuilt order entry layer. Throws std::logic_error before SessionStart.
    [[nodiscard]] protocol::OrderEntry<Book>& order_entry() {
        if (!order_entry_) {
            throw std::logic_error("the journal has not started a session yet");
        }
        return *order_entry_;
    }

private:
    ReplayStatus fail(ReplayStatus status) noexcept {
        status_ = status;
        return status;
    }

    void applied(const JournalRecord& record) noexcept {
        ++stats_.records;
        stats_.last_sequence = record.sequence;
    }

    ReplayStatus start(const JournalRecord& record) {
        if (order_entry_) {
            return fail(ReplayStatus::SecondSessionStart);
        }
        const auto config = session_config_of(record);
        if (!config) {
            return fail(ReplayStatus::ConfigRefused);
        }
        try {
            order_entry_ = std::make_unique<protocol::OrderEntry<Book>>(*replies_, *config);
        } catch (const std::invalid_argument&) {
            return fail(ReplayStatus::ConfigRefused);
        }
        order_entry_->set_market_data_sink(market_data_);
        ++stats_.setup_records;
        applied(record);
        return ReplayStatus::Ok;
    }

    ReplayStatus add_symbol(const JournalRecord& record) {
        if (requests_seen_) {
            return fail(ReplayStatus::SetupAfterRequests);
        }
        const auto listing = symbol_listing_of(record);
        if (!listing) {
            return fail(ReplayStatus::SymbolRefused);
        }
        try {
            // The engine numbers stocks in the order they are listed, so the same records
            // give the same ids. A different id means the journal does not fit this
            // engine, and every later frame would trade the wrong stock.
            if (order_entry_->add_symbol(listing->ticker, listing->config) != listing->id) {
                return fail(ReplayStatus::SymbolRefused);
            }
        } catch (const std::invalid_argument&) {
            return fail(ReplayStatus::SymbolRefused);
        }
        ++stats_.setup_records;
        applied(record);
        return ReplayStatus::Ok;
    }

    ReplayStatus set_risk_limits(const JournalRecord& record) {
        if (requests_seen_) {
            return fail(ReplayStatus::SetupAfterRequests);
        }
        const auto limits = risk_limits_of(record);
        if (!limits) {
            return fail(ReplayStatus::RiskLimitsRefused);
        }
        try {
            order_entry_->set_risk_limits(record.owner, *limits);
        } catch (const std::out_of_range&) {
            return fail(ReplayStatus::RiskLimitsRefused);
        }
        ++stats_.setup_records;
        applied(record);
        return ReplayStatus::Ok;
    }

    protocol::OutboundSink* replies_;
    protocol::MarketDataSink* market_data_;
    std::unique_ptr<protocol::OrderEntry<Book>> order_entry_;
    ReplayStats stats_;
    Timestamp now_ = 0;
    ReplayStatus status_ = ReplayStatus::Ok;
    bool requests_seen_ = false;
};

// What replay_journal did.
struct ReplayOutcome {
    JournalScan scan;
    ReplayStatus status = ReplayStatus::Ok;
};

// Scans a whole journal and applies every good record to `replayer`. A torn tail is
// ignored, since those records never fully reached the file. A corrupt journal or a bad
// header is refused before anything is applied.
template <OrderBook Book>
ReplayOutcome replay_journal(std::span<const std::byte> file, Replayer<Book>& replayer) {
    ReplayOutcome outcome;
    outcome.scan = scan_journal(file);
    if (outcome.scan.tail == TailStatus::BadHeader) {
        outcome.status = ReplayStatus::JournalUnreadable;
        return outcome;
    }
    if (outcome.scan.tail == TailStatus::Corrupt) {
        outcome.status = ReplayStatus::JournalCorrupt;
        return outcome;
    }
    const std::span<const std::byte> records =
        file.subspan(kFileHeaderLength, outcome.scan.valid_length - kFileHeaderLength);
    RecordReader reader(records);
    JournalRecord record;
    while (reader.next(record)) {
        outcome.status = replayer.apply(record);
        if (outcome.status != ReplayStatus::Ok) {
            return outcome;
        }
    }
    return outcome;
}

}  // namespace litbook::journal
