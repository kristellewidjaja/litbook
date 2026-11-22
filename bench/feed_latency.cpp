// litbook_feed_latency measures how long an order takes to show up on the market data feed.
//
//   litbook_feed_latency [ORDERS_PER_PASS] [blocking|spin]
//                        [--pin CLIENT GATEWAY MATCHING PUBLISHER]
//
// The exchange is the three-thread pipeline with SpscQueues and the real feed (FeedWorker
// on the publisher thread, UDP multicast on lo). One client sends orders on TCP and also
// runs a FeedReceiver on the same thread, the way a trading firm watches its own orders
// appear in the public data.
//
// The client sends an Enter Order for 100 shares at a price where nothing else rests, and
// starts the timer just before send(). The timer stops when the client's FeedHandler book
// shows that level with 100 shares. Then it sends a Cancel Order and times until the
// level is gone from the feed. After each timed wait the client also reads the TCP reply,
// which is not timed. So one sample covers the client's TCP send, the gateway thread, the
// request queue, the matching thread and the book, the event queue, the publisher thread,
// one multicast send and receive through the kernel, and the client's decode.
//
// In spin mode every thread polls, the client included (poll with timeout 0). In blocking
// mode every server thread sleeps when idle, and the client sleeps in poll() on the two
// feed sockets. The snapshot stream sends a snapshot every millisecond in both modes, as
// litbook_gateway does, so a blocking client is also woken by snapshot packets.
//
// After one untimed warm up pass, 5 timed passes run. Each printed percentile is the
// median of that percentile over the passes, and max is the largest of any pass (D-034).
// The health line says whether the feed ever lost a packet. A run with gaps is not a clean
// measurement, because recovery from a snapshot then took part in some samples.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/concurrency/thread_affinity.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/feed/feed_receiver.hpp"
#include "litbook/feed/udp_feed_sink.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/metrics/latency_histogram.hpp"
#include "litbook/metrics/tick_clock.hpp"
#include "litbook/pipeline/feed_worker.hpp"
#include "litbook/pipeline/threaded_gateway.hpp"
#include "litbook/protocol/messages.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "latency_support.hpp"

namespace {

using namespace litbook;         // NOLINT(google-build-using-namespace) A small program.
using namespace litbook::bench;  // NOLINT(google-build-using-namespace)

constexpr int kPasses = 5;
constexpr Price kPrice = 1'000'000;  // $100.0000 on the wire.
constexpr Qty kQty = 100;

using Exchange =
    pipeline::BasicThreadedGateway<SortedOrderBook, concurrency::SpscQueue, pipeline::FeedWorker>;

struct Options {
    int per_pass = 20'000;
    bool spin = false;
    int client_cpu = -1;
    int gateway_cpu = -1;
    int matching_cpu = -1;
    int publisher_cpu = -1;
};

const gateway::AccountConfig kAccount{.username = "bench", .password = "bench"};
const BookConfig kBook{.tick_size = 100, .max_orders = 1024};

feed::FeedReceiverConfig receiver_config() {
    feed::FeedReceiverConfig config;
    // Port 0, so runs side by side (the smoke tests) never share a port.
    config.incremental = feed::MulticastGroup{.address = "239.255.92.1", .port = 0};
    config.snapshot = feed::MulticastGroup{.address = "239.255.92.2", .port = 0};
    config.silence_limit = 0;  // The client always knows when it waits.
    config.handler = feed::FeedHandlerConfig{
        .session = "1", .max_symbols = 1, .max_levels_per_side = 64, .buffer_capacity = 4096};
    return config;
}

pipeline::PipelineConfig pipeline_config(const Options& options) {
    pipeline::PipelineConfig config;
    config.wait = options.spin ? pipeline::WaitMode::Spin : pipeline::WaitMode::Block;
    config.gateway_cpu = options.gateway_cpu;
    config.matching_cpu = options.matching_cpu;
    config.publisher_cpu = options.publisher_cpu;
    return config;
}

// True when the client's copy of the book shows our level as it should be now. While the
// stock is stale after a lost packet its book is not trusted, so the answer is false.
bool feed_shows(const feed::FeedHandler& handler, bool resting) {
    if (handler.recovering() || !handler.live(0)) {
        return false;
    }
    const std::optional<feed::PriceLevel> level = handler.book(0)->find(Side::Buy, kPrice);
    if (resting) {
        return level.has_value() && level->total_qty == kQty;
    }
    return !level.has_value();
}

// Reads the feed until it shows the level as wanted. Gives up after five seconds, which
// only happens if the feed is broken.
void wait_for_feed(feed::FeedReceiver& receiver, bool resting, bool spin) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!feed_shows(receiver.handler(), resting)) {
        receiver.poll_once(spin ? 0 : 100);
        if (std::chrono::steady_clock::now() > deadline) {
            throw std::runtime_error("the feed never showed the order");
        }
    }
}

struct FeedPass {
    LatencyHistogram enter;
    LatencyHistogram cancel;
};

FeedPass feed_pass(Client& client, feed::FeedReceiver& receiver, const PassPackets& packets,
                   bool spin) {
    FeedPass pass;
    for (std::size_t i = 0; i < packets.count(); i += 2) {
        std::uint64_t start = TickClock::now();
        client.send(packets.range(i, i + 1));
        wait_for_feed(receiver, true, spin);
        pass.enter.record(TickClock::now() - start);
        client.expect_reply("expected Order Accepted");

        start = TickClock::now();
        client.send(packets.range(i + 1, i + 2));
        wait_for_feed(receiver, false, spin);
        pass.cancel.record(TickClock::now() - start);
        client.expect_reply("expected Order Canceled");
    }
    return pass;
}

void run(const Options& options, Ticker symbol) {
    // The receiver first, because the feed must be sent to the ports it got.
    feed::FeedReceiver receiver(receiver_config());
    feed::FeedNetworkConfig network;
    network.incremental =
        feed::MulticastGroup{.address = "239.255.92.1", .port = receiver.incremental().port()};
    network.snapshot =
        feed::MulticastGroup{.address = "239.255.92.2", .port = receiver.snapshot().port()};
    feed::UdpFeedSink sink(network);

    pipeline::FeedWorkerOptions feed_options;
    feed_options.feed.max_symbols = 1;
    feed_options.sink = &sink;
    feed_options.max_levels_per_side = 64;
    Exchange exchange(
        gateway::SessionConfig{.max_connections = 4},
        protocol::OrderEntryConfig{.max_owners = 1, .max_live_orders = 1024, .max_symbols = 1},
        gateway::GatewayConfig{}, pipeline_config(options), feed_options);
    exchange.add_account(kAccount);
    exchange.add_symbol(symbol, kBook);
    exchange.start();

    Client client(exchange.port());
    log_in(client);

    protocol::UserRefNum next_ref = 1;
    std::vector<FeedPass> passes;
    for (int i = 0; i <= kPasses; ++i) {
        const PassPackets packets =
            make_enter_cancel_packets(options.per_pass, next_ref, symbol, kPrice, kQty);
        const FeedPass pass = feed_pass(client, receiver, packets, options.spin);
        if (i > 0) {  // Pass 0 is the warm up.
            passes.push_back(pass);
        }
    }
    exchange.stop();
    // Read what is left, End of Session included.
    while (receiver.poll_once(10) > 0) {
    }

    std::printf(
        "feed latency, spsc pipeline, %s mode, %d orders per pass, %d passes, "
        "timer tick %.2f ns\n",
        options.spin ? "spin" : "blocking", options.per_pass, kPasses, TickClock::tick_ns());
    if (options.client_cpu >= 0) {
        std::printf("pinned: client %d, gateway %d, matching %d, publisher %d\n",
                    options.client_cpu, options.gateway_cpu, options.matching_cpu,
                    options.publisher_cpu);
    }
    std::printf("%-22s %8s %8s %8s %8s %8s %10s   (microseconds)\n", "order to feed", "p50", "p90",
                "p99", "p99.9", "p99.99", "max");
    std::vector<const LatencyHistogram*> enter;
    std::vector<const LatencyHistogram*> cancel;
    for (const FeedPass& p : passes) {
        enter.push_back(&p.enter);
        cancel.push_back(&p.cancel);
    }
    print_row("enter -> level shown", enter);
    print_row("cancel -> level gone", cancel);

    const feed::FeedHandlerStats& h = receiver.handler().stats();
    const feed::SenderStats& sent = sink.sender(feed::FeedStream::Incremental).stats();
    const auto& published = exchange.publisher().feed().stats();
    std::printf(
        "health: live orders %u, level updates applied %llu, gaps %llu, recoveries %llu, "
        "dropped by sender %llu, ended %s\n",
        exchange.matching().order_entry().live_orders(), static_cast<unsigned long long>(h.applied),
        static_cast<unsigned long long>(h.gaps), static_cast<unsigned long long>(h.recoveries),
        static_cast<unsigned long long>(sent.dropped), receiver.handler().ended() ? "yes" : "no");
    std::printf("feed: incremental packets %llu, snapshots %llu, worker sleeps %llu\n",
                static_cast<unsigned long long>(published.packets[0]),
                static_cast<unsigned long long>(published.snapshots),
                static_cast<unsigned long long>(exchange.publisher().stats().sleeps));
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
        if (flag == "--pin" && argc - i - 1 >= 4) {
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
        } else {
            return std::nullopt;
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) try {
    const std::optional<Options> parsed = parse_options(argc, argv);
    if (!parsed) {
        std::fprintf(stderr,
                     "usage: litbook_feed_latency [ORDERS_PER_PASS] [blocking|spin] "
                     "[--pin CLIENT GATEWAY MATCHING PUBLISHER]\n");
        return 2;
    }
    const Options& options = *parsed;
    if (options.client_cpu >= 0 && concurrency::pin_current_thread(options.client_cpu) != 0) {
        std::fprintf(stderr, "warning: the client thread could not be pinned\n");
    }
    const auto symbol = make_ticker("AAPL");
    if (!symbol) {
        return 1;
    }
    run(options, *symbol);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_feed_latency: %s\n", e.what());
    return 1;
}
