// litbook_feed_recovery measures how long a late joiner needs to get a correct copy of
// every book from the snapshot stream.
//
//   litbook_feed_recovery STOCKS [--spacing-us N] [--levels N] [--runs N] [--spin]
//
// A FeedWorker runs on its own thread, as it does on the publisher thread of the
// pipeline, and sends the feed by UDP multicast on lo. The main thread lists STOCKS
// stocks, gives each one --levels price levels on each side (default 10), and then keeps
// a trickle of level updates going (one every 100 microseconds, to a random stock), as a
// market would. Then it joins the feed --runs times (default 5), each time with a new
// FeedReceiver, and times from the moment the receiver joined until it is in step.
//
// In step means three things. The receiver has read at least one incremental packet, so
// it knows where the stream is (until then the handler has nothing to call a gap). It has
// learned the number of stocks from a snapshot. And no stock is stale, so every book was
// rebuilt from a snapshot plus the buffered updates after it.
//
// A snapshot is sent every --spacing-us microseconds (default 1,000, the feed's default),
// one stock at a time, so one cycle over every stock takes STOCKS x spacing. A joiner needs
// one whole snapshot of each stock after it joined, which takes about one cycle wherever
// in the cycle it joins. The tool prints that prediction next to what it measured.
//
// The worker blocks on its doorbell between events, as litbook_gateway runs it, unless
// --spin is given. A blocking worker sleeps in whole milliseconds, so a spacing below one
// millisecond is only kept while events wake it often enough.

#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/feed/feed_receiver.hpp"
#include "litbook/feed/udp_feed_sink.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/feed_worker.hpp"
#include "litbook/pipeline/messages.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "latency_support.hpp"

namespace {

using namespace litbook;  // NOLINT(google-build-using-namespace) A small program.
using pipeline::MarketEvent;
using pipeline::MarketEventKind;

using Events = concurrency::SpscQueue<MarketEvent>;
using Worker = pipeline::FeedWorker<Events>;

constexpr Price kTick = 100;
constexpr const char* kIncrementalGroup = "239.255.93.1";
constexpr const char* kSnapshotGroup = "239.255.93.2";
constexpr auto kTrickle = std::chrono::microseconds(100);

struct Options {
    std::size_t stocks = 0;
    std::uint64_t spacing_us = 1'000;
    std::size_t levels = 10;
    int runs = 5;
    bool spin = false;
};

// Stock names S0, S1, ... which are valid tickers.
Ticker stock_name(std::size_t i) {
    const std::optional<Ticker> t = make_ticker("S" + std::to_string(i));
    if (!t) {
        throw std::logic_error("bad stock name");
    }
    return *t;
}

MarketEvent level_event(std::size_t stock, Side side, Price price, AggregateQty qty,
                        std::uint32_t count) {
    MarketEvent e;
    e.now = gateway::nanoseconds_since_midnight();
    e.kind = MarketEventKind::LevelUpdate;
    e.side = side;
    e.symbol = static_cast<SymbolId>(stock);
    e.order_count = count;
    e.price = price;
    e.qty = qty;
    return e;
}

// The price of a stock's level `rank` on one side. Bids go down from $100, offers up from
// $100.01.
Price level_price(Side side, std::size_t rank) {
    const auto r = static_cast<Price>(rank);
    return side == Side::Buy ? (10'000 - r) * kTick : (10'001 + r) * kTick;
}

// Pushes one event and wakes the worker. The queue is large, but if it is ever full the
// main thread waits for the worker, which never waits for anyone.
void push(Events& events, pipeline::Doorbell& bell, const MarketEvent& e) {
    while (!events.try_push(e)) {
        bell.ring();
        std::this_thread::yield();
    }
    bell.ring();
}

// Sends Stop and waits for the publisher thread. A std::thread that is destroyed while it
// still runs ends the program, so every way out of run() goes through here.
void stop_publisher(Events& events, pipeline::Doorbell& bell, std::thread& publisher) {
    MarketEvent stop;
    stop.kind = MarketEventKind::Stop;
    push(events, bell, stop);
    publisher.join();
}

feed::FeedReceiverConfig receiver_config(const Options& o, std::uint16_t incremental_port,
                                         std::uint16_t snapshot_port, int receive_buffer) {
    feed::FeedReceiverConfig config;
    config.incremental =
        feed::MulticastGroup{.address = kIncrementalGroup, .port = incremental_port};
    config.snapshot = feed::MulticastGroup{.address = kSnapshotGroup, .port = snapshot_port};
    config.receive_buffer = receive_buffer;
    config.silence_limit = 0;
    config.handler = feed::FeedHandlerConfig{.session = "1",
                                             .max_symbols = o.stocks,
                                             .max_levels_per_side = o.levels + 8,
                                             .buffer_capacity = 65'536};
    return config;
}

bool in_step(const feed::FeedHandler& h, std::size_t stocks) {
    return h.next_expected() > 1 && h.symbol_count().has_value() && *h.symbol_count() == stocks &&
           !h.recovering();
}

// One late join. Returns the milliseconds from joining to being in step, and sends the
// trickle while it waits.
double join_once(const Options& o, std::uint16_t incremental_port, std::uint16_t snapshot_port,
                 Events& events, pipeline::Doorbell& bell, std::mt19937_64& rng,
                 feed::FeedHandlerStats& stats_out) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    feed::FeedReceiver receiver(receiver_config(o, incremental_port, snapshot_port, 0));
    const auto deadline = start + std::chrono::seconds(60);
    auto next_trickle = start;
    std::uniform_int_distribution<std::size_t> pick_stock(0, o.stocks - 1);
    std::uniform_int_distribution<std::size_t> pick_rank(0, o.levels - 1);
    std::uint32_t round = 0;
    while (!in_step(receiver.handler(), o.stocks)) {
        receiver.poll_once(0);
        const auto now = Clock::now();
        if (now >= next_trickle) {
            ++round;
            const Side side = (round % 2 == 0) ? Side::Buy : Side::Sell;
            push(events, bell,
                 level_event(pick_stock(rng), side, level_price(side, pick_rank(rng)),
                             100U + (round % 50U) * 100U, 1U + (round % 4U)));
            next_trickle = now + kTrickle;
        }
        if (now > deadline) {
            throw std::runtime_error("a late joiner never got in step");
        }
    }
    const auto elapsed = Clock::now() - start;
    stats_out = receiver.handler().stats();
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

void run(const Options& o) {
    // The watcher is there from the start. It picks the ports the late joiners share, and
    // shows when the feed has sent every resting level, so the first join is really late.
    feed::FeedReceiver watcher(receiver_config(o, 0, 0, 0));
    const std::uint16_t incremental_port = watcher.incremental().port();
    const std::uint16_t snapshot_port = watcher.snapshot().port();

    feed::FeedNetworkConfig network;
    network.incremental =
        feed::MulticastGroup{.address = kIncrementalGroup, .port = incremental_port};
    network.snapshot = feed::MulticastGroup{.address = kSnapshotGroup, .port = snapshot_port};
    feed::UdpFeedSink sink(network);

    Events events(1U << 16U);
    pipeline::Doorbell bell;
    pipeline::FeedWorkerOptions options;
    options.feed.max_symbols = o.stocks;
    options.feed.snapshot_spacing = o.spacing_us * 1'000;
    options.sink = &sink;
    options.max_levels_per_side = o.levels + 8;
    Worker worker(events, o.spin ? pipeline::WaitMode::Spin : pipeline::WaitMode::Block,
                  o.spin ? nullptr : &bell, &gateway::nanoseconds_since_midnight, options);
    for (std::size_t i = 0; i < o.stocks; ++i) {
        worker.add_symbol(static_cast<SymbolId>(i), stock_name(i), BookConfig{.tick_size = kTick});
    }
    std::thread publisher([&worker] { worker.run(); });

    std::mt19937_64 rng(42);
    for (std::size_t s = 0; s < o.stocks; ++s) {
        for (std::size_t rank = 0; rank < o.levels; ++rank) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                push(events, bell, level_event(s, side, level_price(side, rank), 500, 2));
            }
        }
    }

    // The incremental stream carries Start of Messages, one directory entry per stock and
    // then the setup levels. Once the watcher has seen a sequence number past all of them,
    // the setup is out. The watcher may lose packets in the burst, which does not matter,
    // because only the sequence numbers are needed.
    const auto setup_messages =
        static_cast<feed::Sequence>(1 + o.stocks + (o.stocks * o.levels * 2));
    const auto setup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (watcher.handler().next_expected() <= setup_messages) {
        watcher.poll_once(100);
        if (std::chrono::steady_clock::now() > setup_deadline) {
            stop_publisher(events, bell, publisher);
            throw std::runtime_error("the watcher did not see the setup levels");
        }
    }
    // From here on nobody reads the watcher. Its socket buffer fills up and the kernel
    // drops what does not fit, which costs the feed nothing.

    std::vector<double> times;
    feed::FeedHandlerStats last{};
    std::uint64_t gaps = 0;
    std::uint64_t applied_snapshots = 0;
    try {
        for (int r = 0; r < o.runs; ++r) {
            times.push_back(join_once(o, incremental_port, snapshot_port, events, bell, rng, last));
            gaps += last.gaps;
            applied_snapshots += last.snapshots_applied;
        }
    } catch (...) {
        stop_publisher(events, bell, publisher);
        throw;
    }
    stop_publisher(events, bell, publisher);

    const std::uint64_t expected_snapshots =
        static_cast<std::uint64_t>(o.stocks) * static_cast<std::uint64_t>(o.runs);
    std::vector<double> sorted = times;
    std::ranges::sort(sorted);
    const double predicted_ms =
        static_cast<double>(o.stocks) * static_cast<double>(o.spacing_us) / 1'000.0;
    std::printf(
        "late joiner, %zu stocks, %zu levels per side, snapshot every %llu us, %s worker, "
        "%d joins\n",
        o.stocks, o.levels, static_cast<unsigned long long>(o.spacing_us),
        o.spin ? "spinning" : "blocking", o.runs);
    std::printf("time to in step (ms): median %.2f, min %.2f, max %.2f; one cycle is %.2f\n",
                sorted[sorted.size() / 2], sorted.front(), sorted.back(), predicted_ms);
    std::printf("each join:");
    for (const double t : times) {
        std::printf(" %.2f", t);
    }
    std::printf("\n");
    const feed::SenderStats& snap = sink.sender(feed::FeedStream::Snapshot).stats();
    std::printf(
        "health: snapshots applied %llu of %llu, gaps seen %llu, last join "
        "replayed %llu, too old %llu, broken %llu, snapshot packets dropped by sender "
        "%llu\n",
        static_cast<unsigned long long>(applied_snapshots),
        static_cast<unsigned long long>(expected_snapshots), static_cast<unsigned long long>(gaps),
        static_cast<unsigned long long>(last.replayed),
        static_cast<unsigned long long>(last.snapshots_too_old),
        static_cast<unsigned long long>(last.snapshots_broken),
        static_cast<unsigned long long>(snap.dropped));
}

std::optional<Options> parse_options(int argc, char** argv) {
    if (argc < 2) {
        return std::nullopt;
    }
    Options o;
    const auto stocks = bench::parse_number(argv[1], 1);
    if (!stocks || *stocks > 4'096) {
        return std::nullopt;
    }
    o.stocks = static_cast<std::size_t>(*stocks);
    int i = 2;
    while (i < argc) {
        const std::string_view flag = argv[i];
        if (flag == "--spin") {
            o.spin = true;
            ++i;
            continue;
        }
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        const auto n = bench::parse_number(argv[i + 1], 1);
        if (!n) {
            return std::nullopt;
        }
        if (flag == "--spacing-us") {
            o.spacing_us = static_cast<std::uint64_t>(*n);
        } else if (flag == "--levels" && *n <= 1'000) {
            o.levels = static_cast<std::size_t>(*n);
        } else if (flag == "--runs" && *n <= 1'000) {
            o.runs = static_cast<int>(*n);
        } else {
            return std::nullopt;
        }
        i += 2;
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) try {
    const std::optional<Options> options = parse_options(argc, argv);
    if (!options) {
        std::fprintf(stderr,
                     "usage: litbook_feed_recovery STOCKS (1 to 4096) [--spacing-us N] "
                     "[--levels N] [--runs N] [--spin]\n");
        return 2;
    }
    run(*options);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "litbook_feed_recovery: %s\n", e.what());
    return 1;
}
