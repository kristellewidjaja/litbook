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
#include "litbook/feed/feed_handler.hpp"
#include "litbook/feed/feed_publisher.hpp"
#include "litbook/feed/feed_receiver.hpp"
#include "litbook/feed/udp_feed_sink.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/pipeline/feed_worker.hpp"
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
#include <type_traits>
#include <utility>
#include <vector>

#include "book/book_test_support.hpp"
#include "book/workload.hpp"
#include "concurrency/queue_test_support.hpp"
#include "feed/feed_forwarder.hpp"
#include "feed/lossy_channel.hpp"
#include "gateway/test_client.hpp"
#include "journal/replay_test_support.hpp"
#include "protocol/order_entry_stream.hpp"
#include "support/temp_dir.hpp"
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
// The feed side of the pipeline test. It owns the receiver, the sink the publisher sends
// through, and a reader thread that reads the feed the whole time. The reader has its
// own thread because stop() blocks the test thread while cancel on disconnect sends a
// burst, and a receiver nobody reads during a burst loses its end.
class FeedReader {
public:
    FeedReader() : receiver_(receiver_config()), sink_(network_config(receiver_)) {}

    [[nodiscard]] pipeline::FeedWorkerOptions publisher_options() {
        pipeline::FeedWorkerOptions o;
        o.feed.session = "ALLOC";
        o.feed.snapshot_spacing = 1'000'000;
        o.sink = &sink_;
        o.max_levels_per_side = 256;
        return o;
    }

    // Called before counting starts, because making a thread allocates.
    void start() {
        thread_ = std::thread([this] {
            while (!receiver_.handler().ended() && !give_up_.load()) {
                receiver_.poll_once(1);
            }
            done_ = true;
        });
    }

    // Waits a short while for End of Session, which may never come (see the test), then
    // stops the reader thread.
    void finish() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!done_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        give_up_ = true;
        thread_.join();
    }

    // Read this only after finish().
    [[nodiscard]] const feed::FeedHandler& handler() const { return receiver_.handler(); }

private:
    static feed::FeedReceiverConfig receiver_config() {
        feed::FeedReceiverConfig rc;
        rc.incremental = {.address = "239.255.80.1", .port = 0};
        rc.snapshot = {.address = "239.255.80.2", .port = 0};
        rc.handler = feed::FeedHandlerConfig{.session = "ALLOC", .max_symbols = 4};
        return rc;
    }

    static feed::FeedNetworkConfig network_config(const feed::FeedReceiver& receiver) {
        feed::FeedNetworkConfig network;
        network.incremental = {.address = "239.255.80.1", .port = receiver.incremental().port()};
        network.snapshot = {.address = "239.255.80.2", .port = receiver.snapshot().port()};
        return network;
    }

    feed::FeedReceiver receiver_;
    feed::UdpFeedSink sink_;
    std::thread thread_;
    std::atomic<bool> done_{false};
    std::atomic<bool> give_up_{false};
};

// The publisher's options. PublisherStub has none, FeedWorker gets the reader's sink.
template <class Exchange>
typename Exchange::PublisherOptions publisher_options_for(FeedReader* reader) {
    if constexpr (std::is_same_v<typename Exchange::PublisherOptions,
                                 pipeline::FeedWorkerOptions>) {
        return reader->publisher_options();
    } else {
        return {};
    }
}

// Runs the threaded pipeline over real sockets. With WithFeed its publisher thread runs
// FeedWorker and sends the market data feed over multicast on lo, and a reader thread of
// the test reads it with a FeedReceiver the whole time, inside the counted part too. The
// reader has its own thread because stop() blocks the test thread while cancel on
// disconnect sends a burst, and a receiver nobody reads during a burst loses its end.
// With WithJournal the pipeline also has its journal thread, which writes every request
// to a journal file in a temporary directory before the matching thread sees it.
template <pipeline::WaitMode Mode, bool WithFeed, bool WithJournal = false>
void expect_pipeline_never_allocates() {
    using FeedExchange = pipeline::BasicThreadedGateway<SortedOrderBook, concurrency::SpscQueue,
                                                        pipeline::FeedWorker>;
    using StubExchange = pipeline::ThreadedGateway<SortedOrderBook, concurrency::SpscQueue>;
    using Exchange = std::conditional_t<WithFeed, FeedExchange, StubExchange>;
    constexpr OwnerId kOwners = 4;
    const std::array<std::string_view, kOwners + 1> users{"", "ann", "ben", "cy", "dee"};

    // The 20,000 frames give a little more than this many replies.
    constexpr std::uint64_t kEnoughReplies = 15'000;
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

    // Only the run with the feed has a reader. It is made before the exchange, because the
    // publisher sends to the ports its receiver got.
    const std::unique_ptr<FeedReader> reader =
        WithFeed ? std::make_unique<FeedReader>() : std::unique_ptr<FeedReader>{};
    const auto publisher_options = publisher_options_for<Exchange>(reader.get());
    const testing::TempDir dir;
    // Small segments, so the journal thread rotates during the run. Snapshots are off,
    // because the snapshots at start and stop and the snapshot thread's cleanup allocate,
    // and this test counts every allocation in the process.
    const pipeline::JournalOptions journal{
        .directory = dir.file("alloc"), .records_per_segment = 1000, .keep_snapshots = 0};
    Exchange exchange(gateway::SessionConfig{.max_connections = 8},
                      {.max_owners = kOwners, .max_live_orders = 4096, .max_symbols = 2},
                      gateway::GatewayConfig{}, pipeline::PipelineConfig{.wait = Mode},
                      publisher_options, WithJournal ? &journal : nullptr);
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
    if (reader) {
        reader->start();
    }

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
        // Keep reading until enough replies came back. A fixed wait is not enough on a
        // slow machine (CI runs this under ASan on two cores), because stop() does not
        // serve input that is still waiting, so the count would depend on the machine.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (replies <= kEnoughReplies && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            drain();
        }
        exchange.stop();
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(exchange.sessions().stats().logins_accepted, kOwners);
    EXPECT_GT(exchange.sessions().stats().messages_out, kEnoughReplies);
    EXPECT_GT(replies, kEnoughReplies);
    EXPECT_EQ(exchange.matching().order_entry().live_orders(), 0U);  // Cancel on disconnect.
    if constexpr (WithJournal) {
        ASSERT_NE(exchange.journal(), nullptr);
        EXPECT_FALSE(exchange.journal()->failed());
        EXPECT_EQ(exchange.journal()->stats().forwarded, exchange.matching().stats().requests);
        EXPECT_GT(exchange.journal()->writer().stats().records, kEnoughReplies / 2);
    } else {
        EXPECT_EQ(exchange.journal(), nullptr);
    }
    if constexpr (WithFeed) {
        reader->finish();
        const feed::FeedHandler& handler = reader->handler();
        EXPECT_GT(handler.stats().incremental_packets, 100U);
        EXPECT_GT(exchange.publisher().feed().stats().snapshots, 0U);
        // The feed is UDP, and this test is about allocations, not loss. Cancel on
        // disconnect at the end sends a burst faster than the reader can empty its socket,
        // so the tail of the feed and End of Session can be lost, and then nothing is
        // left to show the gap. Only a handler that saw End of Session with nothing
        // missing must have the feed's books.
        if (handler.ended() && !handler.recovering()) {
            for (feed::Locate l = 0; l < 2; ++l) {
                EXPECT_EQ(*handler.book(l), exchange.publisher().feed().copy(l));
            }
        }
    } else {
        EXPECT_GT(exchange.publisher().stats().trades, 500U);
    }
}

TEST(NoAllocationTest, SpinningPipelineNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Spin, false>();
}

TEST(NoAllocationTest, BlockingPipelineNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Block, false>();
}

TEST(NoAllocationTest, SpinningPipelineWithTheFeedNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Spin, true>();
}

TEST(NoAllocationTest, BlockingPipelineWithTheFeedNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Block, true>();
}

TEST(NoAllocationTest, SpinningPipelineWithTheJournalNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Spin, false, true>();
}

TEST(NoAllocationTest, BlockingPipelineWithTheJournalNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Block, false, true>();
}

TEST(NoAllocationTest, BlockingPipelineWithTheFeedAndTheJournalNeverAllocatesWhileServing) {
    expect_pipeline_never_allocates<pipeline::WaitMode::Block, true, true>();
}

// The market data path must not allocate either. Orders go through the order entry layer,
// their market data through the publisher, and the packets through a channel that loses,
// duplicates and reorders some of them into a feed handler, so gap detection, buffering,
// snapshot assembly and recovery all run inside the counted region.
TEST(NoAllocationTest, MarketDataFeedNeverAllocates) {
    const auto stream = testing::order_entry_stream(17, 30'000);
    feed::FeedHandler handler(feed::FeedHandlerConfig{
        .session = "ALLOC", .max_symbols = 4, .max_levels_per_side = 256, .buffer_capacity = 4096});
    feed::testing::LossyChannel channel(
        handler, 17,
        feed::testing::LossRates{.drop = 30, .duplicate = 20, .hold = 20, .snapshot_lag = 3});
    feed::FeedPublisherConfig config;
    config.session = "ALLOC";
    config.snapshot_spacing = 20'000;
    feed::FeedPublisher publisher(channel, config);
    feed::testing::FeedForwarder forwarder(publisher);
    feed::testing::DiscardReplies replies;
    protocol::OrderEntry<SortedOrderBook> entry(
        replies, {.max_owners = 4, .max_live_orders = 4096, .max_symbols = 2});
    entry.set_market_data_sink(&forwarder);
    for (const char* name : testing::kStreamStocks) {
        const Ticker ticker = testing::unwrap(make_ticker(name));
        entry.add_symbol(ticker, testing::with_test_limits(BookConfig{.tick_size = 100}));
        publisher.add_symbol(
            {.symbol = ticker, .tick_size = 100, .lot_size = 1, .max_levels_per_side = 256});
    }

    std::uint64_t count = 0;
    {
        const AllocationCounter counter;
        feed::Timestamp now = 0;
        publisher.start(now);
        std::uint64_t frames = 0;
        for (const testing::InboundFrame& frame : stream) {
            now += 1'000;
            forwarder.set_now(now);
            (void)entry.on_frame(frame.owner, frame.bytes, now);
            if (++frames % 3 == 0) {
                publisher.flush(now);
            }
            if (frames % 10 == 0) {
                publisher.tick(now);
            }
        }
        channel.set_lossless();
        now += config.heartbeat_interval;
        publisher.tick(now);
        for (int i = 0; i < 8; ++i) {
            now += config.snapshot_spacing;
            publisher.tick(now);
        }
        publisher.end_session(now);
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_GT(handler.stats().gaps, 10U);
    EXPECT_GT(handler.stats().snapshots_applied, 10U);
    EXPECT_GT(handler.stats().replayed, 0U);
    EXPECT_FALSE(handler.recovering());
    EXPECT_TRUE(handler.ended());
    for (feed::Locate l = 0; l < 2; ++l) {
        EXPECT_EQ(*handler.book(l), publisher.copy(l));
    }
}

}  // namespace
}  // namespace litbook

namespace litbook {
namespace {

// Market events counted in plain integers, so they do not allocate.
class CountingMarketData final : public protocol::MarketDataSink {
public:
    void on_trade(SymbolId /*symbol*/, const Trade& /*trade*/) override { ++trades; }
    void on_level_update(SymbolId /*symbol*/, const LevelUpdate& /*update*/) override { ++levels; }
    std::uint64_t trades = 0;
    std::uint64_t levels = 0;
};

// The journal writer is on the path of every request in Phase 9, so appending and
// committing must not allocate once the writer exists. Its buffer is made in create().
TEST(NoAllocationTest, JournalWriterNeverAllocatesWhileAppendingAndCommitting) {
    const testing::TempDir dir;
    const auto requests = journal::testing::replay_stream(3, 50'000);
    journal::JournalWriterConfig config;
    // Small enough that the buffer fills between the regular commits, so the full buffer
    // path (commit, then append again) is covered too.
    config.buffer_capacity = 1024;
    journal::JournalWriter writer = journal::JournalWriter::create(dir.file("a.journal"), config);
    std::uint64_t count = 0;
    std::uint64_t full = 0;
    {
        const AllocationCounter counter;
        int since_commit = 0;
        for (const pipeline::Request& r : requests) {
            journal::JournalStatus status = writer.append(r);
            if (status == journal::JournalStatus::BufferFull) {
                ++full;
                status = writer.commit() == journal::JournalStatus::Ok ? writer.append(r) : status;
            }
            ASSERT_EQ(status, journal::JournalStatus::Ok);
            if (++since_commit == 64) {
                ASSERT_EQ(writer.commit(), journal::JournalStatus::Ok);
                since_commit = 0;
            }
        }
        ASSERT_EQ(writer.flush(), journal::JournalStatus::Ok);
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(writer.stats().records, 50'000U);
    EXPECT_GT(writer.stats().commits, 700U);
    EXPECT_GT(full, 100U);
}

// Scanning a journal and replaying its requests must not allocate either once the setup
// records have built the order entry layer, so recovery has the same no-allocation
// property as the matching thread.
template <class Book>
void expect_replay_never_allocates() {
    const testing::TempDir dir;
    const std::string path = dir.file("a.journal");
    {
        journal::JournalWriter writer = journal::JournalWriter::create(path, {});
        ASSERT_EQ(writer.append_session_start(0, journal::testing::replay_order_entry_config()),
                  journal::JournalStatus::Ok);
        SymbolId id = 0;
        for (const char* name : journal::testing::kReplayStocks) {
            ASSERT_EQ(
                writer.append_add_symbol(
                    0, journal::SymbolListing{.id = id,
                                              .ticker = testing::unwrap(make_ticker(name)),
                                              .config = journal::testing::replay_book_config()}),
                journal::JournalStatus::Ok);
            ++id;
        }
        for (const pipeline::Request& r : journal::testing::replay_stream(4, 40'000)) {
            if (writer.append(r) == journal::JournalStatus::BufferFull) {
                ASSERT_EQ(writer.commit(), journal::JournalStatus::Ok);
                ASSERT_EQ(writer.append(r), journal::JournalStatus::Ok);
            }
        }
        ASSERT_EQ(writer.flush(), journal::JournalStatus::Ok);
    }
    const journal::MappedFile file(path);
    CountingSink replies;
    CountingMarketData events;
    journal::Replayer<Book> replayer(replies, &events);
    journal::RecordReader reader(file.bytes().subspan(journal::kFileHeaderLength));
    journal::JournalRecord record;
    // The setup records allocate the order entry layer and the books, as startup does.
    for (std::size_t i = 0; i < 1 + journal::testing::kReplayStocks.size(); ++i) {
        ASSERT_TRUE(reader.next(record));
        ASSERT_EQ(replayer.apply(record), journal::ReplayStatus::Ok);
    }
    std::uint64_t count = 0;
    journal::JournalScan scan;
    {
        const AllocationCounter counter;
        scan = journal::scan_journal(file.bytes());
        while (reader.next(record)) {
            if (replayer.apply(record) != journal::ReplayStatus::Ok) {
                break;
            }
        }
        count = counter.count();
    }
    EXPECT_EQ(count, 0U);
    EXPECT_EQ(scan.tail, journal::TailStatus::Clean);
    EXPECT_EQ(replayer.status(), journal::ReplayStatus::Ok);
    EXPECT_EQ(replayer.stats().frames + replayer.stats().cancel_alls, 40'000U);
    EXPECT_GT(events.trades, 1'000U);
    EXPECT_GT(replies.frames, 40'000U);
}

TEST(NoAllocationTest, ReplayOverFastBooksNeverAllocatesAfterSetup) {
    expect_replay_never_allocates<FastOrderBook>();
}

TEST(NoAllocationTest, ReplayOverSortedBooksNeverAllocatesAfterSetup) {
    expect_replay_never_allocates<SortedOrderBook>();
}

}  // namespace
}  // namespace litbook
