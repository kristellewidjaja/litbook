#include "litbook/loadgen/load_report.hpp"

#include <algorithm>
#include <string>

namespace litbook::loadgen {

namespace {

constexpr auto kEnter = static_cast<std::size_t>(RequestKind::Enter);
constexpr auto kCancel = static_cast<std::size_t>(RequestKind::Cancel);
constexpr auto kReplace = static_cast<std::size_t>(RequestKind::Replace);

unsigned long long ull(std::uint64_t v) {
    return static_cast<unsigned long long>(v);
}

double us(std::uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

void print_row(std::FILE* out, const char* label,
               const std::vector<metrics::LatencyHistogram>& passes) {
    std::uint64_t max = 0;
    std::uint64_t count = 0;
    for (const metrics::LatencyHistogram& h : passes) {
        max = std::max(max, h.max());
        count += h.count();
    }
    std::fprintf(out, "  %-9s %9.2f %9.2f %9.2f %9.2f %9.2f %10.2f %10llu\n", label,
                 us(median_percentile(passes, 50)), us(median_percentile(passes, 90)),
                 us(median_percentile(passes, 99)), us(median_percentile(passes, 99.9)),
                 us(median_percentile(passes, 99.99)), us(max), ull(count));
}

void print_table(std::FILE* out, const char* title, const LoadResult& result, Measure measure) {
    std::fprintf(out, "%-36s %9s %9s %9s %9s %9s %10s %10s\n", title, "p50", "p90", "p99", "p99.9",
                 "p99.99", "max", "count");
    print_row(out, "enter", per_pass(result, measure, RequestKind::Enter));
    print_row(out, "cancel", per_pass(result, measure, RequestKind::Cancel));
    print_row(out, "replace", per_pass(result, measure, RequestKind::Replace));
    print_row(out, "all", per_pass(result, measure, std::nullopt));
}

const char* to_string(SessionState s) {
    switch (s) {
        case SessionState::LoggingIn:
            return "logging in";
        case SessionState::Active:
            return "active";
        case SessionState::Ended:
            return "ended";
        case SessionState::Failed:
            return "failed";
    }
    return "unknown";
}

}  // namespace

LoadTotals totals(const LoadResult& result) {
    LoadTotals t;
    SessionCounters& sum = t.counters;
    for (const SessionResult& s : result.sessions) {
        const SessionCounters& c = s.counters;
        for (std::size_t k = 0; k < kRequestKinds; ++k) {
            sum.sent[k] += c.sent[k];
            sum.answered[k] += c.answered[k];
            sum.ignored[k] += c.ignored[k];
        }
        sum.accepted += c.accepted;
        sum.rejected += c.rejected;
        sum.rejected_open_limit += c.rejected_open_limit;
        sum.replaced += c.replaced;
        sum.canceled += c.canceled;
        sum.executions += c.executions;
        sum.executed_shares += c.executed_shares;
        sum.added += c.added;
        sum.removed += c.removed;
        sum.unsent += c.unsent;
        sum.unanswered += c.unanswered;
        sum.max_in_flight = std::max(sum.max_in_flight, c.max_in_flight);
        ++t.sessions;
        if (s.state == SessionState::Ended) {
            ++t.sessions_ended;
        }
        t.replies += s.replies;
        t.net_position += s.position;
    }
    return t;
}

std::vector<metrics::LatencyHistogram> per_pass(const LoadResult& result, Measure measure,
                                                std::optional<RequestKind> kind) {
    std::vector<metrics::LatencyHistogram> out(result.passes.size());
    for (std::size_t p = 0; p < result.passes.size(); ++p) {
        const auto& source =
            measure == Measure::Response ? result.passes[p].response : result.passes[p].service;
        if (kind) {
            out[p] = source[static_cast<std::size_t>(*kind)];
        } else {
            for (const metrics::LatencyHistogram& h : source) {
                out[p].merge(h);
            }
        }
    }
    return out;
}

std::vector<metrics::LatencyHistogram> send_lag_per_pass(const LoadResult& result) {
    std::vector<metrics::LatencyHistogram> out;
    out.reserve(result.passes.size());
    for (const PassLatency& p : result.passes) {
        out.push_back(p.send_lag);
    }
    return out;
}

std::uint64_t median_percentile(const std::vector<metrics::LatencyHistogram>& passes,
                                double percent) {
    if (passes.empty()) {
        return 0;
    }
    std::vector<std::uint64_t> values;
    values.reserve(passes.size());
    for (const metrics::LatencyHistogram& h : passes) {
        values.push_back(h.percentile(percent));
    }
    std::ranges::sort(values);
    return values[values.size() / 2];
}

double achieved_rate(const LoadResult& result, const LoadConfig& config) {
    std::uint64_t sent = 0;
    for (const PassLatency& p : result.passes) {
        sent += p.sent;
    }
    const double seconds =
        static_cast<double>(config.pass_ns) * static_cast<double>(config.passes) / 1e9;
    return seconds > 0 ? static_cast<double>(sent) / seconds : 0.0;
}

void print_report(std::FILE* out, const LoadConfig& config, const LoadResult& result) {
    const LoadTotals t = totals(result);
    const SessionCounters& c = t.counters;
    std::fprintf(out,
                 "litbook_loadgen: %llu sessions on %u threads, %llu requests/s, %.3f s warm up "
                 "then %u passes of %.3f s\n",
                 ull(t.sessions), config.threads, ull(config.rate),
                 static_cast<double>(config.warmup_ns) / 1e9, config.passes,
                 static_cast<double>(config.pass_ns) / 1e9);
    std::fprintf(out,
                 "sent:      %llu (enter %llu, cancel %llu, replace %llu), %.1f per second in the "
                 "passes, unsent %llu\n",
                 ull(c.sent[kEnter] + c.sent[kCancel] + c.sent[kReplace]), ull(c.sent[kEnter]),
                 ull(c.sent[kCancel]), ull(c.sent[kReplace]), achieved_rate(result, config),
                 ull(c.unsent));
    std::fprintf(out,
                 "replies:   %llu (accepted %llu, rejected %llu of which open limit %llu, replaced "
                 "%llu, canceled %llu, executions %llu with %llu added and %llu removed, %llu "
                 "shares)\n",
                 ull(t.replies), ull(c.accepted), ull(c.rejected), ull(c.rejected_open_limit),
                 ull(c.replaced), ull(c.canceled), ull(c.executions), ull(c.added), ull(c.removed),
                 ull(c.executed_shares));
    std::fprintf(out, "ignored:   cancels %llu, replaces %llu (their order had already filled)\n",
                 ull(c.ignored[kCancel]), ull(c.ignored[kReplace]));
    std::fprintf(out, "in flight: at most %llu per session, drain %.3f ms, net position %lld\n",
                 ull(c.max_in_flight), static_cast<double>(result.drain_ns) / 1e6,
                 static_cast<long long>(t.net_position));
    print_table(out, "response time from the due time (us)", result, Measure::Response);
    print_table(out, "service time from the send (us)", result, Measure::Service);
    std::fprintf(out, "%-36s %9s %9s %9s %9s %9s %10s %10s\n", "send lag behind the schedule (us)",
                 "p50", "p90", "p99", "p99.9", "p99.99", "max", "count");
    print_row(out, "lag", send_lag_per_pass(result));
    const std::vector<std::string> problems = health_problems(result);
    if (problems.empty()) {
        std::fprintf(out, "health: ok\n");
    } else {
        std::fprintf(out, "health: %zu problems\n", problems.size());
        for (const std::string& p : problems) {
            std::fprintf(out, "  %s\n", p.c_str());
        }
    }
}

void print_sessions(std::FILE* out, const LoadResult& result) {
    std::fprintf(out, "%-8s %-8s %10s %10s %8s %8s %10s %9s %6s %9s\n", "session", "state", "sent",
                 "answered", "ignored", "rejected", "executions", "position", "live", "inflight");
    for (const SessionResult& s : result.sessions) {
        const SessionCounters& c = s.counters;
        std::uint64_t sent = 0;
        std::uint64_t answered = 0;
        std::uint64_t ignored = 0;
        for (std::size_t k = 0; k < kRequestKinds; ++k) {
            sent += c.sent[k];
            answered += c.answered[k];
            ignored += c.ignored[k];
        }
        std::fprintf(out, "%-8s %-8s %10llu %10llu %8llu %8llu %10llu %9lld %6zu %9llu\n",
                     s.username.c_str(), to_string(s.state), ull(sent), ull(answered), ull(ignored),
                     ull(c.rejected), ull(c.executions), static_cast<long long>(s.position),
                     s.live_at_end, ull(c.max_in_flight));
    }
}

}  // namespace litbook::loadgen
