#pragma once

#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/protocol/order_entry_config.hpp"

#include <span>
#include <string>

namespace litbook::journal {

// The setup records of an exchange (Session Start, Add Symbol, Risk Limits) as decoded
// records. The threaded gateway builds one for each of its startup calls. A new journal
// gets them written at its start, and recovery compares them with the setup records at
// the start of an existing journal, so an exchange is never rebuilt from a journal that
// was written with another configuration. The timestamp and sequence number of these
// records are 0, because only the type, owner and payload are compared.

// Each throws std::invalid_argument for a value the journal cannot store.
[[nodiscard]] JournalRecord session_start_record(const protocol::OrderEntryConfig& config);
[[nodiscard]] JournalRecord add_symbol_record(const SymbolListing& listing);
[[nodiscard]] JournalRecord risk_limits_record(OwnerId owner, const protocol::RiskLimits& limits);

// True for Session Start, Add Symbol and Risk Limits.
[[nodiscard]] bool is_setup_record(RecordType type) noexcept;

// Appends one setup record to a writer with the time `now`. A record that is not a setup
// record is Refused.
JournalStatus append_setup_record(JournalWriter& writer, Timestamp now,
                                  const JournalRecord& record);

// Compares the setup records found in a journal with the expected ones, by type, owner
// and payload. Returns an empty string if they agree, and otherwise a sentence that names
// the first difference, for an operator to read.
[[nodiscard]] std::string setup_difference(std::span<const JournalRecord> journal,
                                           std::span<const JournalRecord> expected);

// One line that says what a setup record holds, such as "Add Symbol AAPL (id 0, tick 100,
// lot 1, ...)". Other records are named by their type.
[[nodiscard]] std::string describe_record(const JournalRecord& record);

}  // namespace litbook::journal
