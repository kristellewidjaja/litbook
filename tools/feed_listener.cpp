// litbook_feed_listener joins the market data feed of litbook_gateway --feed and keeps a
// copy of every book, recovering from the snapshot stream when it misses packets or
// joins late (docs/FEED.md).
//
//   litbook_feed_listener [--interface NAME] [--incremental ADDRESS:PORT]
//                         [--snapshot ADDRESS:PORT] [--session NAME] [--every SECONDS]
//                         [--depth N] [--silence SECONDS]
//
// The defaults match the gateway's: interface lo, 239.255.0.1:15100 for the incremental
// stream, 239.255.0.2:15101 for snapshots, session "1". The first line printed is
// "litbook_feed_listener joined ...", so scripts and tests know it is listening. Every
// --every seconds (default 1, 0 turns it off) it prints the best --depth levels (default
// 5) of each stock it knows, and a status line. At End of Session, or on SIGINT or
// SIGTERM, it prints the books and a summary line and exits with 0. If the feed was heard
// and then nothing arrives for --silence seconds (default 3, 0 waits forever), the End of
// Session was probably lost, so it prints the books and a "silent" summary and exits with
// 3. Bad arguments exit with 2.

#include "litbook/feed/feed_receiver.hpp"
#include "litbook/feed/level_book.hpp"
#include "litbook/feed/multicast.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Set by the signal handler, read by the main loop between polls.
std::atomic<bool> g_stop{false};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

extern "C" void on_signal(int /*signal*/) {
    g_stop.store(true);
}

struct Options {
    litbook::feed::FeedReceiverConfig receiver;
    std::uint64_t every_seconds = 1;
    std::size_t depth = 5;
};

bool parse_unsigned(std::string_view text, std::uint64_t max, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > max) {
            return false;
        }
    }
    out = value;
    return true;
}

bool parse_group(std::string_view text, litbook::feed::MulticastGroup& out) {
    try {
        out = litbook::feed::parse_group(text);
    } catch (const std::invalid_argument&) {
        return false;
    }
    return true;
}

bool parse_options(int argc, char** argv, Options& o) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        if (i + 1 >= args.size()) {
            return false;  // Every flag takes a value.
        }
        const std::string_view value = args[++i];
        std::uint64_t number = 0;
        if (flag == "--interface") {
            o.receiver.interface_name = std::string(value);
        } else if (flag == "--incremental") {
            if (!parse_group(value, o.receiver.incremental)) {
                return false;
            }
        } else if (flag == "--snapshot") {
            if (!parse_group(value, o.receiver.snapshot)) {
                return false;
            }
        } else if (flag == "--session") {
            o.receiver.handler.session = std::string(value);
        } else if (flag == "--every" && parse_unsigned(value, 3'600, number)) {
            o.every_seconds = number;
        } else if (flag == "--depth" && parse_unsigned(value, 1'000, number) && number > 0) {
            o.depth = number;
        } else if (flag == "--silence" && parse_unsigned(value, 3'600, number)) {
            constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;
            o.receiver.silence_limit = number * kNanosecondsPerSecond;
        } else {
            return false;
        }
    }
    return true;
}

// A price in the feed's unit (4 implied decimals) as dollars, for example 500100 as
// "50.0100".
std::string dollars(litbook::Price price) {
    constexpr litbook::Price kScale = 10'000;
    const litbook::Price whole = price / kScale;
    const litbook::Price fraction = price % kScale;
    char text[32];  // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
    std::snprintf(text, sizeof(text), "%lld.%04lld", static_cast<long long>(whole),
                  static_cast<long long>(fraction));
    return text;
}

// One stock's best levels on both sides, one line per side.
void print_book(const litbook::feed::FeedHandler& handler, litbook::feed::Locate locate,
                std::size_t depth) {
    const auto& directory = handler.directory(locate);
    if (!directory.has_value()) {
        return;
    }
    // name() returns a new string, so it is kept as one. A string_view of it would point
    // into a temporary that is gone by the next line.
    const std::string name = directory->symbol.name();
    const litbook::feed::LevelBook& book = *handler.book(locate);
    std::printf("%s%s\n", name.c_str(), handler.live(locate) ? "" : " (stale)");
    for (const litbook::Side side : {litbook::Side::Buy, litbook::Side::Sell}) {
        std::printf("  %s", side == litbook::Side::Buy ? "bids  " : "offers");
        const std::size_t shown = std::min(depth, book.level_count(side));
        if (shown == 0) {
            std::printf(" none");
        }
        for (std::size_t rank = 0; rank < shown; ++rank) {
            const litbook::feed::PriceLevel& level = book.level(side, rank);
            std::printf("  %llu @ %s (%u)", static_cast<unsigned long long>(level.total_qty),
                        dollars(level.price).c_str(), static_cast<unsigned>(level.order_count));
        }
        std::printf("\n");
    }
}

void print_books(const litbook::feed::FeedHandler& handler, std::size_t max_symbols,
                 std::size_t depth) {
    for (std::size_t l = 0; l < max_symbols; ++l) {
        print_book(handler, static_cast<litbook::feed::Locate>(l), depth);
    }
}

void print_status(const char* label, const litbook::feed::FeedReceiver& receiver) {
    const litbook::feed::FeedHandlerStats& s = receiver.handler().stats();
    const std::uint64_t packets = s.incremental_packets + s.snapshot_packets;
    const std::uint64_t level_updates = s.applied + s.replayed;
    std::printf(
        "%s: packets %llu, level updates %llu, trades %llu, gaps %llu, recoveries %llu, "
        "trades lost %llu, recovering %s\n",
        label, static_cast<unsigned long long>(packets),
        static_cast<unsigned long long>(level_updates), static_cast<unsigned long long>(s.trades),
        static_cast<unsigned long long>(s.gaps), static_cast<unsigned long long>(s.recoveries),
        static_cast<unsigned long long>(s.trades_lost),
        receiver.handler().recovering() ? "yes" : "no");
}

}  // namespace

int main(int argc, char** argv) try {
    Options o;
    if (!parse_options(argc, argv, o)) {
        std::fprintf(stderr,
                     "usage: litbook_feed_listener [--interface NAME] "
                     "[--incremental ADDRESS:PORT] [--snapshot ADDRESS:PORT] [--session NAME] "
                     "[--every SECONDS] [--depth N] [--silence SECONDS]\n");
        return 2;
    }
    litbook::feed::FeedReceiver receiver(o.receiver);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    const auto& r = o.receiver;
    std::printf("litbook_feed_listener joined %s:%u and %s:%u on %s\n",
                r.incremental.address.c_str(), static_cast<unsigned>(receiver.incremental().port()),
                r.snapshot.address.c_str(), static_cast<unsigned>(receiver.snapshot().port()),
                r.interface_name.c_str());
    std::fflush(stdout);

    using Clock = std::chrono::steady_clock;
    const auto every = std::chrono::seconds(o.every_seconds);
    auto next_print = Clock::now() + every;
    // A signal interrupts poll, and the 100 ms timeout bounds how late the flag is seen.
    while (!g_stop.load() && !receiver.handler().ended() && !receiver.silent()) {
        receiver.poll_once(100);
        if (o.every_seconds > 0 && Clock::now() >= next_print) {
            print_books(receiver.handler(), r.handler.max_symbols, o.depth);
            print_status("status", receiver);
            std::fflush(stdout);
            next_print += every;
        }
    }
    print_books(receiver.handler(), r.handler.max_symbols, o.depth);
    if (receiver.handler().ended()) {
        print_status("ended", receiver);
        return 0;
    }
    if (receiver.silent()) {
        print_status("silent", receiver);
        return 3;
    }
    print_status("stopped", receiver);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_feed_listener: %s\n", e.what());
    return 1;
}
