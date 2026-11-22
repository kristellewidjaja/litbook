// Microbenchmarks of the market data feed without the threads around it.
//
// The events come from a real run. The random order stream that the order entry benchmark
// uses (order_entry_stream, two stocks, four accounts) goes through OrderEntry with a
// SortedOrderBook once, before any timing, and every trade and level update it causes is
// recorded together with which order caused it.
//
// BM_FeedPublisherEvents hands those events to a FeedPublisher and flushes after the last
// event of each order, which is what FeedWorker does when its queue runs empty. The sink
// throws the packets away, so this is the cost of encoding, packing and the publisher's
// copy of each book.
//
// BM_FeedPublisherEventsUdp is the same with a real UdpFeedSink on lo, so the difference
// between the two is the send() system call of each packet.
//
// BM_FeedHandlerIncremental feeds the incremental packets of that run, recorded once, to
// a FeedHandler, which decodes them, checks the sequence numbers and updates its books.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/feed/feed_handler.hpp"
#include "litbook/feed/feed_publisher.hpp"
#include "litbook/feed/udp_feed_sink.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "book/test_limits.hpp"
#include "protocol/order_entry_stream.hpp"
#include "support/unwrap.hpp"

namespace litbook::feed {
namespace {

using pipeline::MarketEvent;
using pipeline::MarketEventKind;

constexpr Price kTickSize = 100;
constexpr int kFrames = 100'000;

// One recorded event, and whether it was the last one caused by its order.
struct RecordedEvent {
    MarketEvent event;
    bool last_of_order = false;
};

// Turns the order entry layer's market data calls into MarketEvents, as MatchingWorker
// does, and keeps them.
class EventRecorder final : public protocol::MarketDataSink {
public:
    void on_trade(SymbolId symbol, const Trade& trade) override {
        MarketEvent e;
        e.now = now;
        e.kind = MarketEventKind::Trade;
        e.side = trade.aggressor_side;
        e.symbol = symbol;
        e.price = trade.price;
        e.qty = trade.qty;
        e.match_id = trade.match_id;
        events.push_back(RecordedEvent{.event = e, .last_of_order = false});
    }

    void on_level_update(SymbolId symbol, const LevelUpdate& update) override {
        MarketEvent e;
        e.now = now;
        e.kind = MarketEventKind::LevelUpdate;
        e.side = update.side;
        e.symbol = symbol;
        e.order_count = update.order_count;
        e.price = update.price;
        e.qty = update.total_qty;
        events.push_back(RecordedEvent{.event = e, .last_of_order = false});
    }

    Timestamp now = 0;
    std::vector<RecordedEvent> events;
};

// The replies are not needed, only the market data.
class DiscardReplies final : public protocol::OutboundSink {
public:
    void send(OwnerId /*owner*/, std::span<const std::byte> /*frame*/) override {}
};

// Every event of the order stream, made once and shared by all benchmarks.
const std::vector<RecordedEvent>& recorded_events() {
    static const std::vector<RecordedEvent> events = [] {
        DiscardReplies replies;
        EventRecorder recorder;
        protocol::OrderEntry<SortedOrderBook> entry(
            replies,
            protocol::OrderEntryConfig{.max_owners = 4, .max_live_orders = 4096, .max_symbols = 2});
        entry.set_market_data_sink(&recorder);
        for (const char* name : testing::kStreamStocks) {
            entry.add_symbol(testing::unwrap(make_ticker(name)),
                             testing::with_test_limits(BookConfig{.tick_size = kTickSize}));
        }
        for (const testing::InboundFrame& frame : testing::order_entry_stream(7, kFrames)) {
            ++recorder.now;
            const std::size_t before = recorder.events.size();
            (void)entry.on_frame(frame.owner, frame.bytes, recorder.now);
            if (recorder.events.size() > before) {
                recorder.events.back().last_of_order = true;
            }
        }
        return recorder.events;
    }();
    return events;
}

// Counts what it is given and keeps nothing.
class CountingFeedSink final : public FeedSink {
public:
    void send(FeedStream /*stream*/, std::span<const std::byte> packet) override {
        ++packets;
        bytes += packet.size();
    }
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
};

// Keeps the incremental packets.
class IncrementalRecorder final : public FeedSink {
public:
    void send(FeedStream stream, std::span<const std::byte> packet) override {
        if (stream == FeedStream::Incremental) {
            packets.emplace_back(packet.begin(), packet.end());
        }
    }
    std::vector<std::vector<std::byte>> packets;
};

std::unique_ptr<FeedPublisher> make_publisher(FeedSink& sink) {
    auto publisher = std::make_unique<FeedPublisher>(sink, FeedPublisherConfig{});
    for (const char* name : testing::kStreamStocks) {
        publisher->add_symbol(FeedSymbol{.symbol = testing::unwrap(make_ticker(name)),
                                         .tick_size = kTickSize,
                                         .lot_size = 1,
                                         .max_levels_per_side = 1024});
    }
    publisher->start(0);
    return publisher;
}

// Publishes every recorded event, flushing after the last event of each order.
void publish_all(FeedPublisher& publisher, const std::vector<RecordedEvent>& events) {
    for (const RecordedEvent& r : events) {
        publisher.on_event(r.event);
        if (r.last_of_order) {
            publisher.flush(r.event.now);
        }
    }
    publisher.flush(events.back().event.now);
}

void report_per_event(benchmark::State& state, std::size_t events,
                      std::uint64_t incremental_packets) {
    const auto count = static_cast<double>(events);
    state.counters["time/event"] = benchmark::Counter(
        count, benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
    state.counters["events/packet"] =
        benchmark::Counter(count / static_cast<double>(incremental_packets));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(events));
}

void BM_FeedPublisherEvents(benchmark::State& state) {
    const std::vector<RecordedEvent>& events = recorded_events();
    CountingFeedSink sink;
    std::unique_ptr<FeedPublisher> publisher;
    std::uint64_t packets = 0;
    for (auto _ : state) {
        state.PauseTiming();
        publisher.reset();
        publisher = make_publisher(sink);
        state.ResumeTiming();
        publish_all(*publisher, events);
        packets = publisher->stats().packets[0];
    }
    benchmark::DoNotOptimize(sink.bytes);
    report_per_event(state, events.size(), packets);
}

void BM_FeedPublisherEventsUdp(benchmark::State& state) {
    const std::vector<RecordedEvent>& events = recorded_events();
    // Groups nobody joins, so the packets go through the send path and are then dropped.
    FeedNetworkConfig network;
    network.incremental = MulticastGroup{.address = "239.255.90.1", .port = 15190};
    network.snapshot = MulticastGroup{.address = "239.255.90.2", .port = 15191};
    UdpFeedSink sink(network);
    std::unique_ptr<FeedPublisher> publisher;
    std::uint64_t packets = 0;
    for (auto _ : state) {
        state.PauseTiming();
        publisher.reset();
        publisher = make_publisher(sink);
        state.ResumeTiming();
        publish_all(*publisher, events);
        packets = publisher->stats().packets[0];
    }
    const SenderStats& sent = sink.sender(FeedStream::Incremental).stats();
    state.counters["dropped"] = benchmark::Counter(static_cast<double>(sent.dropped));
    state.counters["failed"] = benchmark::Counter(static_cast<double>(sent.failed));
    report_per_event(state, events.size(), packets);
}

void BM_FeedHandlerIncremental(benchmark::State& state) {
    IncrementalRecorder recorder;
    std::uint64_t messages = 0;
    {
        const std::unique_ptr<FeedPublisher> publisher = make_publisher(recorder);
        publish_all(*publisher, recorded_events());
        messages = publisher->stats().messages[0];
    }
    const FeedHandlerConfig config{.session = "1", .max_symbols = 2};
    std::unique_ptr<FeedHandler> handler;
    std::uint64_t applied = 0;
    for (auto _ : state) {
        state.PauseTiming();
        handler.reset();
        handler = std::make_unique<FeedHandler>(config);
        state.ResumeTiming();
        for (const std::vector<std::byte>& packet : recorder.packets) {
            benchmark::DoNotOptimize(handler->on_incremental(packet));
        }
        applied = handler->stats().applied;
    }
    const auto count = static_cast<double>(messages);
    state.counters["time/message"] = benchmark::Counter(
        count, benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
    state.counters["time/packet"] = benchmark::Counter(
        static_cast<double>(recorder.packets.size()),
        benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
    state.counters["msgs/packet"] =
        benchmark::Counter(count / static_cast<double>(recorder.packets.size()));
    // A check that the run was real. Every level update must have reached a book.
    state.counters["applied"] = benchmark::Counter(static_cast<double>(applied));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(messages));
}

BENCHMARK(BM_FeedPublisherEvents)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_FeedPublisherEventsUdp)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_FeedHandlerIncremental)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace litbook::feed
