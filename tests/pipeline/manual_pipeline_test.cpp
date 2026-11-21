#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/pipeline/matching_worker.hpp"
#include "litbook/pipeline/publisher.hpp"
#include "litbook/pipeline/queue_backend.hpp"
#include "litbook/pipeline/reply_pump.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "book/test_limits.hpp"
#include "gateway/soup_test_support.hpp"
#include "protocol/order_entry_stream.hpp"
#include "support/unwrap.hpp"

// The pipeline's pieces on one thread, driven by hand. The session layer sits on a
// QueueBackend, the matching worker is stepped directly, and the reply pump moves replies
// back. This checks the whole message flow, including backpressure and cancel on
// disconnect, with no threads and no sockets, so every run is the same.

namespace litbook::pipeline {
namespace {

using gateway::CloseReason;
using gateway::SessionConfig;
using gateway::soup::PacketType;
using gateway::testing::Bytes;
using gateway::testing::login;
using gateway::testing::packet;
using gateway::testing::parse_packets;
using protocol::Timestamp;

using Requests = concurrency::SpscQueue<Request>;
using Replies = concurrency::SpscQueue<Reply>;
using Events = concurrency::SpscQueue<MarketEvent>;
using Backend = QueueBackend<SortedOrderBook, Requests>;
using Sessions = gateway::BasicSessionManager<Backend>;
using Inline = gateway::SessionManager<SortedOrderBook>;
using Worker = MatchingWorker<SortedOrderBook, Requests, Replies, Events>;
using Pump = ReplyPump<Sessions, Replies>;

constexpr OwnerId kAccounts = 4;

protocol::OrderEntryConfig entry_config() {
    return {.max_owners = kAccounts, .max_live_orders = 8192, .max_symbols = 2};
}

SessionConfig session_config(std::uint32_t max_in_flight = 64) {
    return SessionConfig{.max_connections = 8,
                         .input_buffer_bytes = 4096,
                         .output_buffer_bytes = std::size_t{64} * 1024,
                         .output_high_water = std::size_t{32} * 1024,
                         .max_in_flight = max_in_flight};
}

// Accounts u1 to u4, all with password pw. The odd ones cancel on disconnect.
template <class Manager>
void add_accounts_and_stocks(Manager& m) {
    const std::array<std::string, kAccounts> names{"u1", "u2", "u3", "u4"};
    for (OwnerId owner = 1; owner <= kAccounts; ++owner) {
        m.add_account({.username = names.at(owner - 1),
                       .password = "pw",
                       .cancel_on_disconnect = owner % 2 == 1});
    }
    for (const char* name : litbook::testing::kStreamStocks) {
        m.add_symbol(litbook::testing::unwrap(make_ticker(name)),
                     litbook::testing::with_test_limits(BookConfig{.tick_size = 100}));
    }
}

std::string name_of(OwnerId owner) {
    return "u" + std::to_string(owner);
}

// The pipeline's pieces with their queues, all on the test thread.
struct ManualPipeline {
    explicit ManualPipeline(const SessionConfig& config, std::size_t request_capacity = 1024,
                            std::size_t max_batch = 1024)
        : requests(request_capacity),
          replies(4096),
          events(1U << 16U),
          worker(entry_config(), requests, replies, events, WaitMode::Spin, Doorbells{}),
          publisher(events, WaitMode::Spin, nullptr),
          sessions(config, std::in_place, worker.order_entry(), requests, nullptr),
          pump(sessions, replies, nullptr, max_batch) {
        add_accounts_and_stocks(sessions);
    }

    // Runs every piece until nothing is left to do anywhere.
    void settle(Timestamp now) {
        for (;;) {
            bool worked = false;
            while (worker.step()) {
                worked = true;
            }
            while (publisher.step()) {
            }
            if (replies.size_approx() > 0 || sessions.waiting_for_backend()) {
                pump.drain(now);
                worked = true;
            }
            if (!worked) {
                return;
            }
        }
    }

    Requests requests;
    Replies replies;
    Events events;
    Worker worker;
    PublisherStub<Events> publisher;
    Sessions sessions;
    Pump pump;
};

// The output waiting on a connection, which is then marked as written.
template <class Manager>
Bytes take(Manager& m, typename Manager::ConnectionId id, Timestamp now) {
    const auto out = m.output(id);
    Bytes copy(out.begin(), out.end());
    m.sent(id, out.size(), now);
    return copy;
}

// Feeds the same random client stream into the Phase 6 inline session layer and into the
// pipeline, settling the pipeline after every packet, and requires every connection to
// receive exactly the same bytes from both. That includes sequence numbers, timestamps,
// the replies caused by other accounts' orders, malformed frames that close a
// connection, reconnects and cancel on disconnect.
TEST(ManualPipelineTest, EveryConnectionReceivesTheSameBytesAsTheInlineGateway) {
    Inline reference(session_config(), entry_config());
    add_accounts_and_stocks(reference);
    ManualPipeline pipe(session_config());

    std::array<Inline::ConnectionId, kAccounts + 1> ref_conn{};
    std::array<Sessions::ConnectionId, kAccounts + 1> pipe_conn{};
    Timestamp now = 1'000;

    const auto compare_outputs = [&] {
        for (OwnerId owner = 1; owner <= kAccounts; ++owner) {
            const Bytes expected = take(reference, ref_conn.at(owner), now);
            const Bytes actual = take(pipe.sessions, pipe_conn.at(owner), now);
            ASSERT_EQ(actual, expected) << "account " << owner;
        }
    };
    const auto connect = [&](OwnerId owner) {
        ref_conn.at(owner) = reference.open(now);
        pipe_conn.at(owner) = pipe.sessions.open(now);
        const Bytes hello = login(name_of(owner), "pw");
        reference.receive(ref_conn.at(owner), hello, now);
        pipe.sessions.receive(pipe_conn.at(owner), hello, now);
    };
    const auto disconnect = [&](OwnerId owner) {
        reference.close(ref_conn.at(owner), CloseReason::PeerClosed, now);
        pipe.sessions.close(pipe_conn.at(owner), CloseReason::PeerClosed, now);
        pipe.settle(now);
    };

    for (OwnerId owner = 1; owner <= kAccounts; ++owner) {
        connect(owner);
    }
    compare_outputs();

    const auto stream = litbook::testing::order_entry_stream(20261120, 4000);
    std::mt19937_64 rng(7);
    int reconnects = 0;
    int malformed = 0;
    for (const auto& frame : stream) {
        now += 1'000;
        const Bytes p = packet(PacketType::UnsequencedData, frame.bytes);
        reference.receive(ref_conn.at(frame.owner), p, now);
        pipe.sessions.receive(pipe_conn.at(frame.owner), p, now);
        pipe.settle(now);
        compare_outputs();
        ASSERT_EQ(pipe.sessions.should_close(pipe_conn.at(frame.owner)),
                  reference.should_close(ref_conn.at(frame.owner)));
        if (reference.should_close(ref_conn.at(frame.owner))) {
            ++malformed;
            disconnect(frame.owner);
            connect(frame.owner);
            compare_outputs();
        } else if (rng() % 150 == 0) {
            ++reconnects;
            disconnect(frame.owner);
            compare_outputs();
            connect(frame.owner);
            compare_outputs();
        }
    }

    const auto& ref_orders = reference.order_entry();
    const auto& pipe_orders = pipe.worker.order_entry();
    EXPECT_EQ(pipe_orders.live_orders(), ref_orders.live_orders());
    // The pipeline's gateway refuses malformed frames before they reach the matching
    // thread, so they are counted there instead of in the order entry layer.
    protocol::OrderEntryStats expected_stats = ref_orders.stats();
    EXPECT_EQ(pipe.sessions.backend().malformed(), expected_stats.malformed);
    expected_stats.malformed = 0;
    EXPECT_EQ(pipe_orders.stats(), expected_stats);
    for (OwnerId owner = 1; owner <= kAccounts; ++owner) {
        EXPECT_EQ(pipe.sessions.next_sequence(owner), reference.next_sequence(owner));
        EXPECT_EQ(pipe_orders.open_orders(owner), ref_orders.open_orders(owner));
        EXPECT_EQ(pipe.sessions.in_flight(owner), 0U);
    }
    EXPECT_EQ(pipe.sessions.stats().messages_out, reference.stats().messages_out);
    EXPECT_EQ(pipe.sessions.stats().messages_dropped, reference.stats().messages_dropped);
    EXPECT_EQ(pipe.worker.stats().malformed, 0U);  // The gateway caught every bad frame.
    // The comparison only means something if the stream really did these things.
    EXPECT_GT(malformed, 5);
    EXPECT_GT(reconnects, 5);
    EXPECT_GT(pipe.publisher.stats().trades, 50U);
    EXPECT_GT(pipe.sessions.stats().messages_dropped, 0U);
}

// The same stream with tiny queues and limits, and the pieces run in a random order with
// random gaps, so requests pile up, the request queue fills, connections pause on their
// requests in flight and cancels on disconnect have to wait. The order across accounts
// can differ from the inline gateway here, so this test checks what must always hold.
// Every sequenced message reaches its connection or is counted as dropped, no request is
// lost, and the order entry layer matches the books at the end.
TEST(ManualPipelineTest, TinyQueuesAndARandomScheduleLoseNothing) {
    ManualPipeline pipe(session_config(3), 2, 1);
    std::array<Sessions::ConnectionId, kAccounts + 1> conn{};
    std::array<std::uint64_t, kAccounts + 1> received{};  // Sequenced messages per account.
    Timestamp now = 1'000;

    const auto read = [&](OwnerId owner) {
        for (const auto& p : parse_packets(take(pipe.sessions, conn.at(owner), now))) {
            if (p.type == PacketType::SequencedData) {
                ++received.at(owner);
            }
        }
    };
    const auto connect = [&](OwnerId owner) {
        conn.at(owner) = pipe.sessions.open(now);
        pipe.sessions.receive(conn.at(owner), login(name_of(owner), "pw"), now);
    };
    for (OwnerId owner = 1; owner <= kAccounts; ++owner) {
        connect(owner);
    }

    const auto stream = litbook::testing::order_entry_stream(99, 4000);
    std::mt19937_64 rng(11);
    std::uint64_t max_in_flight_seen = 0;
    std::uint64_t deferred_cancels_seen = 0;
    for (const auto& frame : stream) {
        now += 1'000;
        const OwnerId owner = frame.owner;
        pipe.sessions.receive(conn.at(owner), packet(PacketType::UnsequencedData, frame.bytes),
                              now);
        for (OwnerId o = 1; o <= kAccounts; ++o) {
            max_in_flight_seen =
                std::max<std::uint64_t>(max_in_flight_seen, pipe.sessions.in_flight(o));
        }
        // Step the matching worker a random number of times, but only while the reply
        // queue has room for a whole request's replies. On one thread a worker waiting
        // for room would wait forever.
        for (std::uint64_t i = rng() % 3; i > 0; --i) {
            if (pipe.replies.size_approx() + 256 < pipe.replies.capacity()) {
                (void)pipe.worker.step();
            }
        }
        while (pipe.publisher.step()) {
        }
        for (std::uint64_t i = rng() % 2; i > 0; --i) {
            pipe.pump.drain(now);
        }
        if (rng() % 2 == 0) {
            read(owner);
        }
        if (pipe.sessions.should_close(conn.at(owner)) || rng() % 200 == 0) {
            read(owner);
            pipe.sessions.close(conn.at(owner), CloseReason::PeerClosed, now);
            if (pipe.sessions.cancel_pending(owner)) {
                ++deferred_cancels_seen;
            }
            connect(owner);
        }
    }
    pipe.settle(now);
    std::uint64_t total = 0;
    for (OwnerId owner = 1; owner <= kAccounts; ++owner) {
        read(owner);
        total += received.at(owner);
        EXPECT_EQ(pipe.sessions.in_flight(owner), 0U);
        EXPECT_FALSE(pipe.sessions.cancel_pending(owner));
    }
    const auto& stats = pipe.sessions.stats();
    EXPECT_EQ(total + stats.messages_dropped, stats.messages_out);
    EXPECT_EQ(pipe.pump.stats().messages, stats.messages_out);
    EXPECT_EQ(pipe.pump.stats().done, pipe.worker.stats().frames);
    std::uint32_t open = 0;
    for (OwnerId owner = 1; owner <= kAccounts; ++owner) {
        open += pipe.worker.order_entry().open_orders(owner);
    }
    EXPECT_EQ(open, pipe.worker.order_entry().live_orders());
    EXPECT_TRUE(pipe.worker.order_entry().engine().check_invariants());
    // The paths this test is for must really have run.
    EXPECT_GT(stats.backend_busy, 10U);
    EXPECT_EQ(max_in_flight_seen, 3U);
    EXPECT_GT(deferred_cancels_seen + stats.cancels_deferred, 0U);
}

}  // namespace
}  // namespace litbook::pipeline
