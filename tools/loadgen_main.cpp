// litbook_loadgen puts a load of many client sessions on a running litbook_gateway and
// reports what happened and how long the replies took (include/litbook/loadgen/).
//
//   litbook_loadgen --port N [--address A] [--sessions N | --account USER:PASSWORD...]
//                   [--threads T] [--rate R | --rates R1,R2,...]
//                   [--warmup-ms M] [--pass-ms M] [--passes P] [--max-in-flight N]
//                   [--live-target N] [--max-live N] [--mix CANCEL,REPLACE,AGGRESSIVE]
//                   [--symbol TICKER] [--middle PRICE] [--seed N] [--first-ref N]
//                   [--pin CPU,CPU,...] [--per-session on|off]
//
// --sessions N logs in as L00001 to L0000N with the password "load", the accounts that
// litbook_gateway --load-accounts N makes. --account names the accounts one by one
// instead. Every account gets one session, and the sessions are shared out over --threads
// client threads (default 1).
//
// --rate is the number of requests per second of all sessions together (default 10,000),
// on an open loop schedule, so requests are sent when they are due whether or not earlier
// ones were answered (load_run.hpp). --rates runs one test per rate, one after the other,
// on fresh connections, which shows how latency grows with the load. After a warm up of
// --warmup-ms (default 1000) there are --passes timed passes (default 5) of --pass-ms each
// (default 1000). A session never has more than --max-in-flight requests waiting (default
// 64), and a request that would go over waits, which shows up as send lag.
//
// Each session trades --symbol (default AAPL) around --middle (default 1000000, which is
// $100.00 in the protocol's 4 implied decimals). It keeps about --live-target orders
// resting (default 100) and never more than --max-live live orders (default 1000). --mix
// gives the chance of a cancel, a replace and an aggressive IOC order in percent (default
// 30,10,10), and the rest are passive orders. --seed (default 1) fixes the requests. The
// first UserRefNum is --first-ref (default 1). The exchange ignores numbers an account
// already used, so a second run against the same gateway needs a higher one, which the
// last line of a run prints.
//
// --pin puts client thread i on the i-th CPU. --per-session on adds a line per session.
//
// The exit code is 0 when every run was healthy, 1 when one was not or something failed,
// and 2 for bad arguments.

#include "litbook/engine/ticker.hpp"
#include "litbook/loadgen/load_report.hpp"
#include "litbook/loadgen/load_run.hpp"
#include "litbook/loadgen/pacer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using litbook::loadgen::Credentials;
using litbook::loadgen::LoadConfig;

struct Options {
    LoadConfig load;
    std::vector<std::uint64_t> rates;
    std::uint64_t sessions = 0;
    bool per_session = false;
};

constexpr std::uint64_t kNanosPerMilli = 1'000'000;

// Parses an unsigned number no larger than max, or returns nothing.
std::optional<std::uint64_t> parse_unsigned(std::string_view text, std::uint64_t max) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > max) {
            return std::nullopt;
        }
    }
    return value;
}

// Parses "A,B,C" into numbers no larger than max.
std::optional<std::vector<std::uint64_t>> parse_list(std::string_view text, std::uint64_t max) {
    std::vector<std::uint64_t> values;
    for (;;) {
        const auto comma = text.find(',');
        const auto value = parse_unsigned(text.substr(0, comma), max);
        if (!value) {
            return std::nullopt;
        }
        values.push_back(*value);
        if (comma == std::string_view::npos) {
            return values;
        }
        text.remove_prefix(comma + 1);
    }
}

bool parse_account(std::string_view text, std::vector<Credentials>& out) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    out.push_back(Credentials{.username = std::string(text.substr(0, colon)),
                              .password = std::string(text.substr(colon + 1))});
    return true;
}

// Handles one flag and its value. Returns false for anything it does not accept.
bool parse_flag(std::string_view flag, std::string_view value, Options& o) {
    LoadConfig& c = o.load;
    constexpr std::uint64_t kBig = 1'000'000'000;
    std::optional<std::uint64_t> n;
    if (flag == "--account") {
        return parse_account(value, c.accounts);
    }
    if (flag == "--address") {
        c.address = std::string(value);
        return !value.empty();
    }
    if (flag == "--rates" || flag == "--rate") {
        auto list = parse_list(value, litbook::loadgen::kMaxRate);
        if (!list || (flag == "--rate" && list->size() != 1) ||
            std::ranges::find(*list, 0) != list->end()) {
            return false;
        }
        o.rates = std::move(*list);
        return true;
    }
    if (flag == "--mix") {
        const auto list = parse_list(value, 100);
        if (!list || list->size() != 3) {
            return false;
        }
        c.flow.cancel_percent = static_cast<std::uint32_t>((*list)[0]);
        c.flow.replace_percent = static_cast<std::uint32_t>((*list)[1]);
        c.flow.aggressive_percent = static_cast<std::uint32_t>((*list)[2]);
        return true;
    }
    if (flag == "--pin") {
        const auto list = parse_list(value, 4096);
        if (!list) {
            return false;
        }
        c.cpus.assign(list->begin(), list->end());
        return true;
    }
    if (flag == "--symbol") {
        const auto ticker = litbook::make_ticker(value);
        if (!ticker) {
            return false;
        }
        c.flow.symbol = *ticker;
        return true;
    }
    if (flag == "--per-session") {
        o.per_session = value == "on";
        return value == "on" || value == "off";
    }
    if (flag == "--port" && (n = parse_unsigned(value, 65'535)) && *n > 0) {
        c.port = static_cast<std::uint16_t>(*n);
    } else if (flag == "--sessions" && (n = parse_unsigned(value, 10'000)) && *n > 0) {
        o.sessions = *n;
    } else if (flag == "--threads" && (n = parse_unsigned(value, 256)) && *n > 0) {
        c.threads = static_cast<std::uint32_t>(*n);
    } else if (flag == "--warmup-ms" && (n = parse_unsigned(value, kBig))) {
        c.warmup_ns = *n * kNanosPerMilli;
    } else if (flag == "--pass-ms" && (n = parse_unsigned(value, kBig)) && *n > 0) {
        c.pass_ns = *n * kNanosPerMilli;
    } else if (flag == "--passes" && (n = parse_unsigned(value, 1000)) && *n > 0) {
        c.passes = static_cast<std::uint32_t>(*n);
    } else if (flag == "--max-in-flight" && (n = parse_unsigned(value, 65'536)) && *n > 0) {
        c.max_in_flight = static_cast<std::uint32_t>(*n);
    } else if (flag == "--live-target" && (n = parse_unsigned(value, 1'000'000))) {
        c.flow.live_target = static_cast<std::uint32_t>(*n);
    } else if (flag == "--max-live" && (n = parse_unsigned(value, 1'000'000)) && *n > 0) {
        c.max_live = static_cast<std::uint32_t>(*n);
    } else if (flag == "--middle" &&
               (n = parse_unsigned(value, litbook::protocol::kMaxWirePrice))) {
        c.flow.middle = static_cast<litbook::Price>(*n);
    } else if (flag == "--seed" && (n = parse_unsigned(value, ~std::uint64_t{0} / 2))) {
        c.flow.seed = *n;
    } else if (flag == "--first-ref" && (n = parse_unsigned(value, 0xFFFF'0000ULL)) && *n > 0) {
        c.flow.first_ref = static_cast<litbook::protocol::UserRefNum>(*n);
    } else {
        return false;
    }
    return true;
}

bool parse_options(int argc, char** argv, Options& o) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() % 2 != 0) {
        return false;  // Every flag takes a value.
    }
    for (std::size_t i = 0; i < args.size(); i += 2) {
        if (!parse_flag(args[i], args[i + 1], o)) {
            return false;
        }
    }
    if (o.load.port == 0) {
        return false;
    }
    // Either --sessions or --account, not both, and --sessions 1 by default.
    if (o.sessions > 0 && !o.load.accounts.empty()) {
        return false;
    }
    if (o.load.accounts.empty()) {
        for (std::uint64_t i = 1; i <= std::max<std::uint64_t>(o.sessions, 1); ++i) {
            std::array<char, 8> name{};
            (void)std::snprintf(name.data(), name.size(), "L%05u", static_cast<unsigned>(i));
            o.load.accounts.push_back(Credentials{.username = name.data(), .password = "load"});
        }
    }
    if (o.rates.empty()) {
        o.rates.push_back(o.load.rate);
    }
    return o.load.flow.live_target <= o.load.max_live;
}

}  // namespace

int main(int argc, char** argv) try {
    Options o;
    if (!parse_options(argc, argv, o)) {
        std::fprintf(stderr,
                     "usage: litbook_loadgen --port N [--address A] "
                     "[--sessions N | --account USER:PASSWORD...] [--threads T] "
                     "[--rate R | --rates R1,R2,...] [--warmup-ms M] [--pass-ms M] [--passes P] "
                     "[--max-in-flight N] [--live-target N] [--max-live N (>= live target)] "
                     "[--mix CANCEL,REPLACE,AGGRESSIVE] [--symbol TICKER] [--middle PRICE] "
                     "[--seed N] [--first-ref N] [--pin CPU,...] [--per-session on|off]\n");
        return 2;
    }
    bool healthy = true;
    for (const std::uint64_t rate : o.rates) {
        o.load.rate = rate;
        const litbook::loadgen::LoadResult result = litbook::loadgen::run_load(o.load);
        litbook::loadgen::print_report(stdout, o.load, result);
        if (o.per_session) {
            litbook::loadgen::print_sessions(stdout, result);
        }
        healthy = healthy && litbook::loadgen::health_problems(result).empty();
        // The next run must use UserRefNums above every one this run used.
        litbook::protocol::UserRefNum next = o.load.flow.first_ref;
        for (const auto& s : result.sessions) {
            next = std::max(next, s.next_ref);
        }
        o.load.flow.first_ref = next;
        std::printf("next first ref: %u\n", next);
        std::fflush(stdout);
    }
    return healthy ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_loadgen: %s\n", e.what());
    return 1;
}
