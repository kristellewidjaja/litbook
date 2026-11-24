// litbook_gateway runs the exchange as a TCP server that clients reach with SoupBinTCP
// and OUCH-style messages (docs/PROTOCOL.md).
//
//   litbook_gateway [--port N] [--account USER:PASSWORD[:cod]]... [--load-accounts N]
//                   [--symbol TICKER]... [--max-order-qty N] [--max-open-orders N]
//                   [--feed INTERFACE] [--feed-incremental ADDRESS:PORT]
//                   [--feed-snapshot ADDRESS:PORT] [--journal DIRECTORY] [--sync write|sync]
//                   [--segment-records N] [--keep-snapshots K] [--snapshot-thread on|off]
//
// --port 0 lets the kernel pick a free port. The first line printed is always
// "litbook_gateway listening on 127.0.0.1:PORT", so scripts and tests can find the port.
// ":cod" after an account turns on cancel on disconnect for it. --load-accounts N adds N
// accounts for litbook_loadgen, named L00001 to L0000N (up to 10,000) with the password
// "load" and cancel on disconnect, so a load run leaves no orders behind. They come after
// the --account ones. Without any account there is one, test/test. The gateway takes 16
// more connections than there are accounts, and at least 64. Without --symbol the only
// stock is AAPL. Every stock uses a $0.01 tick, a lot size of 1, and room for 65,536
// resting orders.
//
// Without --feed the whole exchange runs on one thread, as in Phase 6. With --feed it
// runs on the three-thread pipeline of Phase 7 in blocking mode, and the publisher thread
// sends the market data feed (docs/FEED.md) by UDP multicast on that interface, by default
// to 239.255.0.1:15100 (incremental) and 239.255.0.2:15101 (snapshots). The second line
// printed then says where the feed goes and names the feed's session, which a receiver
// must be given (litbook_feed_listener --session) when it is not 1. A gateway without a
// journal always starts session 1, and every restart from a journal starts a new one.
//
// With --journal the exchange also runs on the pipeline, with a fourth thread that writes
// every request to the journal before the matching thread handles it (docs/JOURNAL.md).
// The journal is a directory of segment files and snapshot files, created if it does not
// exist. If it holds a journal, the exchange first rebuilds itself from the newest usable
// snapshot and the records after it, so a restart after a crash carries on where the
// journal ends, and every account's sequence numbers carry on too. The accounts, stocks
// and limits on the command line must be the same as when the journal was started. A line
// after the listening line (and the feed line) says what was found. --sync sync calls
// fdatasync after every batch of requests, and the default, write, survives a crash of
// the process but not of the machine.
//
// --segment-records starts a new segment file after that many records (default
// 1,000,000, and 0 keeps one segment). --keep-snapshots says how many snapshots to keep
// (default 2). A snapshot is written at every start and at a clean stop, and older
// snapshots and the segments they cover are removed. 0 turns snapshots off, and then
// nothing is ever removed. --snapshot-thread on (the default) also takes a snapshot each
// time a segment closes, on a fifth thread that keeps a second copy of the exchange. It
// needs segments and snapshots, so asking for it without them is a usage error.
//
// SIGINT (Ctrl-C) or SIGTERM stops the server. It then ends the session, so clients get
// End of Session (and feed listeners too), prints its counters and exits with 0. Bad
// arguments exit with 2. A journal that cannot be used, or that fails while the exchange
// runs, stops the server with a message and exit code 1.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/feed/multicast.hpp"
#include "litbook/feed/udp_feed_sink.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/pipeline/feed_worker.hpp"
#include "litbook/pipeline/publisher.hpp"
#include "litbook/pipeline/threaded_gateway.hpp"

#include <poll.h>
#include <pthread.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace {

using Gateway = litbook::gateway::EpollGateway<litbook::SortedOrderBook>;
using Manager = litbook::gateway::SessionManager<litbook::SortedOrderBook>;
using FeedExchange = litbook::pipeline::BasicThreadedGateway<
    litbook::SortedOrderBook, litbook::concurrency::SpscQueue, litbook::pipeline::FeedWorker>;
using StubExchange =
    litbook::pipeline::ThreadedGateway<litbook::SortedOrderBook, litbook::concurrency::SpscQueue>;

// The signal handler needs to reach the gateway, and a handler can only see globals.
Gateway* g_gateway = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

extern "C" void on_signal(int /*signal*/) {
    if (g_gateway != nullptr) {
        g_gateway->stop();  // Only an atomic store and a write(), both safe in a handler.
    }
}

struct Account {
    std::string username;
    std::string password;
    bool cancel_on_disconnect = false;
};

// The most accounts --load-accounts makes. Their names have 5 digits.
constexpr std::uint64_t kMaxLoadAccounts = 10'000;

// Load account i (from 1) is L00001 and so on, with the password "load" and cancel on
// disconnect. SoupBinTCP usernames have at most 6 characters.
Account load_account(std::uint64_t i) {
    std::array<char, 8> name{};
    (void)std::snprintf(name.data(), name.size(), "L%05u", static_cast<unsigned>(i));
    return Account{.username = name.data(), .password = "load", .cancel_on_disconnect = true};
}

struct Options {
    std::uint16_t port = 15000;
    std::vector<Account> accounts;
    std::uint64_t load_accounts = 0;
    std::vector<std::string> symbols;
    litbook::protocol::RiskLimits limits;
    // Set by --feed. The groups have their defaults until a --feed-* flag changes them.
    std::optional<litbook::feed::FeedNetworkConfig> feed;
    std::optional<litbook::feed::MulticastGroup> feed_incremental;
    std::optional<litbook::feed::MulticastGroup> feed_snapshot;
    // Set by --journal and the flags after it, which all need --journal.
    std::optional<std::string> journal;
    std::optional<litbook::journal::SyncPolicy> sync;
    std::optional<std::uint64_t> segment_records;
    std::optional<std::uint64_t> keep_snapshots;
    std::optional<bool> snapshot_thread;
};

// Parses "ADDRESS:PORT" for a feed group, or returns false.
bool parse_feed_group(std::string_view text, std::optional<litbook::feed::MulticastGroup>& out) {
    try {
        out = litbook::feed::parse_group(text);
    } catch (const std::invalid_argument&) {
        return false;
    }
    return out->port != 0;  // A sender needs a real port.
}

// Parses an unsigned number, or returns false.
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

// Splits "USER:PASSWORD" or "USER:PASSWORD:cod".
bool parse_account(std::string_view text, Account& out) {
    const auto first = text.find(':');
    if (first == std::string_view::npos) {
        return false;
    }
    out.username = std::string(text.substr(0, first));
    std::string_view rest = text.substr(first + 1);
    const auto second = rest.find(':');
    if (second != std::string_view::npos) {
        if (rest.substr(second + 1) != "cod") {
            return false;
        }
        out.cancel_on_disconnect = true;
        rest = rest.substr(0, second);
    }
    out.password = std::string(rest);
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
        if (flag == "--port" && parse_unsigned(value, 65'535, number)) {
            o.port = static_cast<std::uint16_t>(number);
        } else if (flag == "--account") {
            Account a;
            if (!parse_account(value, a)) {
                return false;
            }
            o.accounts.push_back(a);
        } else if (flag == "--load-accounts" && parse_unsigned(value, kMaxLoadAccounts, number) &&
                   number > 0) {
            o.load_accounts = number;
        } else if (flag == "--symbol") {
            o.symbols.emplace_back(value);
        } else if (flag == "--max-order-qty" && parse_unsigned(value, 0xFFFF'FFFFULL, number)) {
            o.limits.max_order_qty = static_cast<litbook::Qty>(number);
        } else if (flag == "--max-open-orders" && parse_unsigned(value, 0xFFFF'FFFFULL, number)) {
            o.limits.max_open_orders = static_cast<std::uint32_t>(number);
        } else if (flag == "--feed") {
            o.feed = litbook::feed::FeedNetworkConfig{};
            o.feed->interface_name = std::string(value);
        } else if (flag == "--feed-incremental") {
            if (!parse_feed_group(value, o.feed_incremental)) {
                return false;
            }
        } else if (flag == "--feed-snapshot") {
            if (!parse_feed_group(value, o.feed_snapshot)) {
                return false;
            }
        } else if (flag == "--journal" && !value.empty()) {
            o.journal = std::string(value);
        } else if (flag == "--sync" && value == "write") {
            o.sync = litbook::journal::SyncPolicy::Write;
        } else if (flag == "--sync" && value == "sync") {
            o.sync = litbook::journal::SyncPolicy::Sync;
        } else if (flag == "--segment-records" &&
                   parse_unsigned(value, 0xFFFF'FFFF'FFFFULL, number)) {
            o.segment_records = number;
        } else if (flag == "--keep-snapshots" && parse_unsigned(value, 1'000'000, number)) {
            o.keep_snapshots = number;
        } else if (flag == "--snapshot-thread" && value == "on") {
            o.snapshot_thread = true;
        } else if (flag == "--snapshot-thread" && value == "off") {
            o.snapshot_thread = false;
        } else {
            return false;
        }
    }
    // The group flags only make sense with a feed.
    if (!o.feed && (o.feed_incremental || o.feed_snapshot)) {
        return false;
    }
    // And the journal flags only with a journal.
    const bool journal_flags = o.sync || o.segment_records || o.keep_snapshots || o.snapshot_thread;
    if (!o.journal && journal_flags) {
        return false;
    }
    // The snapshot thread snapshots each closed segment, so it needs both.
    const bool no_segments = o.segment_records.value_or(1) == 0;
    const bool no_snapshots = o.keep_snapshots.value_or(1) == 0;
    if (o.snapshot_thread.value_or(false) && (no_segments || no_snapshots)) {
        return false;
    }
    if (o.feed && o.feed_incremental) {
        o.feed->incremental = *o.feed_incremental;
    }
    if (o.feed && o.feed_snapshot) {
        o.feed->snapshot = *o.feed_snapshot;
    }
    for (std::uint64_t i = 1; i <= o.load_accounts; ++i) {
        o.accounts.push_back(load_account(i));
    }
    if (o.accounts.empty()) {
        o.accounts.push_back(Account{.username = "test", .password = "test"});
    }
    if (o.symbols.empty()) {
        o.symbols.emplace_back("AAPL");
    }
    return true;
}

// Prints the counters every run ends with.
void print_session_stats(const litbook::gateway::SessionStats& stats, std::uint32_t live_orders) {
    std::printf(
        "stopped: logins %llu, rejected logins %llu, packets in %llu, messages out %llu, "
        "dropped %llu, live orders %u\n",
        static_cast<unsigned long long>(stats.logins_accepted),
        static_cast<unsigned long long>(stats.logins_rejected),
        static_cast<unsigned long long>(stats.packets_in),
        static_cast<unsigned long long>(stats.messages_out),
        static_cast<unsigned long long>(stats.messages_dropped), live_orders);
}

// One connection for every account, plus room for clients that have not logged in yet.
litbook::gateway::SessionConfig session_config(const Options& o) {
    litbook::gateway::SessionConfig c;
    c.max_connections = std::max<std::uint32_t>(c.max_connections,
                                                static_cast<std::uint32_t>(o.accounts.size()) + 16);
    return c;
}

litbook::protocol::OrderEntryConfig order_entry_config(const Options& o) {
    return {.max_owners = static_cast<litbook::OwnerId>(o.accounts.size()),
            .max_live_orders = 1U << 20U,
            .max_symbols = o.symbols.size()};
}

litbook::gateway::AccountConfig account_config(const Options& o, const Account& a) {
    return {.username = a.username,
            .password = a.password,
            .cancel_on_disconnect = a.cancel_on_disconnect,
            .limits = o.limits};
}

const litbook::BookConfig kBookConfig{.tick_size = 100, .max_orders = 1U << 16U};

// The Phase 6 server, everything on this thread.
int run_single_threaded(const Options& o, const std::vector<litbook::Ticker>& tickers) {
    Manager sessions(session_config(o), order_entry_config(o));
    for (const Account& a : o.accounts) {
        sessions.add_account(account_config(o, a));
    }
    for (const litbook::Ticker ticker : tickers) {
        sessions.add_symbol(ticker, kBookConfig);
    }

    Gateway gateway(sessions, litbook::gateway::GatewayConfig{.port = o.port});
    g_gateway = &gateway;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("litbook_gateway listening on 127.0.0.1:%u\n",
                static_cast<unsigned>(gateway.port()));
    std::fflush(stdout);

    gateway.run();
    gateway.end_session();
    g_gateway = nullptr;
    print_session_stats(sessions.stats(), sessions.order_entry().live_orders());
    return 0;
}

// The end of the recovery line, about the snapshot this start wrote.
void print_start_snapshot(const litbook::pipeline::RecoveryReport& r) {
    if (r.start_snapshot == 0) {
        std::printf("; snapshots off\n");
        return;
    }
    std::printf("; start snapshot as of %llu, files removed %u\n",
                static_cast<unsigned long long>(r.start_snapshot), r.files_removed);
}

// Prints what start() found in the journal, on one line.
void print_recovery(const std::string& path, const litbook::pipeline::RecoveryReport& r) {
    if (!r.recovered) {
        std::printf("litbook_gateway journal %s: new", path.c_str());
        print_start_snapshot(r);
        return;
    }
    std::printf(
        "litbook_gateway journal %s: recovered %llu records (torn bytes cut %zu), replayed "
        "%llu frames and %llu cancel alls, replies %llu, restart %llu, cancels on restart %u",
        path.c_str(), static_cast<unsigned long long>(r.records), r.torn_bytes,
        static_cast<unsigned long long>(r.frames), static_cast<unsigned long long>(r.cancel_alls),
        static_cast<unsigned long long>(r.replies),
        static_cast<unsigned long long>(r.restart_sequence), r.cancels_on_restart);
    std::printf("; segments %u, ", r.segments);
    if (r.snapshot_as_of == 0) {
        std::printf("from record 1");
    } else {
        std::printf("from snapshot as of %llu", static_cast<unsigned long long>(r.snapshot_as_of));
    }
    std::printf(", snapshots refused %u", r.snapshots_refused);
    if (r.torn_rotation) {
        std::printf(", torn rotation removed");
    }
    print_start_snapshot(r);
}

// The text for an errno value. std::strerror may share one buffer between threads, and
// this does not.
std::string error_text(int error) {
    return std::generic_category().message(error);
}

// The last line, with the journal thread's and the snapshot thread's counters.
template <class Exchange>
void print_journal_stats(const Exchange& exchange) {
    const auto* worker = exchange.journal();
    if (worker == nullptr) {
        return;
    }
    const litbook::journal::JournalWriterStats& w = worker->writer().stats();
    std::printf(
        "journal: records %llu, commits %llu, syncs %llu, next sequence %llu, rotations %llu",
        static_cast<unsigned long long>(w.records), static_cast<unsigned long long>(w.commits),
        static_cast<unsigned long long>(w.syncs),
        static_cast<unsigned long long>(worker->writer().next_sequence()),
        static_cast<unsigned long long>(w.rotations));
    if (const auto* snapshots = exchange.snapshotter()) {
        const litbook::pipeline::SnapshotWorkerStats& s = snapshots->stats();
        std::printf("; snapshot thread snapshots %llu, removed %llu",
                    static_cast<unsigned long long>(s.snapshots),
                    static_cast<unsigned long long>(s.removed));
        if (s.failures > 0) {
            std::printf(", failed (%s)", error_text(s.last_error).c_str());
        }
    }
    if (exchange.stop_snapshot_error() != 0) {
        std::printf("; stop snapshot failed (%s)",
                    error_text(exchange.stop_snapshot_error()).c_str());
    } else if (exchange.stop_snapshot() != 0) {
        std::printf("; stop snapshot as of %llu",
                    static_cast<unsigned long long>(exchange.stop_snapshot()));
    }
    std::printf("\n");
}

// Blocks SIGINT and SIGTERM in this thread, before any other thread starts (they inherit
// the mask), and returns a signalfd that becomes readable when one of them arrives. A
// signal handler cannot stop the pipeline, because stop() joins threads.
litbook::gateway::Fd block_stop_signals() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        throw std::runtime_error("pthread_sigmask failed");
    }
    litbook::gateway::Fd fd(signalfd(-1, &signals, SFD_CLOEXEC));
    if (!fd.valid()) {
        throw std::runtime_error("signalfd failed");
    }
    return fd;
}

// Waits until a stop signal arrives or the exchange shuts itself down (its journal
// failed). Returns true for a signal.
bool wait_for_stop(int signal_fd, int finished_fd) {
    std::array<pollfd, 2> fds{pollfd{.fd = signal_fd, .events = POLLIN, .revents = 0},
                              pollfd{.fd = finished_fd, .events = POLLIN, .revents = 0}};
    for (;;) {
        const int n = poll(fds.data(), fds.size(), -1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            throw std::runtime_error("poll failed");
        }
        return (fds[0].revents & POLLIN) != 0;
    }
}

// The name of the feed's session, which start() sets. Only FeedExchange has a feed, so
// every other exchange gives an empty name, and run_threaded never prints it for them.
template <class Exchange>
std::string feed_session([[maybe_unused]] const Exchange& exchange) {
    if constexpr (std::is_same_v<Exchange, FeedExchange>) {
        return exchange.publisher().feed().session();
    } else {
        return {};
    }
}

// The pipeline, with the market data feed if `feed` is set and a journal if the options
// name one. It runs in blocking mode.
template <class Exchange>
int run_threaded(const Options& o, const std::vector<litbook::Ticker>& tickers,
                 const typename Exchange::PublisherOptions& publisher_options,
                 const litbook::feed::FeedNetworkConfig* feed,
                 const litbook::feed::UdpFeedSink* sink) {
    std::optional<litbook::pipeline::JournalOptions> journal;
    if (o.journal) {
        journal.emplace();
        journal->directory = *o.journal;
        journal->writer.sync = o.sync.value_or(litbook::journal::SyncPolicy::Write);
        journal->records_per_segment = o.segment_records.value_or(journal->records_per_segment);
        journal->keep_snapshots = o.keep_snapshots.value_or(journal->keep_snapshots);
        // Without segments or snapshots the thread would have nothing to do, so the
        // default turns it off then.
        const bool possible = journal->records_per_segment > 0 && journal->keep_snapshots > 0;
        journal->snapshot_thread = o.snapshot_thread.value_or(possible);
    }
    Exchange exchange(session_config(o), order_entry_config(o),
                      litbook::gateway::GatewayConfig{.port = o.port},
                      litbook::pipeline::PipelineConfig{.wait = litbook::pipeline::WaitMode::Block},
                      publisher_options, journal ? &*journal : nullptr);
    for (const Account& a : o.accounts) {
        exchange.add_account(account_config(o, a));
    }
    for (const litbook::Ticker ticker : tickers) {
        exchange.add_symbol(ticker, kBookConfig);
    }

    const litbook::gateway::Fd signals = block_stop_signals();
    exchange.start();
    std::printf("litbook_gateway listening on 127.0.0.1:%u\n",
                static_cast<unsigned>(exchange.port()));
    if (feed != nullptr) {
        // Receivers only accept packets of the session they expect, and a restart from a
        // journal starts a new one, so the line names it. start() has already set it, and
        // the publisher thread never changes it afterwards.
        const std::string session = feed_session(exchange);
        std::printf("litbook_gateway feed on %s: incremental %s:%u, snapshot %s:%u, session %s\n",
                    feed->interface_name.c_str(), feed->incremental.address.c_str(),
                    static_cast<unsigned>(feed->incremental.port), feed->snapshot.address.c_str(),
                    static_cast<unsigned>(feed->snapshot.port), session.c_str());
    }
    if (journal) {
        print_recovery(journal->directory, exchange.recovery());
    }
    std::fflush(stdout);

    (void)wait_for_stop(signals.get(), exchange.finished_fd());
    // This rethrows a failed journal, which main reports with exit code 1.
    exchange.stop();

    print_session_stats(exchange.sessions().stats(),
                        exchange.matching().order_entry().live_orders());
    if (sink != nullptr) {
        const auto& incremental = sink->sender(litbook::feed::FeedStream::Incremental).stats();
        const auto& snapshot = sink->sender(litbook::feed::FeedStream::Snapshot).stats();
        const std::uint64_t dropped = incremental.dropped + snapshot.dropped;
        const std::uint64_t failed = incremental.failed + snapshot.failed;
        std::printf(
            "feed: incremental packets %llu, snapshot packets %llu, dropped %llu, failed %llu\n",
            static_cast<unsigned long long>(incremental.sent),
            static_cast<unsigned long long>(snapshot.sent),
            static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(failed));
    }
    print_journal_stats(exchange);
    return 0;
}

int run_with_feed(const Options& o, const litbook::feed::FeedNetworkConfig& network,
                  const std::vector<litbook::Ticker>& tickers) {
    litbook::feed::UdpFeedSink sink(network);
    litbook::pipeline::FeedWorkerOptions feed;
    feed.feed.max_symbols = tickers.size();
    feed.sink = &sink;
    return run_threaded<FeedExchange>(o, tickers, feed, &network, &sink);
}

}  // namespace

int main(int argc, char** argv) try {
    Options o;
    if (!parse_options(argc, argv, o)) {
        std::fprintf(
            stderr,
            "usage: litbook_gateway [--port N] [--account USER:PASSWORD[:cod]]... "
            "[--load-accounts N] [--symbol TICKER]... [--max-order-qty N] [--max-open-orders N] "
            "[--feed INTERFACE] [--feed-incremental ADDRESS:PORT] "
            "[--feed-snapshot ADDRESS:PORT] [--journal DIRECTORY] [--sync write|sync] "
            "[--segment-records N] [--keep-snapshots K] [--snapshot-thread on|off]\n");
        return 2;
    }
    std::vector<litbook::Ticker> tickers;
    for (const std::string& name : o.symbols) {
        const auto ticker = litbook::make_ticker(name);
        if (!ticker) {
            std::fprintf(stderr, "not a valid ticker: %s\n", name.c_str());
            return 2;
        }
        tickers.push_back(*ticker);
    }
    if (o.feed.has_value()) {
        return run_with_feed(o, *o.feed, tickers);
    }
    if (o.journal.has_value()) {
        return run_threaded<StubExchange>(o, tickers, {}, nullptr, nullptr);
    }
    return run_single_threaded(o, tickers);
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_gateway: %s\n", e.what());
    return 1;
}
