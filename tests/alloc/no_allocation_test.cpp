// Proves FastOrderBook does not touch the heap after construction.
//
// This executable replaces the global operator new / delete with versions that count
// calls while "armed". A test builds the book and the operation stream, arms the
// counter, replays every standard workload (about 270,000 operations), disarms, and
// requires a count of zero. The listener counts events in plain integers, so it does
// not allocate either.
//
// It is a separate executable because replacing operator new affects the whole program.

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/listener.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/engine/engine.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "book/book_test_support.hpp"
#include "book/workload.hpp"
#include "protocol/order_entry_stream.hpp"
#include "support/unwrap.hpp"

namespace {

// These are plain globals because the tests are single-threaded and the hook must not
// allocate.
bool g_armed = false;
std::uint64_t g_allocations = 0;

void* counted_alloc(std::size_t size, std::size_t alignment) {
    if (g_armed) {
        ++g_allocations;
    }
    size = size == 0 ? 1 : size;
    // NOLINTBEGIN(cppcoreguidelines-no-malloc): this *is* the allocator
    return alignment <= alignof(std::max_align_t)
               ? std::malloc(size)
               : std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
    // NOLINTEND(cppcoreguidelines-no-malloc)
}

void* counted_alloc_or_throw(std::size_t size, std::size_t alignment) {
    void* const p = counted_alloc(size, alignment);  // NOLINT(misc-const-correctness): returned
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void counted_free(void* p) noexcept {
    std::free(p);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
}

constexpr std::size_t kDefaultAlign = alignof(std::max_align_t);

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-owning-memory): global allocation function replacements
void* operator new(std::size_t size) {
    return counted_alloc_or_throw(size, kDefaultAlign);
}
void* operator new[](std::size_t size) {
    return counted_alloc_or_throw(size, kDefaultAlign);
}
void* operator new(std::size_t size, std::align_val_t align) {
    return counted_alloc_or_throw(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return counted_alloc_or_throw(size, static_cast<std::size_t>(align));
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return counted_alloc(size, kDefaultAlign);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return counted_alloc(size, kDefaultAlign);
}
void operator delete(void* p) noexcept {
    counted_free(p);
}
void operator delete[](void* p) noexcept {
    counted_free(p);
}
void operator delete(void* p, std::size_t /*size*/) noexcept {
    counted_free(p);
}
void operator delete[](void* p, std::size_t /*size*/) noexcept {
    counted_free(p);
}
void operator delete(void* p, std::align_val_t /*align*/) noexcept {
    counted_free(p);
}
void operator delete[](void* p, std::align_val_t /*align*/) noexcept {
    counted_free(p);
}
void operator delete(void* p, std::size_t /*size*/, std::align_val_t /*align*/) noexcept {
    counted_free(p);
}
void operator delete[](void* p, std::size_t /*size*/, std::align_val_t /*align*/) noexcept {
    counted_free(p);
}
// NOLINTEND(cppcoreguidelines-owning-memory)

namespace litbook {
namespace {

// Counts heap allocations made inside its scope.
class AllocationCounter {
public:
    AllocationCounter() {
        g_allocations = 0;
        g_armed = true;
    }
    ~AllocationCounter() { g_armed = false; }
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    AllocationCounter(AllocationCounter&&) = delete;
    AllocationCounter& operator=(AllocationCounter&&) = delete;

    [[nodiscard]] std::uint64_t count() const noexcept { return g_allocations; }
};

// This listener only counts events, so it does not allocate.
class CountingListener final : public OrderBookListener {
public:
    void on_accepted(const OrderAccepted& /*event*/) override { ++events; }
    void on_rejected(const OrderRejected& /*event*/) override { ++events; }
    void on_trade(const Trade& /*event*/) override { ++trades; }
    void on_canceled(const OrderCanceled& /*event*/) override { ++events; }
    void on_cancel_rejected(const CancelRejected& /*event*/) override { ++events; }
    void on_modified(const OrderModified& /*event*/) override { ++events; }
    void on_modify_rejected(const ModifyRejected& /*event*/) override { ++events; }
    void on_level_update(const LevelUpdate& /*event*/) override { ++events; }

    std::uint64_t events = 0;
    std::uint64_t trades = 0;
};

// The compiler may remove a new/delete pair it can see through; storing the pointer in
// a volatile global keeps the allocation observable.
int* volatile g_sink = nullptr;

TEST(NoAllocationTest, TheCounterSeesAllocations) {
    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        const auto p = std::make_unique<int>(42);
        g_sink = p.get();
        count = counter.count();
    }
    EXPECT_GE(count, 1U);
}

// This is a positive control. The reference book allocates (map nodes, list nodes, hash
// nodes), so a zero for the fast book below means something.
TEST(NoAllocationTest, MapOrderBookAllocates) {
    const auto params = testing::standard_workloads().front();
    const auto ops = testing::generate(params);
    CountingListener listener;
    MapOrderBook book(listener, testing::book_config(params));
    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        for (const auto& op : ops) {
            testing::apply(book, op);
        }
        count = counter.count();
    }
    EXPECT_GT(count, 1'000U);
}

// Replays every standard workload into a `Book` and requires zero allocations.
template <class Book>
void expect_no_allocations() {
    std::uint64_t total_operations = 0;
    for (const auto& params : testing::standard_workloads()) {
        const auto ops = testing::generate(params);
        CountingListener listener;
        Book book(listener, testing::book_config(params));
        std::uint64_t count = 0;
        {
            const AllocationCounter counter;
            for (const auto& op : ops) {
                testing::apply(book, op);
            }
            count = counter.count();
        }
        EXPECT_EQ(count, 0U) << params.name;
        EXPECT_GT(listener.trades, 0U) << params.name;  // proves the book really did work
        EXPECT_TRUE(book.check_invariants()) << params.name;
        total_operations += ops.size();
    }
    ::testing::Test::RecordProperty("operations", static_cast<int>(total_operations));
}

TEST(NoAllocationTest, FastOrderBookNeverAllocatesAfterConstruction) {
    expect_no_allocations<FastOrderBook>();
}

// The sorted book inserts into and erases from std::vector, which would allocate if the
// vector ever had to grow. Its capacity is reserved in the constructor, and this proves
// it never runs out.
TEST(NoAllocationTest, SortedOrderBookNeverAllocatesAfterConstruction) {
    expect_no_allocations<SortedOrderBook>();
}

// Counts engine events without allocating, like CountingListener above.
class CountingEngineListener final : public EngineListener {
public:
    void on_accepted(SymbolId /*s*/, const OrderAccepted& /*e*/) override { ++events; }
    void on_rejected(SymbolId /*s*/, const OrderRejected& /*e*/) override { ++events; }
    void on_trade(SymbolId /*s*/, const Trade& /*e*/) override { ++trades; }
    void on_canceled(SymbolId /*s*/, const OrderCanceled& /*e*/) override { ++events; }
    void on_cancel_rejected(SymbolId /*s*/, const CancelRejected& /*e*/) override { ++events; }
    void on_modified(SymbolId /*s*/, const OrderModified& /*e*/) override { ++events; }
    void on_modify_rejected(SymbolId /*s*/, const ModifyRejected& /*e*/) override { ++events; }
    void on_level_update(SymbolId /*s*/, const LevelUpdate& /*e*/) override { ++events; }

    std::uint64_t events = 0;
    std::uint64_t trades = 0;
};

// Puts every standard workload on its own stock in one engine, then replays them with
// their operations interleaved. Routing, ticker lookup and the trade renumbering must not
// allocate either.
template <class Book>
void expect_engine_never_allocates() {
    const auto workloads = testing::standard_workloads();
    CountingEngineListener listener;
    Engine<Book> engine(listener, workloads.size());
    std::vector<std::vector<testing::Operation>> streams;
    std::vector<Ticker> tickers;
    for (const auto& params : workloads) {
        tickers.push_back(testing::unwrap(make_ticker("W" + std::to_string(streams.size()))));
        engine.add_symbol(tickers.back(), testing::book_config(params));
        streams.push_back(testing::generate(params));
    }
    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        for (std::size_t i = 0;; ++i) {
            bool any = false;
            for (std::size_t s = 0; s < streams.size(); ++s) {
                if (i >= streams[s].size()) {
                    continue;
                }
                any = true;
                const SymbolId symbol = testing::unwrap(engine.find(tickers[s]));
                const testing::Operation& op = streams[s][i];
                switch (op.kind) {
                    case testing::Operation::Kind::Submit:
                        engine.submit(symbol, op.request);
                        break;
                    case testing::Operation::Kind::Cancel:
                        engine.cancel(symbol, op.id);
                        break;
                    case testing::Operation::Kind::Modify:
                        engine.modify(symbol, op.id, op.price, op.qty);
                        break;
                }
            }
            if (!any) {
                break;
            }
        }
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_GT(listener.trades, 0U);
    EXPECT_EQ(engine.last_match_id(), listener.trades);
    EXPECT_TRUE(engine.check_invariants());
}

TEST(NoAllocationTest, EngineOverFastBooksNeverAllocates) {
    expect_engine_never_allocates<FastOrderBook>();
}

TEST(NoAllocationTest, EngineOverSortedBooksNeverAllocates) {
    expect_engine_never_allocates<SortedOrderBook>();
}

// Counts outbound frames without allocating.
class CountingSink final : public protocol::OutboundSink {
public:
    void send(OwnerId /*owner*/, std::span<const std::byte> frame) override {
        ++frames;
        bytes += frame.size();
    }
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
};

// The whole order entry path, from decoding a frame to encoding the replies, must not
// allocate once the stocks are listed.
template <class Book>
void expect_order_entry_never_allocates() {
    const auto stream = testing::order_entry_stream(7, 50'000);
    CountingSink sink;
    protocol::OrderEntry<Book> entry(sink,
                                     {.max_owners = 4, .max_live_orders = 4096, .max_symbols = 2});
    for (const char* name : testing::kStreamStocks) {
        entry.add_symbol(testing::unwrap(make_ticker(name)),
                         testing::with_test_limits(BookConfig{.tick_size = 100}));
    }
    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        protocol::Timestamp now = 0;
        for (const testing::InboundFrame& frame : stream) {
            (void)entry.on_frame(frame.owner, frame.bytes, ++now);
        }
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_GT(sink.frames, 50'000U);
    EXPECT_GT(entry.stats().malformed, 0U);
    EXPECT_GT(entry.engine().last_match_id(), 1'000U);
    EXPECT_TRUE(entry.engine().check_invariants());
}

TEST(NoAllocationTest, OrderEntryOverFastBooksNeverAllocates) {
    expect_order_entry_never_allocates<FastOrderBook>();
}

TEST(NoAllocationTest, OrderEntryOverSortedBooksNeverAllocates) {
    expect_order_entry_never_allocates<SortedOrderBook>();
}

}  // namespace
}  // namespace litbook
