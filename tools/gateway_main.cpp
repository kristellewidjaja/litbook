// litbook_gateway runs the exchange as a TCP server that clients reach with SoupBinTCP
// and OUCH-style messages (docs/PROTOCOL.md).
//
//   litbook_gateway [--port N] [--account USER:PASSWORD[:cod]]... [--symbol TICKER]...
//                   [--max-order-qty N] [--max-open-orders N]
//
// --port 0 lets the kernel pick a free port. The first line printed is always
// "litbook_gateway listening on 127.0.0.1:PORT", so scripts and tests can find the port.
// ":cod" after an account turns on cancel on disconnect for it. Without --account there is
// one account, test/test. Without --symbol the only stock is AAPL. Every stock uses a $0.01
// tick, a lot size of 1, and room for 65,536 resting orders.
//
// SIGINT (Ctrl-C) or SIGTERM stops the server. It then ends the session, so clients get
// End of Session, prints its counters and exits with 0. Bad arguments exit with 2.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Gateway = litbook::gateway::EpollGateway<litbook::SortedOrderBook>;
using Manager = litbook::gateway::SessionManager<litbook::SortedOrderBook>;

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
};

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
        } else {
            return false;
        }
    }
    if (o.accounts.empty()) {
        o.accounts.push_back(Account{.username = "test", .password = "test"});
    }
    if (o.symbols.empty()) {
        o.symbols.emplace_back("AAPL");
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) try {
    Options o;
    if (!parse_options(argc, argv, o)) {
        std::fprintf(stderr,
                     "usage: litbook_gateway [--port N] [--account USER:PASSWORD[:cod]]... "
                     "[--symbol TICKER]... [--max-order-qty N] [--max-open-orders N]\n");
        return 2;
    }

    Manager sessions(litbook::gateway::SessionConfig{},
                     {.max_owners = static_cast<litbook::OwnerId>(o.accounts.size()),
                      .max_live_orders = 1U << 20U,
                      .max_symbols = o.symbols.size()});
    for (const Account& a : o.accounts) {
        sessions.add_account({.username = a.username,
                              .password = a.password,
                              .cancel_on_disconnect = a.cancel_on_disconnect,
                              .limits = o.limits});
    }
    for (const std::string& name : o.symbols) {
        const auto ticker = litbook::make_ticker(name);
        if (!ticker) {
            std::fprintf(stderr, "not a valid ticker: %s\n", name.c_str());
            return 2;
        }
        sessions.add_symbol(*ticker,
                            litbook::BookConfig{.tick_size = 100, .max_orders = 1U << 16U});
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

    const auto& stats = sessions.stats();
    std::printf(
        "stopped: logins %llu, rejected logins %llu, packets in %llu, messages out %llu, "
        "dropped %llu, live orders %u\n",
        static_cast<unsigned long long>(stats.logins_accepted),
        static_cast<unsigned long long>(stats.logins_rejected),
        static_cast<unsigned long long>(stats.packets_in),
        static_cast<unsigned long long>(stats.messages_out),
        static_cast<unsigned long long>(stats.messages_dropped),
        sessions.order_entry().live_orders());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_gateway: %s\n", e.what());
    return 1;
}
