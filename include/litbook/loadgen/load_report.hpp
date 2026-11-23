#pragma once

// Turns a LoadResult into numbers and text. Each printed percentile is the median of that
// percentile over the timed passes, and max is the largest value of any pass, the same
// method as the other latency tools (DECISIONS.md D-034). A single pass can be hit by the
// virtual machine stopping for a moment, and the median of five passes is not.

#include "litbook/loadgen/load_run.hpp"
#include "litbook/metrics/latency_histogram.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

namespace litbook::loadgen {

// The counters of every session added up.
struct LoadTotals {
    SessionCounters counters;
    std::uint64_t sessions = 0;
    std::uint64_t sessions_ended = 0;  // Sessions that ended cleanly.
    std::uint64_t replies = 0;
    std::int64_t net_position = 0;  // Should be 0 when only these sessions trade.
};

[[nodiscard]] LoadTotals totals(const LoadResult& result);

// Which histogram of a pass a row of the report shows.
enum class Measure : std::uint8_t { Response, Service };

// One histogram per pass. kind is a timed RequestKind, or nothing for all three merged.
[[nodiscard]] std::vector<metrics::LatencyHistogram> per_pass(const LoadResult& result,
                                                              Measure measure,
                                                              std::optional<RequestKind> kind);
[[nodiscard]] std::vector<metrics::LatencyHistogram> send_lag_per_pass(const LoadResult& result);

// The median over the passes of one percentile, in nanoseconds. For an even number of
// passes it is the upper of the two middle values. 0 if there are no passes.
[[nodiscard]] std::uint64_t median_percentile(const std::vector<metrics::LatencyHistogram>& passes,
                                              double percent);

// Requests sent per second in the timed passes.
[[nodiscard]] double achieved_rate(const LoadResult& result, const LoadConfig& config);

// Prints the summary, which is what was sent, the replies, the latency tables and the
// health line.
void print_report(std::FILE* out, const LoadConfig& config, const LoadResult& result);

// One line per session with its own counters.
void print_sessions(std::FILE* out, const LoadResult& result);

}  // namespace litbook::loadgen
