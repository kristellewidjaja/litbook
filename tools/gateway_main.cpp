// litbook_gateway runs the exchange as a TCP server that clients reach with SoupBinTCP
// and OUCH-style messages (docs/PROTOCOL.md).
//
//   litbook_gateway [--port N] [--account USER:PASSWORD[:cod]]... [--symbol TICKER]...
//                   [--max-order-qty N] [--max-open-orders N]
//                   [--feed INTERFACE] [--feed-incremental ADDRESS:PORT]
//                   [--feed-snapshot ADDRESS:PORT]
//
// --port 0 lets the kernel pick a free port. The first line printed is always
// "litbook_gateway listening on 127.0.0.1:PORT", so scripts and tests can find the port.
// ":cod" after an account turns on cancel on disconnect for it. Without --account there is
// one account, test/test. Without --symbol the only stock is AAPL. Every stock uses a $0.01
// tick, a lot size of 1, and room for 65,536 resting orders.
//
// Without --feed the whole exchange runs on one thread, as in Phase 6. With --feed it
// runs on the three-thread pipeline of Phase 7 in blocking mode, and the publisher thread
// sends the market data feed (docs/FEED.md) by UDP multicast on that interface, by default
// to 239.255.0.1:15100 (incremental) and 239.255.0.2:15101 (snapshots). The second line
// printed then says where the feed goes. litbook_feed_listener reads it.
//
// SIGINT (Ctrl-C) or SIGTERM stops the server. It then ends the session, so clients get
// End of Session (and feed listeners too), prints its counters and exits with 0. Bad
// arguments exit with 2.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/feed/multicast.hpp"
#include "litbook/feed/udp_feed_sink.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/pipeline/feed_worker.hpp"
#include "litbook/pipeline/threaded_gateway.hpp"

#include <pthread.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Gateway = litbook::gateway::EpollGateway<litbook::SortedOrderBook>;
using Manager = litbook::gateway::SessionManager<litbook::SortedOrderBook>;
using FeedExchange = litbook::pipeline::BasicThreadedGateway<
    litbook::SortedOrderBook, litbook::concurrency::SpscQueue, litbook::pipeline::FeedWorker>;

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

struct Options {
    std::uint16_t port = 15000;
    std::vector<Account> accounts;
    std::vector<std::string> symbols;
    litbook::protocol::RiskLimits limits;
    // Set by --feed. The groups have their defaults until a --feed-* flag changes them.
    std::optional<litbook::feed::FeedNetworkConfig> feed;
    std::optional<litbook::feed::MulticastGroup> feed_incremental;
    std::optional<litbook::feed::MulticastGroup> feed_snapshot;
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
        } else {
            return false;
        }
    }
    // The group flags only make sense with a feed.
    if (!o.feed && (o.feed_incremental || o.feed_snapshot)) {
        return false;
    }
    if (o.feed && o.feed_incremental) {
        o.feed->incremental = *o.feed_incremental;
    }
    if (o.feed && o.feed_snapshot) {
        o.feed->snapshot = *o.feed_snapshot;
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
    Manager sessions(litbook::gateway::SessionConfig{}, order_entry_config(o));
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

// The three-thread pipeline with the market data feed, sent where `network` says.
int run_with_feed(const Options& o, const litbook::feed::FeedNetworkConfig& network,
                  const std::vector<litbook::Ticker>& tickers) {
    litbook::feed::UdpFeedSink sink(network);
    litbook::pipeline::FeedWorkerOptions feed;
    feed.feed.max_symbols = tickers.size();
    feed.sink = &sink;

    FeedExchange exchange(
        litbook::gateway::SessionConfig{}, order_entry_config(o),
        litbook::gateway::GatewayConfig{.port = o.port},
        litbook::pipeline::PipelineConfig{.wait = litbook::pipeline::WaitMode::Block}, feed);
    for (const Account& a : o.accounts) {
        exchange.add_account(account_config(o, a));
    }
    for (const litbook::Ticker ticker : tickers) {
        exchange.add_symbol(ticker, kBookConfig);
    }

    // stop() joins threads, which is not allowed inside a signal handler. So the signals
    // are blocked here, before the threads start (they inherit the mask), and this thread
    // waits for one with sigwait and then stops the pipeline normally.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        throw std::runtime_error("pthread_sigmask failed");
    }

    exchange.start();
    std::printf("litbook_gateway listening on 127.0.0.1:%u\n",
                static_cast<unsigned>(exchange.port()));
    std::printf("litbook_gateway feed on %s: incremental %s:%u, snapshot %s:%u\n",
                network.interface_name.c_str(), network.incremental.address.c_str(),
                static_cast<unsigned>(network.incremental.port), network.snapshot.address.c_str(),
                static_cast<unsigned>(network.snapshot.port));
    std::fflush(stdout);

    int received = 0;
    sigwait(&signals, &received);
    exchange.stop();

    print_session_stats(exchange.sessions().stats(),
                        exchange.matching().order_entry().live_orders());
    const auto& incremental = sink.sender(litbook::feed::FeedStream::Incremental).stats();
    const auto& snapshot = sink.sender(litbook::feed::FeedStream::Snapshot).stats();
    const std::uint64_t dropped = incremental.dropped + snapshot.dropped;
    const std::uint64_t failed = incremental.failed + snapshot.failed;
    std::printf(
        "feed: incremental packets %llu, snapshot packets %llu, dropped %llu, failed %llu\n",
        static_cast<unsigned long long>(incremental.sent),
        static_cast<unsigned long long>(snapshot.sent), static_cast<unsigned long long>(dropped),
        static_cast<unsigned long long>(failed));
    return 0;
}

}  // namespace

int main(int argc, char** argv) try {
    Options o;
    if (!parse_options(argc, argv, o)) {
        std::fprintf(stderr,
                     "usage: litbook_gateway [--port N] [--account USER:PASSWORD[:cod]]... "
                     "[--symbol TICKER]... [--max-order-qty N] [--max-open-orders N] "
                     "[--feed INTERFACE] [--feed-incremental ADDRESS:PORT] "
                     "[--feed-snapshot ADDRESS:PORT]\n");
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
    return run_single_threaded(o, tickers);
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_gateway: %s\n", e.what());
    return 1;
}
