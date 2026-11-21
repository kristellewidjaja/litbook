#include "litbook/book/sorted_order_book.hpp"
#include "litbook/gateway/order_backend.hpp"
#include "litbook/gateway/session_manager.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "book/test_limits.hpp"
#include "gateway/soup_test_support.hpp"
#include "support/unwrap.hpp"

// Tests of the session layer in front of an asynchronous backend, the way the gateway
// thread of the pipeline uses it. A fake backend records what it was given and can be
// told to be busy, so every rule about requests in flight is checked without threads.

namespace litbook::gateway {
namespace {

using testing::Bytes;
using testing::login;
using testing::packet;
using testing::parse_packets;
using testing::ServerPacket;

constexpr Timestamp kSecond = 1'000'000'000;
constexpr OwnerId kAlice = 1;  // Cancel on disconnect is on.
constexpr OwnerId kBob = 2;

// What the fake backend was asked to do, in order.
struct Call {
    enum class Kind : std::uint8_t { Forward, CancelAll };
    Kind kind;
    OwnerId owner;
    Bytes frame;  // Empty for CancelAll.
};

// An asynchronous backend that only records. A frame whose first byte is '?' counts as
// malformed. While busy is true every forward and cancel is refused.
class FakeBackend {
public:
    static constexpr bool kAsynchronous = true;

    explicit FakeBackend(OwnerId max_owners) : max_owners_(max_owners) {}

    void attach(ReplyTarget& /*target*/) noexcept {}

    ForwardResult forward(OwnerId owner, std::span<const std::byte> frame,
                          protocol::Timestamp /*now*/) {
        if (!frame.empty() && frame[0] == std::byte{'?'}) {
            return ForwardResult::Malformed;
        }
        if (busy) {
            return ForwardResult::Busy;
        }
        calls.push_back(Call{.kind = Call::Kind::Forward,
                             .owner = owner,
                             .frame = Bytes(frame.begin(), frame.end())});
        return ForwardResult::Taken;
    }

    bool cancel_all(OwnerId owner, protocol::Timestamp /*now*/) {
        if (busy) {
            return false;
        }
        calls.push_back(Call{.kind = Call::Kind::CancelAll, .owner = owner, .frame = {}});
        return true;
    }

    void set_risk_limits(OwnerId /*owner*/, const protocol::RiskLimits& /*limits*/) {}
    SymbolId add_symbol(Ticker /*ticker*/, const BookConfig& /*config*/) { return 0; }
    [[nodiscard]] OwnerId max_owners() const noexcept { return max_owners_; }

    [[nodiscard]] std::size_t forwards() const {
        std::size_t n = 0;
        for (const Call& c : calls) {
            n += c.kind == Call::Kind::Forward ? 1 : 0;
        }
        return n;
    }

    bool busy = false;
    std::vector<Call> calls;

private:
    OwnerId max_owners_;
};

static_assert(OrderBackend<FakeBackend>);
static_assert(OrderBackend<InlineBackend<SortedOrderBook>>);

using AsyncManager = BasicSessionManager<FakeBackend>;
using ConnectionId = AsyncManager::ConnectionId;

SessionConfig async_config() {
    return SessionConfig{.session_name = "1",
                         .max_connections = 4,
                         .input_buffer_bytes = 4096,
                         .output_buffer_bytes = 4096,
                         .output_high_water = 2048,
                         .max_in_flight = 1000,
                         .heartbeat_interval = kSecond,
                         .idle_timeout = 15 * kSecond,
                         .login_timeout = 30 * kSecond};
}

std::unique_ptr<AsyncManager> make_manager(const SessionConfig& config = async_config()) {
    auto m = std::make_unique<AsyncManager>(config, std::in_place, OwnerId{4});
    m->add_account({.username = "alice", .password = "secret", .cancel_on_disconnect = true});
    m->add_account({.username = "bob", .password = "hunter2"});
    return m;
}

// An Unsequenced Data packet whose two payload bytes are 'O' and a number, so the fake
// backend's record shows which packet it was.
Bytes order_packet(std::uint8_t number) {
    const std::array<std::byte, 2> payload{std::byte{'O'}, std::byte{number}};
    return packet(soup::PacketType::UnsequencedData, payload);
}

// The packets that wait in a connection's output, then clears it as if written.
std::vector<ServerPacket> take_output(AsyncManager& m, ConnectionId id) {
    const std::span<const std::byte> out = m.output(id);
    std::vector<ServerPacket> packets = parse_packets(out);
    m.sent(id, out.size(), 0);
    return packets;
}

ConnectionId logged_in(AsyncManager& m, std::string_view user, std::string_view password) {
    const ConnectionId id = m.open(0);
    EXPECT_NE(id, AsyncManager::kNoConnection);
    m.receive(id, login(user, password), 0);
    const auto packets = take_output(m, id);
    EXPECT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets.front().type, soup::PacketType::LoginAccepted);
    return id;
}

TEST(SessionManagerAsyncTest, ForwardsEachOrderAndCountsItInFlight) {
    auto m = make_manager();
    const ConnectionId a = logged_in(*m, "alice", "secret");
    for (std::uint8_t i = 1; i <= 3; ++i) {
        m->receive(a, order_packet(i), 0);
    }
    ASSERT_EQ(m->backend().calls.size(), 3U);
    for (std::uint8_t i = 1; i <= 3; ++i) {
        const Call& c = m->backend().calls[i - 1U];
        EXPECT_EQ(c.kind, Call::Kind::Forward);
        EXPECT_EQ(c.owner, kAlice);
        EXPECT_EQ(c.frame, (Bytes{std::byte{'O'}, std::byte{i}}));
    }
    EXPECT_EQ(m->in_flight(kAlice), 3U);
    EXPECT_EQ(m->in_flight(kBob), 0U);
    EXPECT_EQ(m->stats().packets_in, 4U);  // The login and three orders.
    for (int i = 0; i < 3; ++i) {
        m->request_done(kAlice, 0);
    }
    EXPECT_EQ(m->in_flight(kAlice), 0U);
}

TEST(SessionManagerAsyncTest, DeliveredRepliesGetTheNextSequenceNumbers) {
    auto m = make_manager();
    const ConnectionId a = logged_in(*m, "alice", "secret");
    const Bytes frame{std::byte{'C'}, std::byte{1}, std::byte{2}};
    m->deliver(kAlice, frame, kSecond);
    m->deliver(kAlice, frame, kSecond);
    const auto packets = take_output(*m, a);
    ASSERT_EQ(packets.size(), 2U);
    for (const ServerPacket& p : packets) {
        EXPECT_EQ(p.type, soup::PacketType::SequencedData);
        EXPECT_EQ(p.payload, frame);
    }
    EXPECT_EQ(m->next_sequence(kAlice), 3U);
    EXPECT_EQ(m->stats().messages_out, 2U);
    EXPECT_EQ(m->stats().messages_dropped, 0U);
}

TEST(SessionManagerAsyncTest, RepliesAfterTheConnectionClosedAreDroppedButNumbered) {
    auto m = make_manager();
    const ConnectionId b = logged_in(*m, "bob", "hunter2");
    m->receive(b, order_packet(1), 0);
    m->close(b, CloseReason::PeerClosed, 0);
    m->deliver(kBob, Bytes{std::byte{'A'}}, 0);
    m->request_done(kBob, 0);
    EXPECT_EQ(m->stats().messages_dropped, 1U);
    EXPECT_EQ(m->next_sequence(kBob), 2U);
    EXPECT_EQ(m->in_flight(kBob), 0U);
}

TEST(SessionManagerAsyncTest, TheInFlightLimitHoldsInputBackUntilRequestsFinish) {
    SessionConfig config = async_config();
    config.max_in_flight = 2;
    auto m = make_manager(config);
    const ConnectionId a = logged_in(*m, "alice", "secret");
    Bytes burst;
    for (std::uint8_t i = 1; i <= 5; ++i) {
        const Bytes p = order_packet(i);
        burst.insert(burst.end(), p.begin(), p.end());
    }
    EXPECT_EQ(m->receive(a, burst, 0), burst.size());  // Taken into the input buffer.
    EXPECT_EQ(m->backend().forwards(), 2U);
    EXPECT_EQ(m->in_flight(kAlice), 2U);
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->backend().forwards(), 3U);
    m->request_done(kAlice, 0);
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->backend().forwards(), 5U);
    // Every packet went through, in the order it was sent.
    for (std::uint8_t i = 1; i <= 5; ++i) {
        EXPECT_EQ(m->backend().calls[i - 1U].frame[1], std::byte{i});
    }
}

TEST(SessionManagerAsyncTest, EachRequestInFlightReservesRoomForOneReply) {
    // With a high water mark of 2,048 bytes and 50 bytes reserved per request, the 41st
    // request brings the reservation to 2,050 bytes, so forwarding stops there.
    auto m = make_manager();
    const ConnectionId a = logged_in(*m, "alice", "secret");
    Bytes burst;
    for (int i = 0; i < 60; ++i) {
        const Bytes p = order_packet(static_cast<std::uint8_t>(i));
        burst.insert(burst.end(), p.begin(), p.end());
    }
    m->receive(a, burst, 0);
    EXPECT_EQ(m->backend().forwards(), 41U);
    // A reply that arrives does not free room by itself, because it now sits in the
    // output. Writing it out and finishing the request does.
    m->deliver(kAlice, Bytes(47, std::byte{'E'}), 0);
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->backend().forwards(), 41U);
    (void)take_output(*m, a);  // sent() resumes the input as well.
    EXPECT_EQ(m->backend().forwards(), 42U);
}

TEST(SessionManagerAsyncTest, ABusyBackendKeepsThePacketAndRetryWaitingSendsItInOrder) {
    auto m = make_manager();
    const ConnectionId a = logged_in(*m, "alice", "secret");
    const ConnectionId b = logged_in(*m, "bob", "hunter2");
    m->backend().busy = true;
    m->receive(a, order_packet(1), 0);
    m->receive(a, order_packet(2), 0);
    m->receive(b, order_packet(3), 0);
    EXPECT_EQ(m->backend().forwards(), 0U);
    EXPECT_TRUE(m->waiting_for_backend());
    EXPECT_GE(m->stats().backend_busy, 2U);
    EXPECT_EQ(m->stats().packets_in, 2U);  // Only the two logins so far.
    EXPECT_EQ(m->in_flight(kAlice), 0U);

    m->retry_waiting(0);  // Still busy, nothing changes.
    EXPECT_EQ(m->backend().forwards(), 0U);
    EXPECT_TRUE(m->waiting_for_backend());

    m->backend().busy = false;
    m->retry_waiting(0);
    ASSERT_EQ(m->backend().forwards(), 3U);
    EXPECT_FALSE(m->waiting_for_backend());
    EXPECT_EQ(m->backend().calls[0].frame[1], std::byte{1});
    EXPECT_EQ(m->backend().calls[1].frame[1], std::byte{2});
    EXPECT_EQ(m->backend().calls[2].owner, kBob);
    EXPECT_EQ(m->in_flight(kAlice), 2U);
    EXPECT_EQ(m->in_flight(kBob), 1U);
    EXPECT_EQ(m->stats().packets_in, 5U);
}

TEST(SessionManagerAsyncTest, RetryWaitingDoesNothingWhenNothingWaits) {
    auto m = make_manager();
    const ConnectionId a = logged_in(*m, "alice", "secret");
    m->receive(a, order_packet(1), 0);
    EXPECT_FALSE(m->waiting_for_backend());
    m->retry_waiting(0);
    EXPECT_EQ(m->backend().forwards(), 1U);
}

TEST(SessionManagerAsyncTest, AMalformedFrameClosesTheConnection) {
    auto m = make_manager();
    const ConnectionId a = logged_in(*m, "alice", "secret");
    const std::array<std::byte, 1> bad{std::byte{'?'}};
    m->receive(a, packet(soup::PacketType::UnsequencedData, bad), 0);
    EXPECT_TRUE(m->should_close(a));
    EXPECT_EQ(m->session(a).close_reason(), CloseReason::MalformedMessage);
    EXPECT_EQ(m->in_flight(kAlice), 0U);
}

TEST(SessionManagerAsyncTest, CancelOnDisconnectIsSentToTheBackend) {
    auto m = make_manager();
    const ConnectionId a = logged_in(*m, "alice", "secret");
    const ConnectionId b = logged_in(*m, "bob", "hunter2");
    m->close(b, CloseReason::PeerClosed, 0);  // bob did not ask for it.
    m->close(a, CloseReason::PeerClosed, 0);
    ASSERT_EQ(m->backend().calls.size(), 1U);
    EXPECT_EQ(m->backend().calls[0].kind, Call::Kind::CancelAll);
    EXPECT_EQ(m->backend().calls[0].owner, kAlice);
    EXPECT_EQ(m->stats().cancels_deferred, 0U);
}

TEST(SessionManagerAsyncTest, ADeferredCancelGoesBeforeTheAccountsNextOrders) {
    auto m = make_manager();
    const ConnectionId first = logged_in(*m, "alice", "secret");
    m->backend().busy = true;
    m->close(first, CloseReason::PeerClosed, 0);
    EXPECT_TRUE(m->cancel_pending(kAlice));
    EXPECT_EQ(m->stats().cancels_deferred, 1U);
    EXPECT_TRUE(m->waiting_for_backend());

    // alice comes back and sends an order. It must wait behind the cancel, or the cancel
    // would wrongly cancel it too.
    const ConnectionId second = logged_in(*m, "alice", "secret");
    m->backend().busy = false;
    m->receive(second, order_packet(9), 0);
    EXPECT_TRUE(m->backend().calls.empty());

    m->retry_waiting(0);
    ASSERT_EQ(m->backend().calls.size(), 2U);
    EXPECT_EQ(m->backend().calls[0].kind, Call::Kind::CancelAll);
    EXPECT_EQ(m->backend().calls[1].kind, Call::Kind::Forward);
    EXPECT_EQ(m->backend().calls[1].frame[1], std::byte{9});
    EXPECT_FALSE(m->cancel_pending(kAlice));
    EXPECT_FALSE(m->waiting_for_backend());
}

TEST(SessionManagerAsyncTest, RequestDoneForAnUnknownOrIdleOwnerChangesNothing) {
    auto m = make_manager();
    m->request_done(kNoOwner, 0);
    m->request_done(99, 0);
    m->request_done(kBob, 0);
    EXPECT_EQ(m->in_flight(kBob), 0U);
    EXPECT_THROW((void)m->in_flight(99), std::out_of_range);
}

TEST(SessionManagerAsyncTest, ZeroMaxInFlightIsRejected) {
    SessionConfig config = async_config();
    config.max_in_flight = 0;
    EXPECT_THROW(AsyncManager(config, std::in_place, OwnerId{4}), std::invalid_argument);
}

TEST(SessionManagerAsyncTest, AnInlineBackendNeverHasRequestsInFlight) {
    SessionManager<SortedOrderBook> m(
        async_config(),
        protocol::OrderEntryConfig{.max_owners = 2, .max_live_orders = 64, .max_symbols = 1});
    m.add_account({.username = "alice", .password = "secret"});
    m.add_symbol(litbook::testing::unwrap(make_ticker("AAPL")),
                 litbook::testing::with_test_limits(BookConfig{.tick_size = 100}));
    const auto id = m.open(0);
    m.receive(id, login("alice", "secret"), 0);
    m.receive(id,
              testing::unsequenced(
                  protocol::EnterOrder{.user_ref = 1,
                                       .side = Side::Buy,
                                       .qty = 100,
                                       .symbol = litbook::testing::unwrap(make_ticker("AAPL")),
                                       .price = 10'000,
                                       .tif = TimeInForce::GoodTillCancel,
                                       .type = OrderType::Limit,
                                       .post_only = false}),
              0);
    EXPECT_EQ(m.in_flight(kAlice), 0U);
    EXPECT_EQ(m.order_entry().live_orders(), 1U);
    EXPECT_EQ(m.next_sequence(kAlice), 2U);  // The Accepted came back at once.
}

// Sampling. The values are real clock readings, so these tests only count samples.

SessionConfig sampling_config(std::uint32_t every) {
    SessionConfig config = async_config();
    config.sample_every = every;
    return config;
}

TEST(SessionManagerAsyncTest, NoSamplesAreTakenWhenSamplingIsOff) {
    auto m = make_manager();
    const ConnectionId a = logged_in(*m, "alice", "secret");
    m->receive(a, order_packet(1), 0);
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->request_latency().count(), 0U);
}

TEST(SessionManagerAsyncTest, ASampleEndsWhenItsOwnRequestIsDone) {
    auto m = make_manager(sampling_config(1));
    const ConnectionId a = logged_in(*m, "alice", "secret");
    m->receive(a, order_packet(1), 0);  // Sampled.
    m->receive(a, order_packet(2), 0);  // Not sampled, one sample is already running.
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->request_latency().count(), 1U);
    m->receive(a, order_packet(3), 0);  // Sampled, and done after order 2.
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->request_latency().count(), 1U);
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->request_latency().count(), 2U);
}

TEST(SessionManagerAsyncTest, AnotherAccountsRequestDoneDoesNotEndTheSample) {
    auto m = make_manager(sampling_config(1));
    const ConnectionId a = logged_in(*m, "alice", "secret");
    const ConnectionId b = logged_in(*m, "bob", "hunter2");
    m->receive(b, order_packet(1), 0);  // Sampled.
    m->receive(a, order_packet(2), 0);
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->request_latency().count(), 0U);
    m->request_done(kBob, 0);
    EXPECT_EQ(m->request_latency().count(), 1U);
}

TEST(SessionManagerAsyncTest, OnlyEveryNthOrderIsSampled) {
    auto m = make_manager(sampling_config(3));
    const ConnectionId a = logged_in(*m, "alice", "secret");
    for (std::uint8_t i = 1; i <= 7; ++i) {
        m->receive(a, order_packet(i), 0);
        m->request_done(kAlice, 0);
    }
    EXPECT_EQ(m->request_latency().count(), 2U);  // Orders 3 and 6.
}

TEST(SessionManagerAsyncTest, ABusyOrMalformedOrderIsNotSampled) {
    auto m = make_manager(sampling_config(1));
    const ConnectionId a = logged_in(*m, "alice", "secret");
    m->backend().busy = true;
    m->receive(a, order_packet(1), 0);
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->request_latency().count(), 0U);
    m->backend().busy = false;
    m->retry_waiting(0);  // Taken now, and sampled once.
    m->request_done(kAlice, 0);
    EXPECT_EQ(m->request_latency().count(), 1U);
    const std::array<std::byte, 1> bad{std::byte{'?'}};
    m->receive(a, packet(soup::PacketType::UnsequencedData, bad), 0);
    EXPECT_EQ(m->request_latency().count(), 1U);
}

TEST(SessionManagerAsyncTest, AnInlineBackendSamplesEachForward) {
    SessionConfig config = async_config();
    config.sample_every = 2;
    SessionManager<SortedOrderBook> m(
        config,
        protocol::OrderEntryConfig{.max_owners = 2, .max_live_orders = 64, .max_symbols = 1});
    m.add_account({.username = "alice", .password = "secret"});
    const Ticker aapl = litbook::testing::unwrap(make_ticker("AAPL"));
    m.add_symbol(aapl, litbook::testing::with_test_limits(BookConfig{.tick_size = 100}));
    const auto id = m.open(0);
    m.receive(id, login("alice", "secret"), 0);
    for (protocol::UserRefNum ref = 1; ref <= 4; ++ref) {
        m.receive(id,
                  testing::unsequenced(protocol::EnterOrder{.user_ref = ref,
                                                            .side = Side::Buy,
                                                            .qty = 100,
                                                            .symbol = aapl,
                                                            .price = 10'000,
                                                            .tif = TimeInForce::GoodTillCancel,
                                                            .type = OrderType::Limit,
                                                            .post_only = false}),
                  0);
    }
    EXPECT_EQ(m.order_entry().live_orders(), 4U);
    EXPECT_EQ(m.request_latency().count(), 2U);  // Orders 2 and 4.
}

}  // namespace
}  // namespace litbook::gateway
