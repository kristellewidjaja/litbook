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
#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/engine/engine.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/pipeline/threaded_gateway.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "book/book_test_support.hpp"
#include "book/workload.hpp"
#include "concurrency/queue_test_support.hpp"
#include "gateway/test_client.hpp"
#include "protocol/order_entry_stream.hpp"
#include "support/unwrap.hpp"

namespace {

// The hook must not allocate itself. The counters are atomic because the pipeline tests
// count allocations made on the pipeline's own threads too.
std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_allocations{0};

void* counted_alloc(std::size_t size, std::size_t alignment) {
    if (g_armed.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
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
        g_allocations.store(0);
        g_armed.store(true);
    }
    ~AllocationCounter() { g_armed.store(false); }
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    AllocationCounter(AllocationCounter&&) = delete;
    AllocationCounter& operator=(AllocationCounter&&) = delete;

    [[nodiscard]] std::uint64_t count() const noexcept { return g_allocations.load(); }
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

// The SoupBinTCP session layer in front of order entry must not allocate either, for
// logins, cut up packets, replies, heartbeats, disconnects with cancel on disconnect, and
// reconnects. The stream's malformed frames close their connection, and the test then
// reconnects and logs in again, so those paths run inside the counted region too.
template <class Book>
void expect_sessions_never_allocate() {
    using Manager = gateway::SessionManager<Book>;
    using ConnectionId = typename Manager::ConnectionId;
    constexpr OwnerId kOwners = 4;
    const std::array<std::string_view, kOwners + 1> users{"", "ann", "ben", "cy", "dee"};

    // Every packet is built before the counter is armed.
    const auto stream = testing::order_entry_stream(11, 50'000);
    std::vector<std::vector<std::byte>> packets;
    packets.reserve(stream.size());
    for (const testing::InboundFrame& frame : stream) {
        std::vector<std::byte> packet(gateway::soup::kHeaderSize + frame.bytes.size());
        (void)gateway::soup::encode_packet(gateway::soup::PacketType::UnsequencedData, frame.bytes,
                                           packet);
        packets.push_back(std::move(packet));
    }
    std::array<std::vector<std::byte>, kOwners + 1> logins{};
    for (OwnerId owner = 1; owner <= kOwners; ++owner) {
        const auto request = gateway::soup::make_login_request(users.at(owner), "pw", "", 0);
        logins.at(owner).resize(gateway::soup::kHeaderSize + gateway::soup::kLoginRequestPayload);
        (void)gateway::soup::encode_login_request(testing::unwrap(request), logins.at(owner));
    }

    Manager m(gateway::SessionConfig{.max_connections = 8},
              {.max_owners = kOwners, .max_live_orders = 4096, .max_symbols = 2});
    for (OwnerId owner = 1; owner <= kOwners; ++owner) {
        m.add_account({.username = users.at(owner),
                       .password = "pw",
                       .cancel_on_disconnect = owner % 2 == 1});
    }
    for (const char* name : testing::kStreamStocks) {
        m.add_symbol(testing::unwrap(make_ticker(name)),
                     testing::with_test_limits(BookConfig{.tick_size = 100}));
    }

    std::array<ConnectionId, kOwners + 1> ids{};
    std::uint64_t reconnects = 0;
    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        gateway::Timestamp now = 0;
        // Opens a connection for `owner`, logs it in and writes out the reply.
        auto connect = [&](OwnerId owner) {
            const ConnectionId id = m.open(now);
            ids.at(owner) = id;
            (void)m.receive(id, logins.at(owner), now);
            m.sent(id, m.output(id).size(), now);
        };
        for (OwnerId owner = 1; owner <= kOwners; ++owner) {
            connect(owner);
        }
        for (std::size_t i = 0; i < stream.size(); ++i) {
            now += 1'000;
            const OwnerId owner = stream[i].owner;
            const ConnectionId id = ids.at(owner);
            // Deliver each packet in two pieces, cut at a different place every time.
            const std::span<const std::byte> packet(packets[i]);
            const std::size_t cut = i % packet.size();
            (void)m.receive(id, packet.first(cut), now);
            (void)m.receive(id, packet.subspan(cut), now);
            for (OwnerId o = 1; o <= kOwners; ++o) {
                const ConnectionId c = ids.at(o);
                m.sent(c, m.output(c).size(), now);
            }
            if (m.should_close(id)) {
                m.close(id, gateway::CloseReason::PeerClosed, now);
                connect(owner);
                ++reconnects;
            }
            if (i % 100 == 0) {
                m.tick(now);
            }
        }
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_GT(reconnects, 100U);
    EXPECT_EQ(m.stats().logins_accepted, kOwners + reconnects);
    EXPECT_GT(m.stats().messages_out, 50'000U);
    EXPECT_GT(m.order_entry().engine().last_match_id(), 1'000U);
    EXPECT_TRUE(m.order_entry().engine().check_invariants());
}

TEST(NoAllocationTest, SessionsOverFastBooksNeverAllocate) {
    expect_sessions_never_allocate<FastOrderBook>();
}

TEST(NoAllocationTest, SessionsOverSortedBooksNeverAllocate) {
    expect_sessions_never_allocate<SortedOrderBook>();
}

// The epoll gateway serving real loopback sockets must not allocate either. The clients
// connect before the counter is armed. Malformed frames are left out of the stream,
// because they end the connection and making a new test client allocates its buffer (the
// session layer test above covers reconnects).
TEST(NoAllocationTest, EpollGatewayNeverAllocatesWhileServing) {
    using Manager = gateway::SessionManager<SortedOrderBook>;
    constexpr OwnerId kOwners = 4;
    const std::array<std::string_view, kOwners + 1> users{"", "ann", "ben", "cy", "dee"};

    const auto stream = testing::order_entry_stream(13, 20'000);
    std::vector<std::vector<std::byte>> packets;
    std::vector<OwnerId> owners;
    for (const testing::InboundFrame& frame : stream) {
        if (protocol::decode_inbound(frame.bytes).status != protocol::DecodeStatus::Ok) {
            continue;
        }
        std::vector<std::byte> packet(gateway::soup::kHeaderSize + frame.bytes.size());
        (void)gateway::soup::encode_packet(gateway::soup::PacketType::UnsequencedData, frame.bytes,
                                           packet);
        packets.push_back(std::move(packet));
        owners.push_back(frame.owner);
    }
    std::array<std::vector<std::byte>, kOwners + 1> logins{};
    for (OwnerId owner = 1; owner <= kOwners; ++owner) {
        const auto request = gateway::soup::make_login_request(users.at(owner), "pw", "", 0);
        logins.at(owner).resize(gateway::soup::kHeaderSize + gateway::soup::kLoginRequestPayload);
        (void)gateway::soup::encode_login_request(testing::unwrap(request), logins.at(owner));
    }

    Manager m(gateway::SessionConfig{.max_connections = 8},
              {.max_owners = kOwners, .max_live_orders = 4096, .max_symbols = 2});
    for (OwnerId owner = 1; owner <= kOwners; ++owner) {
        m.add_account({.username = users.at(owner), .password = "pw"});
    }
    for (const char* name : testing::kStreamStocks) {
        m.add_symbol(testing::unwrap(make_ticker(name)),
                     testing::with_test_limits(BookConfig{.tick_size = 100}));
    }
    gateway::EpollGateway<SortedOrderBook> gw(m, gateway::GatewayConfig{});
    // One client per account, indexed by owner. Slot 0 is a connection that never logs in,
    // which also checks that an idle connection costs nothing.
    std::vector<std::unique_ptr<gateway::testing::TestClient>> clients;
    for (OwnerId owner = 0; owner <= kOwners; ++owner) {
        clients.push_back(std::make_unique<gateway::testing::TestClient>(gw.port()));
    }
    std::uint64_t replies = 0;
    // Reads every client's replies and counts the packets.
    auto drain = [&] {
        for (OwnerId owner = 1; owner <= kOwners; ++owner) {
            auto& c = *clients.at(owner);
            c.read_available();
            while (c.next_packet().status == gateway::soup::PacketStatus::Complete) {
                ++replies;
            }
        }
    };

    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        for (OwnerId owner = 1; owner <= kOwners; ++owner) {
            (void)clients.at(owner)->send_some(logins.at(owner));
        }
        for (std::size_t i = 0; i < packets.size(); ++i) {
            auto& c = *clients.at(owners[i]);
            std::span<const std::byte> rest(packets[i]);
            while (!rest.empty()) {
                rest = rest.subspan(c.send_some(rest));
                (void)gw.poll_once(0);
                drain();
            }
        }
        // Let the last replies arrive.
        for (int round = 0; round < 200 && m.order_entry().stats().malformed == 0; ++round) {
            (void)gw.poll_once(1);
            drain();
            if (replies >= kOwners + m.stats().messages_out) {
                break;
            }
        }
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(gw.open_sockets(), kOwners + 1);
    EXPECT_EQ(m.stats().logins_accepted, kOwners);
    EXPECT_EQ(replies, kOwners + m.stats().messages_out);  // Logins plus every reply.
    EXPECT_GT(m.stats().messages_out, 15'000U);
    EXPECT_GT(m.order_entry().engine().last_match_id(), 500U);
}

// The queues allocate their slots in the constructor and never again. This pushes and
// pops through every call (including the waiting calls of MutexQueue, which do not wait
// here because there is always room or a message). The counter is a plain global, so
// this runs on one thread. Allocating or not does not depend on which thread calls.
template <class Queue>
void expect_queue_never_allocates() {
    using concurrency::testing::make_message;
    using concurrency::testing::TestMessage;
    Queue queue(64);
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        for (int round = 0; round < 10'000; ++round) {
            const int batch = 1 + (round % 64);
            for (int i = 0; i < batch; ++i) {
                if (queue.try_push(make_message(pushed))) {
                    ++pushed;
                }
            }
            TestMessage out;
            while (queue.try_pop(out)) {
                ++popped;
            }
            if (queue.try_push(make_message(pushed))) {
                ++pushed;
            }
            if (queue.front() != nullptr) {
                queue.pop();
                ++popped;
            }
            if constexpr (requires { queue.push_wait(out); }) {
                queue.push_wait(make_message(pushed));
                ++pushed;
                queue.pop_wait(out);
                ++popped;
            }
        }
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(pushed, popped);
    EXPECT_GT(pushed, 300'000U);
}

TEST(NoAllocationTest, SpscQueueNeverAllocates) {
    expect_queue_never_allocates<concurrency::SpscQueue<concurrency::testing::TestMessage>>();
}

TEST(NoAllocationTest, MutexQueueNeverAllocates) {
    expect_queue_never_allocates<concurrency::MutexQueue<concurrency::testing::TestMessage>>();
}

}  // namespace
}  // namespace litbook

namespace litbook {
namespace {

// The threaded pipeline serving 20,000 frames from four clients over real sockets. The
// counter is armed from the first login until stop() has joined the threads, so it sees
// every allocation on the gateway, matching and publisher threads while they serve, and
// during the shutdown (End of Session, cancel on disconnect, Stop). Starting the threads
// happens before, because creating a thread allocates.
template <pipeline::WaitMode Mode>
void expect_pipeline_never_allocates() {
    using Exchange = pipeline::ThreadedGateway<SortedOrderBook, concurrency::SpscQueue>;
    constexpr OwnerId kOwners = 4;
    const std::array<std::string_view, kOwners + 1> users{"", "ann", "ben", "cy", "dee"};

    const auto stream = testing::order_entry_stream(17, 20'000);
    std::vector<std::vector<std::byte>> packets;
    std::vector<OwnerId> owners;
    for (const testing::InboundFrame& frame : stream) {
        if (protocol::decode_inbound(frame.bytes).status != protocol::DecodeStatus::Ok) {
            continue;
        }
        std::vector<std::byte> packet(gateway::soup::kHeaderSize + frame.bytes.size());
        (void)gateway::soup::encode_packet(gateway::soup::PacketType::UnsequencedData, frame.bytes,
                                           packet);
        packets.push_back(std::move(packet));
        owners.push_back(frame.owner);
    }
    std::array<std::vector<std::byte>, kOwners + 1> logins{};
    for (OwnerId owner = 1; owner <= kOwners; ++owner) {
        const auto request = gateway::soup::make_login_request(users.at(owner), "pw", "", 0);
        logins.at(owner).resize(gateway::soup::kHeaderSize + gateway::soup::kLoginRequestPayload);
        (void)gateway::soup::encode_login_request(testing::unwrap(request), logins.at(owner));
    }

    Exchange exchange(gateway::SessionConfig{.max_connections = 8},
                      {.max_owners = kOwners, .max_live_orders = 4096, .max_symbols = 2},
                      gateway::GatewayConfig{}, pipeline::PipelineConfig{.wait = Mode});
    for (OwnerId owner = 1; owner <= kOwners; ++owner) {
        // Every account cancels on disconnect, so the shutdown has real work to do.
        exchange.add_account(
            {.username = users.at(owner), .password = "pw", .cancel_on_disconnect = true});
    }
    for (const char* name : testing::kStreamStocks) {
        exchange.add_symbol(testing::unwrap(make_ticker(name)),
                            testing::with_test_limits(BookConfig{.tick_size = 100}));
    }
    std::vector<std::unique_ptr<gateway::testing::TestClient>> clients;
    for (OwnerId owner = 0; owner <= kOwners; ++owner) {
        clients.push_back(std::make_unique<gateway::testing::TestClient>(exchange.port()));
    }
    exchange.start();

    std::uint64_t replies = 0;
    auto drain = [&] {
        for (OwnerId owner = 1; owner <= kOwners; ++owner) {
            auto& c = *clients.at(owner);
            c.read_available();
            while (c.next_packet().status == gateway::soup::PacketStatus::Complete) {
                ++replies;
            }
        }
    };

    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        for (OwnerId owner = 1; owner <= kOwners; ++owner) {
            (void)clients.at(owner)->send_some(logins.at(owner));
        }
        for (std::size_t i = 0; i < packets.size(); ++i) {
            auto& c = *clients.at(owners[i]);
            std::span<const std::byte> rest(packets[i]);
            while (!rest.empty()) {
                rest = rest.subspan(c.send_some(rest));
                drain();
            }
        }
        // Give the last replies a moment to come back before stopping.
        for (int round = 0; round < 50; ++round) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            drain();
        }
        exchange.stop();
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(exchange.sessions().stats().logins_accepted, kOwners);
    EXPECT_GT(exchange.sessions().stats().messages_out, 15'000U);
    EXPECT_GT(replies, 15'000U);
    EXPECT_GT(exchange.publisher().stats().trades, 500U);
    EXPECT_EQ(exchange.matching().order_entry().live_orders(), 0U);  // Cancel on disconnect.
}

TEST(NoAllocationTest, SpinningPipelineNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Spin>();
}

TEST(NoAllocationTest, BlockingPipelineNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Block>();
}

}  // namespace
}  // namespace litbook
