// Benchmarks of the order entry protocol. The codec benchmarks time one decode or encode of
// a single message, which is the cost added in front of the book for every message. The
// order entry benchmark sends a whole stream of inbound frames from four accounts through
// OrderEntry (decode, session rules, the book, and encoding every reply), so its time per
// frame is the full cost of one client message inside the exchange, without the network.
//
// The sink only counts bytes (a virtual call per outbound message, as in real use). The
// stream is generated before timing starts, and each iteration builds a fresh OrderEntry
// with the timer paused, so every iteration replays the same stream from an empty book.
//
// Run with the release preset only. The logged results are in docs/BENCHMARKS.md.

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "book/test_limits.hpp"
#include "protocol/order_entry_stream.hpp"
#include "support/unwrap.hpp"

namespace litbook::protocol {
namespace {

std::vector<std::byte> encoded(const InboundMessage& m) {
    std::vector<std::byte> out(kMaxMessageLength);
    out.resize(encode(m, out));
    return out;
}

const EnterOrder kEnter{.user_ref = 7,
                        .side = Side::Buy,
                        .qty = 300,
                        .symbol = testing::unwrap(make_ticker("AAPL")),
                        .price = 1'234'500,
                        .tif = TimeInForce::GoodTillCancel,
                        .type = OrderType::Limit,
                        .post_only = false};

void BM_DecodeEnterOrder(benchmark::State& state) {
    const std::vector<std::byte> frame = encoded(kEnter);
    for (auto _ : state) {
        benchmark::DoNotOptimize(frame.data());
        auto decoded = decode_inbound(frame);
        benchmark::DoNotOptimize(decoded);
    }
}

void BM_DecodeCancelOrder(benchmark::State& state) {
    const std::vector<std::byte> frame = encoded(CancelOrder{.user_ref = 7, .qty = 0});
    for (auto _ : state) {
        benchmark::DoNotOptimize(frame.data());
        auto decoded = decode_inbound(frame);
        benchmark::DoNotOptimize(decoded);
    }
}

void BM_EncodeExecuted(benchmark::State& state) {
    ExecutedMessage m{.timestamp = 1,
                      .user_ref = 7,
                      .qty = 100,
                      .price = 1'234'500,
                      .liquidity = Liquidity::Removed,
                      .match = 42};
    std::array<std::byte, kMaxMessageLength> buffer{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(m);
        std::size_t n = encode(m, buffer);
        benchmark::DoNotOptimize(n);
        benchmark::ClobberMemory();
    }
}

void BM_EncodeAccepted(benchmark::State& state) {
    AcceptedMessage m{.timestamp = 1,
                      .user_ref = 7,
                      .side = Side::Buy,
                      .qty = 300,
                      .symbol = testing::unwrap(make_ticker("AAPL")),
                      .price = 1'234'500,
                      .tif = TimeInForce::GoodTillCancel,
                      .type = OrderType::Limit,
                      .post_only = false,
                      .order_ref = 99};
    std::array<std::byte, kMaxMessageLength> buffer{};
    for (auto _ : state) {
        benchmark::DoNotOptimize(m);
        std::size_t n = encode(m, buffer);
        benchmark::DoNotOptimize(n);
        benchmark::ClobberMemory();
    }
}

// Decodes a mix of real stream frames one after another, so the branch on the message
// type cannot be predicted perfectly as it can in the single-message benchmarks.
void BM_DecodeMixedStream(benchmark::State& state) {
    const auto stream = testing::order_entry_stream(11, 4096);
    std::size_t i = 0;
    for (auto _ : state) {
        auto decoded = decode_inbound(stream[i].bytes);
        benchmark::DoNotOptimize(decoded);
        i = (i + 1) % stream.size();
    }
}

class CountingSink final : public OutboundSink {
public:
    void send(OwnerId /*owner*/, std::span<const std::byte> frame) override {
        ++frames;
        bytes += frame.size();
    }
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
};

template <class Book>
void BM_OrderEntryStream(benchmark::State& state) {
    const auto stream = testing::order_entry_stream(7, 100'000);
    CountingSink sink;
    std::unique_ptr<OrderEntry<Book>> entry;
    for (auto _ : state) {
        state.PauseTiming();
        entry.reset();
        entry = std::make_unique<OrderEntry<Book>>(
            sink, OrderEntryConfig{.max_owners = 4, .max_live_orders = 4096, .max_symbols = 2});
        for (const char* name : testing::kStreamStocks) {
            entry->add_symbol(testing::unwrap(make_ticker(name)),
                              testing::with_test_limits(BookConfig{.tick_size = 100}));
        }
        state.ResumeTiming();
        Timestamp now = 0;
        for (const testing::InboundFrame& frame : stream) {
            benchmark::DoNotOptimize(entry->on_frame(frame.owner, frame.bytes, ++now));
        }
    }
    benchmark::DoNotOptimize(sink.bytes);
    const auto frames = static_cast<double>(stream.size());
    state.counters["time/frame"] = benchmark::Counter(
        frames, benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
    state.counters["replies/frame"] = benchmark::Counter(
        static_cast<double>(sink.frames) / (frames * static_cast<double>(state.iterations())));
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(stream.size()));
}

BENCHMARK(BM_DecodeEnterOrder);
BENCHMARK(BM_DecodeCancelOrder);
BENCHMARK(BM_EncodeExecuted);
BENCHMARK(BM_EncodeAccepted);
BENCHMARK(BM_DecodeMixedStream);
BENCHMARK_TEMPLATE(BM_OrderEntryStream, MapOrderBook)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_OrderEntryStream, FastOrderBook)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_OrderEntryStream, SortedOrderBook)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace litbook::protocol
