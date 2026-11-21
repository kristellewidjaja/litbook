// litbook_gateway_latency measures the round trip through the TCP gateway on loopback.
//
//   litbook_gateway_latency [ROUND_TRIPS_PER_PASS] [blocking|spin]
//                           [--pipeline inline|spsc|mutex]
//                           [--pin CLIENT GATEWAY MATCHING PUBLISHER]
//                           [--window N] [--sample-every N]
//
// With --pipeline inline (the default) the gateway runs on one thread, as in Phase 6, and
// the session layer calls the order entry layer directly. With spsc or mutex it is the
// Phase 7 pipeline (ThreadedGateway), where the gateway thread hands each order to a
// matching thread through a queue of that kind and a publisher thread takes the market
// events.
//
// In blocking mode every thread of the server sleeps when it has nothing to do. The
// gateway sleeps in epoll_wait, which is how litbook_gateway runs, and the pipeline
// threads sleep on their doorbells. In spin mode every server thread polls without
// sleeping, which burns a core each but never waits for the kernel to wake it.
//
// Latency (the default). One client sends an Enter Order and waits for Order Accepted,
// then sends a Cancel Order and waits for Order Canceled, so the book never grows. Each
// wait is one round trip, timed with TickClock from just before send() to the moment the
// whole reply has been read. A round trip includes both kernels' loopback TCP paths, the
// gateway's epoll wakeup, the session layer, order entry, the book and the client's
// wakeup. This is closed loop (one message in flight), so it measures service time, not
// response time under a load.
//
// Throughput (--window N). The client keeps up to N packets in flight and sends the same
// Enter and Cancel pairs as fast as the window lets it. A pass is timed from its first
// send to its last reply, and the result is replies per second. N is at most 512, so the
// book never holds more than 512 orders.
//
// Inside the gateway (--sample-every N, 16 by default, 0 for off). The session layer
// times one order in N from the moment it is handed to the order entry layer, or pushed
// into the request queue, until every reply to it is in the connection's output buffer
// (SessionConfig::sample_every). This leaves out both kernels and the client, so it
// shows what the pipeline itself adds. It is one histogram over the whole run, the warm
// up pass included.
//
// --pin puts the client (the main thread) and the gateway, matching and publisher threads
// on the given CPUs. The inline gateway only uses CLIENT and GATEWAY. Inside the Docker
// VM those are virtual CPUs (see thread_affinity.hpp).
//
// After one untimed warm up pass, 5 timed passes run. Each printed percentile is the
// median of that percentile over the passes, and max is the largest of any pass, the
// same method as litbook_latency (D-034).

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/concurrency/thread_affinity.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/byte_buffer.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/metrics/latency_histogram.hpp"
#include "litbook/metrics/tick_clock.hpp"
#include "litbook/pipeline/threaded_gateway.hpp"
#include "litbook/protocol/codec.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace litbook;  // NOLINT(google-build-using-namespace): a small program.
using metrics::LatencyHistogram;
using metrics::TickClock;

constexpr int kPasses = 5;
constexpr std::size_t kMaxWindow = 512;

struct Options {
    int per_pass = 20'000;
    bool spin = false;
    std::string pipeline = "inline";
    int client_cpu = -1;
    int gateway_cpu = -1;
    int matching_cpu = -1;
    int publisher_cpu = -1;
    std::size_t window = 0;  // 0 means the latency run.
    std::uint32_t sample_every = 16;
};

// The configuration both kinds of server use.
gateway::SessionConfig session_config(const Options& options) {
    gateway::SessionConfig config{.max_connections = 4};
    config.sample_every = options.sample_every;
    return config;
}

protocol::OrderEntryConfig order_entry_config() {
    return protocol::OrderEntryConfig{.max_owners = 1, .max_live_orders = 1024, .max_symbols = 1};
}

const gateway::AccountConfig kAccount{.username = "bench", .password = "bench"};
const BookConfig kBook{.tick_size = 100, .max_orders = 1024};

// What the run reports about the server once it has stopped.
struct ServerReport {
    LatencyHistogram inside;  // SessionManager::request_latency().
    std::uint32_t live_orders = 0;
    std::uint64_t messages_out = 0;
    std::uint64_t messages_dropped = 0;
    std::uint64_t backend_busy = 0;
    bool pipeline = false;
    std::uint64_t reply_waits = 0;
    std::uint64_t event_waits = 0;
    std::uint64_t pump_drains = 0;
    std::uint64_t level_updates = 0;
};

// The single-threaded gateway of Phase 6, run on a thread of its own.
class InlineServer {
public:
    using Manager = gateway::SessionManager<SortedOrderBook>;
    using Gateway = gateway::EpollGateway<SortedOrderBook>;

    InlineServer(const Options& options, Ticker symbol)
        : sessions_(session_config(options), order_entry_config()),
          gateway_(sessions_, gateway::GatewayConfig{}),
          options_(options) {
        sessions_.add_account(kAccount);
        sessions_.add_symbol(symbol, kBook);
    }

    [[nodiscard]] std::uint16_t port() const { return gateway_.port(); }

    void start() {
        thread_ = std::thread([this] {
            if (options_.gateway_cpu >= 0) {
                pin_result_ = concurrency::pin_current_thread(options_.gateway_cpu);
            }
            if (options_.spin) {
                gateway_.run(0);
            } else {
                gateway_.run();
            }
        });
    }

    ServerReport stop() {
        gateway_.stop();
        thread_.join();
        if (pin_result_ != 0) {
            std::fprintf(stderr, "warning: the gateway thread could not be pinned\n");
        }
        ServerReport report;
        report.inside = sessions_.request_latency();
        report.live_orders = sessions_.order_entry().live_orders();
        report.messages_out = sessions_.stats().messages_out;
        report.messages_dropped = sessions_.stats().messages_dropped;
        report.backend_busy = sessions_.stats().backend_busy;
        return report;
    }

private:
    Manager sessions_;
    Gateway gateway_;
    Options options_;
    std::thread thread_;
    int pin_result_ = 0;  // Written by the gateway thread, read after the join.
};

// The three-thread pipeline of Phase 7.
template <template <concurrency::QueueElement> class Queue>
class PipelineServer {
public:
    using Exchange = pipeline::ThreadedGateway<SortedOrderBook, Queue>;

    PipelineServer(const Options& options, Ticker symbol)
        : exchange_(session_config(options), order_entry_config(), gateway::GatewayConfig{},
                    pipeline_config(options)) {
        exchange_.add_account(kAccount);
        exchange_.add_symbol(symbol, kBook);
    }

    [[nodiscard]] std::uint16_t port() const { return exchange_.port(); }

    void start() { exchange_.start(); }

    ServerReport stop() {
        exchange_.stop();
        constexpr std::array<pipeline::PipelineThread, 3> kThreads{
            pipeline::PipelineThread::Gateway, pipeline::PipelineThread::Matching,
            pipeline::PipelineThread::Publisher};
        for (const pipeline::PipelineThread thread : kThreads) {
            if (exchange_.pin_result(thread) != 0) {
                std::fprintf(stderr, "warning: pipeline thread %d could not be pinned\n",
                             static_cast<int>(thread));
            }
        }
        const auto& sessions = exchange_.sessions();
        ServerReport report;
        report.inside = sessions.request_latency();
        report.live_orders = exchange_.matching().order_entry().live_orders();
        report.messages_out = sessions.stats().messages_out;
        report.messages_dropped = sessions.stats().messages_dropped;
        report.backend_busy = sessions.stats().backend_busy;
        report.pipeline = true;
        report.reply_waits = exchange_.matching().stats().reply_waits;
        report.event_waits = exchange_.matching().stats().event_waits;
        report.pump_drains = exchange_.pump().stats().drains;
        report.level_updates = exchange_.publisher().stats().level_updates;
        return report;
    }

private:
    static pipeline::PipelineConfig pipeline_config(const Options& options) {
        pipeline::PipelineConfig config;
        config.wait = options.spin ? pipeline::WaitMode::Spin : pipeline::WaitMode::Block;
        config.gateway_cpu = options.gateway_cpu;
        config.matching_cpu = options.matching_cpu;
        config.publisher_cpu = options.publisher_cpu;
        return config;
    }

    Exchange exchange_;
};

// A blocking client that sends packets and reads reply packets one at a time.
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

    // Blocks until one whole packet other than a heartbeat has arrived and returns its
    // type. A heartbeat can come in whenever the server sent nothing for a second.
    gateway::soup::PacketType wait_packet() {
        for (;;) {
            const auto p =
                gateway::soup::next_packet(in_.readable(), gateway::soup::kMaxLengthField);
            if (p.status == gateway::soup::PacketStatus::Complete) {
                in_.consume(p.size);
                const auto type = static_cast<gateway::soup::PacketType>(static_cast<char>(p.type));
                if (type == gateway::soup::PacketType::ServerHeartbeat) {
                    continue;
                }
                return type;
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

    void expect_reply(const char* what) {
        if (wait_packet() != gateway::soup::PacketType::SequencedData) {
            throw std::runtime_error(what);
        }
    }

private:
    gateway::Fd socket_;
    gateway::ByteBuffer in_;
};

void append_packet(std::vector<std::byte>& out, const protocol::InboundMessage& m) {
    std::array<std::byte, protocol::kMaxMessageLength> frame{};
    const std::size_t n = protocol::encode(m, frame);
    const std::size_t at = out.size();
    out.resize(at + gateway::soup::kHeaderSize + n);
    (void)gateway::soup::encode_packet(gateway::soup::PacketType::UnsequencedData,
                                       std::span<const std::byte>(frame.data(), n),
                                       std::span<std::byte>(out).subspan(at));
}

// Every packet of one pass, one after the other, and where each one starts. Building
// them before the pass keeps the encoding out of the timed loop.
struct PassPackets {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> starts;  // One more entry than there are packets.

    [[nodiscard]] std::size_t count() const { return starts.size() - 1; }
    [[nodiscard]] std::span<const std::byte> range(std::size_t first, std::size_t end) const {
        return std::span<const std::byte>(bytes).subspan(starts[first],
                                                         starts[end] - starts[first]);
    }
};

// n Enter and Cancel pairs. UserRefNums continue from `next_ref`.
PassPackets make_packets(int n, protocol::UserRefNum& next_ref, Ticker symbol) {
    PassPackets packets;
    packets.starts.push_back(0);
    for (int i = 0; i < n; ++i) {
        const protocol::UserRefNum ref = next_ref++;
        append_packet(packets.bytes, protocol::EnterOrder{.user_ref = ref,
                                                          .side = Side::Buy,
                                                          .qty = 100,
                                                          .symbol = symbol,
                                                          .price = 1'000'000,
                                                          .tif = TimeInForce::GoodTillCancel,
                                                          .type = OrderType::Limit,
                                                          .post_only = false});
        packets.starts.push_back(packets.bytes.size());
        append_packet(packets.bytes, protocol::CancelOrder{.user_ref = ref, .qty = 0});
        packets.starts.push_back(packets.bytes.size());
    }
    return packets;
}

struct LatencyPass {
    LatencyHistogram enter;
    LatencyHistogram cancel;
};

// The packets alternate Enter and Cancel, and each one waits for its reply.
LatencyPass latency_pass(Client& client, const PassPackets& packets) {
    LatencyPass pass;
    for (std::size_t i = 0; i < packets.count(); i += 2) {
        std::uint64_t start = TickClock::now();
        client.send(packets.range(i, i + 1));
        client.expect_reply("expected Order Accepted");
        pass.enter.record(TickClock::now() - start);
        start = TickClock::now();
        client.send(packets.range(i + 1, i + 2));
        client.expect_reply("expected Order Canceled");
        pass.cancel.record(TickClock::now() - start);
    }
    return pass;
}

// Returns millions of replies per second. Every packet gets exactly one reply, so the
// packets in flight are those sent minus the replies read.
double throughput_pass(Client& client, const PassPackets& packets, std::size_t window) {
    const std::size_t total = packets.count();
    std::size_t sent = 0;
    std::size_t received = 0;
    const std::uint64_t start = TickClock::now();
    while (received < total) {
        const std::size_t room = window - (sent - received);
        const std::size_t end = std::min(total, sent + room);
        if (end > sent) {
            client.send(packets.range(sent, end));
            sent = end;
        }
        client.expect_reply("expected a reply");
        ++received;
    }
    const auto ns = static_cast<double>(TickClock::to_ns(TickClock::now() - start));
    return static_cast<double>(total) / ns * 1e3;
}

// The median over passes of one percentile, in microseconds. The values are in ticks.
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

// The inside histogram is already in nanoseconds and is one run, not a set of passes.
void print_inside_row(const LatencyHistogram& h) {
    const auto us = [&h](double percent) {
        return static_cast<double>(h.percentile(percent)) / 1000.0;
    };
    std::printf("%-22s %8.2f %8.2f %8.2f %8.2f %8.2f %10.2f   (%llu samples)\n",
                "inside the gateway", us(50), us(90), us(99), us(99.9), us(99.99),
                static_cast<double>(h.max()) / 1000.0, static_cast<unsigned long long>(h.count()));
}

void log_in(Client& client) {
    std::array<std::byte, gateway::soup::kHeaderSize + gateway::soup::kLoginRequestPayload> login{};
    const auto request = gateway::soup::make_login_request("bench", "bench", "", 0);
    if (!request) {
        throw std::runtime_error("bad login request");
    }
    (void)gateway::soup::encode_login_request(*request, login);
    client.send(login);
    if (client.wait_packet() != gateway::soup::PacketType::LoginAccepted) {
        throw std::runtime_error("login failed");
    }
}

template <class Server>
void run(const Options& options, Ticker symbol) {
    Server server(options, symbol);
    server.start();
    // The client is made after start(), so the gateway thread is the one that accepts it.
    Client client(server.port());
    log_in(client);

    protocol::UserRefNum next_ref = 1;
    std::vector<LatencyPass> latency;
    std::vector<double> rates;
    for (int i = 0; i <= kPasses; ++i) {
        const PassPackets packets = make_packets(options.per_pass, next_ref, symbol);
        if (options.window == 0) {
            const LatencyPass pass = latency_pass(client, packets);
            if (i > 0) {  // Pass 0 is the warm up.
                latency.push_back(pass);
            }
        } else {
            const double rate = throughput_pass(client, packets, options.window);
            if (i > 0) {
                rates.push_back(rate);
            }
        }
    }
    const ServerReport report = server.stop();

    std::printf("gateway %s, %s mode, %d %s per pass, %d passes, timer tick %.2f ns\n",
                options.pipeline.c_str(), options.spin ? "spin" : "blocking", options.per_pass,
                options.window == 0 ? "round trips" : "order pairs", kPasses, TickClock::tick_ns());
    if (options.client_cpu >= 0) {
        std::printf("pinned: client %d, gateway %d, matching %d, publisher %d\n",
                    options.client_cpu, options.gateway_cpu, options.matching_cpu,
                    options.publisher_cpu);
    }
    std::printf("%-22s %8s %8s %8s %8s %8s %10s   (microseconds)\n",
                options.window == 0 ? "round trip" : "", "p50", "p90", "p99", "p99.9", "p99.99",
                "max");
    if (options.window == 0) {
        std::vector<const LatencyHistogram*> enter;
        std::vector<const LatencyHistogram*> cancel;
        for (const LatencyPass& p : latency) {
            enter.push_back(&p.enter);
            cancel.push_back(&p.cancel);
        }
        print_row("enter -> accepted", enter);
        print_row("cancel -> canceled", cancel);
    }
    if (options.sample_every != 0) {
        print_inside_row(report.inside);
    }
    if (options.window != 0) {
        std::vector<double> sorted = rates;
        std::ranges::sort(sorted);
        std::printf("throughput, window %zu: %.3f M replies/s median, %.3f min, %.3f max\n",
                    options.window, sorted[sorted.size() / 2], sorted.front(), sorted.back());
    }
    // A healthy run leaves no orders behind and dropped no replies.
    std::printf("health: live orders %u, replies %llu, dropped replies %llu, backend busy %llu\n",
                report.live_orders, static_cast<unsigned long long>(report.messages_out),
                static_cast<unsigned long long>(report.messages_dropped),
                static_cast<unsigned long long>(report.backend_busy));
    if (report.pipeline) {
        std::printf(
            "pipeline: reply queue waits %llu, event queue waits %llu, reply drains %llu, "
            "level updates published %llu\n",
            static_cast<unsigned long long>(report.reply_waits),
            static_cast<unsigned long long>(report.event_waits),
            static_cast<unsigned long long>(report.pump_drains),
            static_cast<unsigned long long>(report.level_updates));
    }
}

// Reads a whole number that must be at least `low`. Returns nothing for anything else.
std::optional<long long> parse_number(std::string_view text, long long low) {
    if (text.empty()) {
        return std::nullopt;
    }
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9' || value > 1'000'000'000LL) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    if (value < low) {
        return std::nullopt;
    }
    return value;
}

// Returns nothing if the arguments are wrong.
std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    int i = 1;
    if (i < argc && !std::string_view(argv[i]).starts_with("--")) {
        const auto n = parse_number(argv[i], 1);
        if (!n) {
            return std::nullopt;
        }
        options.per_pass = static_cast<int>(*n);
        ++i;
    }
    if (i < argc && !std::string_view(argv[i]).starts_with("--")) {
        const std::string_view mode = argv[i];
        if (mode != "blocking" && mode != "spin") {
            return std::nullopt;
        }
        options.spin = mode == "spin";
        ++i;
    }
    while (i < argc) {
        const std::string_view flag = argv[i];
        const int left = argc - i - 1;  // Arguments after the flag.
        if (flag == "--pipeline" && left >= 1) {
            options.pipeline = argv[i + 1];
            if (options.pipeline != "inline" && options.pipeline != "spsc" &&
                options.pipeline != "mutex") {
                return std::nullopt;
            }
            i += 2;
        } else if (flag == "--pin" && left >= 4) {
            std::array<int, 4> cpus{};
            for (std::size_t k = 0; k < cpus.size(); ++k) {
                const auto cpu = parse_number(argv[i + 1 + static_cast<int>(k)], 0);
                if (!cpu) {
                    return std::nullopt;
                }
                cpus[k] = static_cast<int>(*cpu);
            }
            options.client_cpu = cpus[0];
            options.gateway_cpu = cpus[1];
            options.matching_cpu = cpus[2];
            options.publisher_cpu = cpus[3];
            i += 5;
        } else if (flag == "--window" && left >= 1) {
            const auto n = parse_number(argv[i + 1], 1);
            if (!n || std::cmp_greater(*n, kMaxWindow)) {
                return std::nullopt;
            }
            options.window = static_cast<std::size_t>(*n);
            i += 2;
        } else if (flag == "--sample-every" && left >= 1) {
            const auto n = parse_number(argv[i + 1], 0);
            if (!n) {
                return std::nullopt;
            }
            options.sample_every = static_cast<std::uint32_t>(*n);
            i += 2;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

// Every CPU named by --pin must be one this process may use.
bool pins_allowed(const Options& options) {
    if (options.client_cpu < 0) {
        return true;
    }
    const std::vector<int> allowed = concurrency::allowed_cpus();
    for (const int cpu :
         {options.client_cpu, options.gateway_cpu, options.matching_cpu, options.publisher_cpu}) {
        if (std::ranges::find(allowed, cpu) == allowed.end()) {
            std::fprintf(stderr, "litbook_gateway_latency: CPU %d is not available\n", cpu);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) try {
    const std::optional<Options> parsed = parse_options(argc, argv);
    if (!parsed) {
        std::fprintf(stderr,
                     "usage: litbook_gateway_latency [ROUND_TRIPS_PER_PASS] [blocking|spin]\n"
                     "       [--pipeline inline|spsc|mutex] "
                     "[--pin CLIENT GATEWAY MATCHING PUBLISHER]\n"
                     "       [--window N (1 to %zu)] [--sample-every N]\n",
                     kMaxWindow);
        return 2;
    }
    const Options& options = *parsed;
    if (!pins_allowed(options)) {
        return 2;
    }
    if (options.client_cpu >= 0 && concurrency::pin_current_thread(options.client_cpu) != 0) {
        std::fprintf(stderr, "warning: the client thread could not be pinned\n");
    }
    const auto symbol = make_ticker("AAPL");
    if (!symbol) {
        return 1;
    }
    if (options.pipeline == "spsc") {
        run<PipelineServer<concurrency::SpscQueue>>(options, *symbol);
    } else if (options.pipeline == "mutex") {
        run<PipelineServer<concurrency::MutexQueue>>(options, *symbol);
    } else {
        run<InlineServer>(options, *symbol);
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_gateway_latency: %s\n", e.what());
    return 1;
}
