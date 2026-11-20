// litbook_gateway_latency measures the round trip through the TCP gateway on loopback.
//
//   litbook_gateway_latency [ROUND_TRIPS_PER_PASS] [blocking|spin]
//
// The gateway runs on its own thread. In blocking mode it sleeps in epoll_wait until a
// socket is ready, which is how litbook_gateway runs. In spin mode it calls
// epoll_wait with a zero timeout in a loop, which burns a core but never sleeps.
//
// One client sends an Enter Order and waits for Order Accepted, then sends a Cancel Order
// and waits for Order Canceled, so the book never grows. Each wait is one round trip,
// timed with TickClock from just before send() to the moment the whole reply has been
// read. A round trip includes both kernels' loopback TCP paths, the gateway's epoll
// wakeup, the session layer, order entry, the book and the client's wakeup.
//
// After one untimed warm up pass, 5 timed passes run. Each printed percentile is the
// median of that percentile over the passes, and max is the largest of any pass, the
// same method as litbook_latency (D-034). This is closed loop (one message in flight), so
// it measures service time, not response time under a load.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/byte_buffer.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/metrics/latency_histogram.hpp"
#include "litbook/metrics/tick_clock.hpp"
#include "litbook/protocol/codec.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace litbook;  // NOLINT(google-build-using-namespace): a small program.
using metrics::LatencyHistogram;
using metrics::TickClock;
using Gateway = gateway::EpollGateway<SortedOrderBook>;
using Manager = gateway::SessionManager<SortedOrderBook>;

constexpr int kPasses = 5;

// A blocking client that sends one packet and waits for one reply packet.
class Client {
public:
    explicit Client(std::uint16_t port)
        : socket_(gateway::connect_tcp("127.0.0.1", port)), in_(4096) {}

    void send(std::span<const std::byte> bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n =
                ::send(socket_.get(), bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n <= 0) {
                throw std::runtime_error("send failed");
            }
            done += static_cast<std::size_t>(n);
        }
    }

    // Blocks until one whole packet has arrived and returns its type.
    gateway::soup::PacketType wait_packet() {
        for (;;) {
            const auto p =
                gateway::soup::next_packet(in_.readable(), gateway::soup::kMaxLengthField);
            if (p.status == gateway::soup::PacketStatus::Complete) {
                in_.consume(p.size);
                return static_cast<gateway::soup::PacketType>(static_cast<char>(p.type));
            }
            if (!in_.make_room(1)) {
                throw std::runtime_error("client buffer full");
            }
            const auto space = in_.writable();
            const ssize_t n = ::recv(socket_.get(), space.data(), space.size(), 0);
            if (n <= 0) {
                throw std::runtime_error("connection closed");
            }
            in_.commit(static_cast<std::size_t>(n));
        }
    }

private:
    gateway::Fd socket_;
    gateway::ByteBuffer in_;
};

std::vector<std::byte> packet_for(const protocol::InboundMessage& m) {
    std::array<std::byte, protocol::kMaxMessageLength> frame{};
    const std::size_t n = protocol::encode(m, frame);
    std::vector<std::byte> out(gateway::soup::kHeaderSize + n);
    (void)gateway::soup::encode_packet(gateway::soup::PacketType::UnsequencedData,
                                       std::span<const std::byte>(frame.data(), n), out);
    return out;
}

struct Pass {
    LatencyHistogram enter;
    LatencyHistogram cancel;
};

// Runs n Enter and Cancel round trips. UserRefNums continue from `next_ref`.
Pass run_pass(Client& client, int n, protocol::UserRefNum& next_ref, Ticker symbol) {
    Pass pass;
    for (int i = 0; i < n; ++i) {
        const protocol::UserRefNum ref = next_ref++;
        const auto enter = packet_for(protocol::EnterOrder{.user_ref = ref,
                                                           .side = Side::Buy,
                                                           .qty = 100,
                                                           .symbol = symbol,
                                                           .price = 1'000'000,
                                                           .tif = TimeInForce::GoodTillCancel,
                                                           .type = OrderType::Limit,
                                                           .post_only = false});
        const auto cancel = packet_for(protocol::CancelOrder{.user_ref = ref, .qty = 0});
        std::uint64_t start = TickClock::now();
        client.send(enter);
        if (client.wait_packet() != gateway::soup::PacketType::SequencedData) {
            throw std::runtime_error("expected Order Accepted");
        }
        pass.enter.record(TickClock::now() - start);
        start = TickClock::now();
        client.send(cancel);
        if (client.wait_packet() != gateway::soup::PacketType::SequencedData) {
            throw std::runtime_error("expected Order Canceled");
        }
        pass.cancel.record(TickClock::now() - start);
    }
    return pass;
}

// The median over passes of one percentile, in microseconds.
double median_us(const std::vector<const LatencyHistogram*>& passes, double percent) {
    std::vector<std::uint64_t> values;
    values.reserve(passes.size());
    for (const LatencyHistogram* h : passes) {
        values.push_back(h->percentile(percent));
    }
    std::ranges::sort(values);
    return static_cast<double>(TickClock::to_ns(values[values.size() / 2])) / 1000.0;
}

void print_row(std::string_view label, const std::vector<const LatencyHistogram*>& passes) {
    std::uint64_t max = 0;
    for (const LatencyHistogram* h : passes) {
        max = std::max(max, h->max());
    }
    std::printf("%-22.*s %8.2f %8.2f %8.2f %8.2f %8.2f %10.2f\n", static_cast<int>(label.size()),
                label.data(), median_us(passes, 50), median_us(passes, 90), median_us(passes, 99),
                median_us(passes, 99.9), median_us(passes, 99.99),
                static_cast<double>(TickClock::to_ns(max)) / 1000.0);
}

}  // namespace

int main(int argc, char** argv) try {
    const int per_pass = argc > 1 ? std::atoi(argv[1]) : 20'000;
    const std::string_view mode = argc > 2 ? argv[2] : "blocking";
    if (per_pass <= 0 || (mode != "blocking" && mode != "spin")) {
        std::fprintf(stderr,
                     "usage: litbook_gateway_latency [ROUND_TRIPS_PER_PASS] [blocking|spin]\n");
        return 2;
    }
    const bool spin = mode == "spin";

    Manager sessions(gateway::SessionConfig{.max_connections = 4},
                     {.max_owners = 1, .max_live_orders = 1024, .max_symbols = 1});
    sessions.add_account({.username = "bench", .password = "bench"});
    const auto symbol = make_ticker("AAPL");
    if (!symbol) {
        return 1;
    }
    sessions.add_symbol(*symbol, BookConfig{.tick_size = 100, .max_orders = 1024});
    Gateway gw(sessions, gateway::GatewayConfig{});

    std::atomic<bool> done{false};
    std::thread server([&] {
        while (!done.load(std::memory_order_acquire)) {
            if (gw.poll_once(spin ? 0 : 100) < 0) {
                return;
            }
        }
    });

    Client client(gw.port());
    std::array<std::byte, gateway::soup::kHeaderSize + gateway::soup::kLoginRequestPayload> login{};
    const auto request = gateway::soup::make_login_request("bench", "bench", "", 0);
    if (!request) {
        return 1;
    }
    (void)gateway::soup::encode_login_request(*request, login);
    client.send(login);
    if (client.wait_packet() != gateway::soup::PacketType::LoginAccepted) {
        throw std::runtime_error("login failed");
    }

    protocol::UserRefNum next_ref = 1;
    (void)run_pass(client, per_pass, next_ref, *symbol);  // Warm up.
    std::vector<Pass> passes;
    passes.reserve(kPasses);
    for (int i = 0; i < kPasses; ++i) {
        passes.push_back(run_pass(client, per_pass, next_ref, *symbol));
    }
    done.store(true, std::memory_order_release);
    gw.stop();
    server.join();

    std::vector<const LatencyHistogram*> enter;
    std::vector<const LatencyHistogram*> cancel;
    for (const Pass& p : passes) {
        enter.push_back(&p.enter);
        cancel.push_back(&p.cancel);
    }
    std::printf("gateway %s mode, %d round trips per pass, %d passes, timer tick %.2f ns\n",
                spin ? "spin" : "blocking", per_pass, kPasses, TickClock::tick_ns());
    std::printf("%-22s %8s %8s %8s %8s %8s %10s   (microseconds)\n", "round trip", "p50", "p90",
                "p99", "p99.9", "p99.99", "max");
    print_row("enter -> accepted", enter);
    print_row("cancel -> canceled", cancel);
    // A healthy run leaves no orders behind, dropped no replies and kept its connection.
    std::printf("health: live orders %u, dropped replies %llu, open connections %u\n",
                sessions.order_entry().live_orders(),
                static_cast<unsigned long long>(sessions.stats().messages_dropped),
                sessions.open_connections());
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_gateway_latency: %s\n", e.what());
    return 1;
}
