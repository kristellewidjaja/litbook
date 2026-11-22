#include "litbook/gateway/session_manager.hpp"

#include "litbook/book/sorted_order_book.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

#include "book/test_limits.hpp"
#include "gateway/soup_test_support.hpp"
#include "support/unwrap.hpp"

namespace litbook::gateway {
namespace {

using testing::Bytes;
using testing::concat;
using testing::login;
using testing::parse_packets;
using testing::ServerPacket;
using testing::unsequenced;
using Manager = SessionManager<SortedOrderBook>;
using ConnectionId = Manager::ConnectionId;
using protocol::CancelOrder;
using protocol::EnterOrder;
using protocol::OutboundMessage;
using protocol::WireRejectReason;

constexpr Timestamp kSecond = 1'000'000'000;
constexpr Price kTick = 100;  // $0.01 with 4 implied decimals.

// alice has cancel on disconnect turned on, bob does not.
constexpr OwnerId kAlice = 1;
constexpr OwnerId kBob = 2;

SessionConfig small_config() {
    return SessionConfig{.session_name = "1",
                         .max_connections = 4,
                         .input_buffer_bytes = 64,
                         .output_buffer_bytes = 4096,
                         .output_high_water = 2048,
                         .heartbeat_interval = kSecond,
                         .idle_timeout = 15 * kSecond,
                         .login_timeout = 30 * kSecond};
}

protocol::OrderEntryConfig entry_config() {
    return {.max_owners = 4, .max_live_orders = 1024, .max_symbols = 4};
}

std::unique_ptr<Manager> make_manager(const SessionConfig& config = small_config()) {
    auto m = std::make_unique<Manager>(config, entry_config());
    m->add_account({.username = "alice", .password = "secret", .cancel_on_disconnect = true});
    m->add_account({.username = "bob", .password = "hunter2"});
    m->add_symbol(litbook::testing::unwrap(make_ticker("AAPL")),
                  litbook::testing::with_test_limits(BookConfig{.tick_size = kTick}));
    return m;
}

EnterOrder buy(protocol::UserRefNum ref, Qty qty, Price price) {
    return EnterOrder{.user_ref = ref,
                      .side = Side::Buy,
                      .qty = qty,
                      .symbol = litbook::testing::unwrap(make_ticker("AAPL")),
                      .price = price,
                      .tif = TimeInForce::GoodTillCancel,
                      .type = OrderType::Limit,
                      .post_only = false};
}

EnterOrder sell(protocol::UserRefNum ref, Qty qty, Price price) {
    EnterOrder e = buy(ref, qty, price);
    e.side = Side::Sell;
    return e;
}

class SessionManagerTest : public ::testing::Test {
protected:
    SessionManagerTest() : m_(make_manager()) {}

    // Takes everything the server queued for a connection, as if the network code had
    // written it all to the socket.
    std::vector<ServerPacket> take(ConnectionId id, Timestamp now = 0) {
        const auto out = m_->output(id);
        auto packets = parse_packets(out);
        m_->sent(id, out.size(), now);
        return packets;
    }

    // Opens a connection and logs it in, and checks that the login worked.
    ConnectionId logged_in(std::string_view user, std::string_view password, Timestamp now = 0) {
        const ConnectionId id = m_->open(now);
        m_->receive(id, login(user, password), now);
        const auto packets = take(id);
        EXPECT_EQ(packets.size(), 1U);
        if (!packets.empty()) {
            EXPECT_EQ(packets[0].type, soup::PacketType::LoginAccepted);
        }
        return id;
    }

    // The OUCH messages among the packets.
    static std::vector<OutboundMessage> messages(const std::vector<ServerPacket>& packets) {
        std::vector<OutboundMessage> out;
        for (const ServerPacket& p : packets) {
            if (const auto m = p.message()) {
                out.push_back(*m);
            }
        }
        return out;
    }

    std::unique_ptr<Manager> m_;
};

soup::LoginAccepted accepted_body(const ServerPacket& p) {
    EXPECT_EQ(p.type, soup::PacketType::LoginAccepted);
    return litbook::testing::unwrap(soup::decode_login_accepted(p.payload));
}

soup::LoginRejectReason rejected_reason(const ServerPacket& p) {
    EXPECT_EQ(p.type, soup::PacketType::LoginRejected);
    return litbook::testing::unwrap(soup::decode_login_rejected(p.payload));
}

// Logging in

TEST_F(SessionManagerTest, LoginIsAcceptedAndStartsAtSequenceOne) {
    const ConnectionId id = m_->open(0);
    EXPECT_EQ(m_->open_connections(), 1U);
    m_->receive(id, login("alice", "secret"), 5);
    const auto packets = take(id);
    ASSERT_EQ(packets.size(), 1U);
    const soup::LoginAccepted body = accepted_body(packets[0]);
    EXPECT_EQ(soup::trimmed(body.session), "1");
    EXPECT_EQ(body.sequence, 1U);
    EXPECT_EQ(m_->session(id).state(), SessionState::LoggedIn);
    EXPECT_EQ(m_->session(id).owner(), kAlice);
    EXPECT_EQ(m_->connection_of(kAlice), id);
    EXPECT_EQ(m_->stats().logins_accepted, 1U);
    EXPECT_FALSE(m_->should_close(id));
}

TEST_F(SessionManagerTest, UsernameAndPasswordIgnoreCase) {
    logged_in("ALICE", "SeCrEt");
    EXPECT_EQ(m_->stats().logins_accepted, 1U);
}

TEST_F(SessionManagerTest, TheSessionNameMayBeGivenOrLeftBlank) {
    const ConnectionId a = m_->open(0);
    m_->receive(a, login("alice", "secret", "1"), 0);
    EXPECT_EQ(take(a)[0].type, soup::PacketType::LoginAccepted);
    const ConnectionId b = m_->open(0);
    m_->receive(b, login("bob", "hunter2", ""), 0);
    EXPECT_EQ(take(b)[0].type, soup::PacketType::LoginAccepted);
}

TEST_F(SessionManagerTest, WrongPasswordOrUnknownUserIsNotAuthorized) {
    for (const auto& [user, password] : {std::pair{"alice", "wrong"}, std::pair{"carol", "x"}}) {
        const ConnectionId id = m_->open(0);
        m_->receive(id, login(user, password), 0);
        const auto packets = take(id);
        ASSERT_EQ(packets.size(), 1U);
        EXPECT_EQ(rejected_reason(packets[0]), soup::LoginRejectReason::NotAuthorized);
        EXPECT_TRUE(m_->should_close(id));
        EXPECT_EQ(m_->session(id).close_reason(), CloseReason::LoginRejected);
        m_->close(id, CloseReason::PeerClosed, 0);
    }
    EXPECT_EQ(m_->stats().logins_rejected, 2U);
    EXPECT_EQ(m_->stats().closed(CloseReason::LoginRejected), 2U);
    EXPECT_EQ(m_->connection_of(kAlice), Manager::kNoConnection);
}

TEST_F(SessionManagerTest, AnotherSessionNameIsNotAvailable) {
    const ConnectionId id = m_->open(0);
    m_->receive(id, login("alice", "secret", "2"), 0);
    EXPECT_EQ(rejected_reason(take(id).at(0)), soup::LoginRejectReason::SessionNotAvailable);
}

TEST_F(SessionManagerTest, AnAccountCanHaveOnlyOneConnection) {
    const ConnectionId first = logged_in("alice", "secret");
    const ConnectionId second = m_->open(0);
    m_->receive(second, login("alice", "secret"), 0);
    EXPECT_EQ(rejected_reason(take(second).at(0)), soup::LoginRejectReason::SessionNotAvailable);
    m_->close(second, CloseReason::PeerClosed, 0);
    // The first connection is untouched, and once it is gone the account can log in again.
    EXPECT_EQ(m_->connection_of(kAlice), first);
    EXPECT_FALSE(m_->should_close(first));
    m_->close(first, CloseReason::PeerClosed, 0);
    logged_in("alice", "secret");
}

TEST_F(SessionManagerTest, ARestoredSequenceIsWhatTheNextLoginAndReplyUse) {
    m_->restore_next_sequence(kBob, 41);
    EXPECT_EQ(m_->next_sequence(kBob), 41U);
    const ConnectionId id = m_->open(0);
    m_->receive(id, login("bob", "hunter2"), 0);
    const auto packets = take(id);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(accepted_body(packets[0]).sequence, 41U);
    m_->receive(id, unsequenced(buy(1, 10, 500'000)), 1);
    EXPECT_EQ(messages(take(id)).size(), 1U);
    EXPECT_EQ(m_->next_sequence(kBob), 42U);
    EXPECT_EQ(m_->next_sequence(kAlice), 1U);  // Other accounts are left alone.
}

TEST_F(SessionManagerTest, AfterARestoreALoginAskingForAnOlderMessageIsRefused) {
    m_->restore_next_sequence(kBob, 10);
    const ConnectionId id = m_->open(0);
    m_->receive(id, login("bob", "hunter2", "", 9), 0);
    const auto packets = take(id);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(rejected_reason(packets[0]), soup::LoginRejectReason::SessionNotAvailable);
    const ConnectionId again = m_->open(0);
    m_->receive(again, login("bob", "hunter2", "", 10), 0);
    EXPECT_EQ(accepted_body(take(again).at(0)).sequence, 10U);
}

TEST_F(SessionManagerTest, RestoringASequenceChecksItsArguments) {
    EXPECT_THROW(m_->restore_next_sequence(kBob, 0), std::invalid_argument);
    EXPECT_THROW(m_->restore_next_sequence(kNoOwner, 5), std::out_of_range);
    EXPECT_THROW(m_->restore_next_sequence(99, 5), std::out_of_range);
    (void)logged_in("bob", "hunter2");
    EXPECT_THROW(m_->restore_next_sequence(kBob, 5), std::logic_error);
    EXPECT_EQ(m_->next_sequence(kBob), 1U);
}

TEST_F(SessionManagerTest, AccountsAndTheirCancelOnDisconnectCanBeListed) {
    EXPECT_EQ(m_->account_count(), 2U);
    EXPECT_THROW((void)m_->cancels_on_disconnect(kNoOwner), std::out_of_range);
    EXPECT_THROW((void)m_->cancels_on_disconnect(3), std::out_of_range);
    EXPECT_TRUE(m_->cancels_on_disconnect(kAlice));
    EXPECT_FALSE(m_->cancels_on_disconnect(kBob));
}

TEST_F(SessionManagerTest, SequenceNumbersContinueAcrossConnections) {
    ConnectionId id = logged_in("bob", "hunter2");
    m_->receive(id, unsequenced(buy(1, 10, 500'000)), 1);
    m_->receive(id, unsequenced(buy(2, 10, 490'000)), 2);
    EXPECT_EQ(messages(take(id)).size(), 2U);
    EXPECT_EQ(m_->next_sequence(kBob), 3U);
    m_->close(id, CloseReason::PeerClosed, 3);

    // Asking for a message that was already sent would need a replay, which comes with
    // the journal in Phase 9.
    id = m_->open(4);
    m_->receive(id, login("bob", "hunter2", "", 2), 4);
    EXPECT_EQ(rejected_reason(take(id).at(0)), soup::LoginRejectReason::SessionNotAvailable);
    m_->close(id, CloseReason::PeerClosed, 4);

    // Asking for 0 (from now on) or for exactly the next number works.
    for (const std::uint64_t wanted : {0U, 3U}) {
        id = m_->open(5);
        m_->receive(id, login("bob", "hunter2", "", wanted), 5);
        EXPECT_EQ(accepted_body(take(id).at(0)).sequence, 3U) << wanted;
        m_->close(id, CloseReason::Logout, 5);
    }

    // Asking for a number beyond the next one is accepted, and Login Accepted tells the
    // client the real next number.
    id = m_->open(6);
    m_->receive(id, login("bob", "hunter2", "", 99), 6);
    EXPECT_EQ(accepted_body(take(id).at(0)).sequence, 3U);
}

// Orders through the session

TEST_F(SessionManagerTest, OrdersFlowBothWays) {
    const ConnectionId a = logged_in("alice", "secret");
    const ConnectionId b = logged_in("bob", "hunter2");
    m_->receive(a, unsequenced(buy(1, 10, 500'000)), 10);
    m_->receive(b, unsequenced(sell(1, 4, 500'000)), 11);
    const auto to_alice = messages(take(a));
    const auto to_bob = messages(take(b));
    ASSERT_EQ(to_alice.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<protocol::AcceptedMessage>(to_alice[0]));
    const auto& fill = std::get<protocol::ExecutedMessage>(to_alice[1]);
    EXPECT_EQ(fill.qty, 4U);
    EXPECT_EQ(fill.timestamp, 11U);  // The time of the message that caused it.
    ASSERT_EQ(to_bob.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<protocol::AcceptedMessage>(to_bob[0]));
    EXPECT_TRUE(std::holds_alternative<protocol::ExecutedMessage>(to_bob[1]));
    EXPECT_EQ(m_->next_sequence(kAlice), 3U);
    EXPECT_EQ(m_->next_sequence(kBob), 3U);
    EXPECT_EQ(m_->stats().messages_dropped, 0U);
}

TEST_F(SessionManagerTest, AccountRiskLimitsApply) {
    Manager m(small_config(), entry_config());
    m.add_account({.username = "carol", .password = "pw", .limits = {.max_order_qty = 100}});
    m.add_symbol(litbook::testing::unwrap(make_ticker("AAPL")),
                 litbook::testing::with_test_limits(BookConfig{.tick_size = kTick}));
    const ConnectionId id = m.open(0);
    m.receive(id, concat({login("carol", "pw"), unsequenced(buy(1, 101, 500'000))}), 0);
    const auto packets = parse_packets(m.output(id));
    ASSERT_EQ(packets.size(), 2U);
    const auto reject =
        std::get<protocol::RejectedMessage>(litbook::testing::unwrap(packets[1].message()));
    EXPECT_EQ(reject.reason, WireRejectReason::MaxQuantityExceeded);
}

// Malformed input closes the connection

TEST_F(SessionManagerTest, MalformedOuchMessageCloses) {
    const ConnectionId id = logged_in("alice", "secret");
    Bytes frame = unsequenced(buy(1, 10, 500'000));
    frame[soup::kHeaderSize + 5] = std::byte{'X'};  // The side byte of the Enter Order.
    m_->receive(id, concat({frame, unsequenced(buy(2, 10, 500'000))}), 0);
    EXPECT_TRUE(m_->should_close(id));
    EXPECT_EQ(m_->session(id).close_reason(), CloseReason::MalformedMessage);
    EXPECT_EQ(m_->order_entry().stats().malformed, 1U);
    // The order after the bad one was never handled.
    EXPECT_TRUE(take(id).empty());
    EXPECT_EQ(m_->order_entry().live_orders(), 0U);
}

TEST_F(SessionManagerTest, ZeroLengthCloses) {
    const ConnectionId id = logged_in("alice", "secret");
    m_->receive(id, Bytes{std::byte{0}, std::byte{0}}, 0);
    EXPECT_EQ(m_->session(id).close_reason(), CloseReason::BadLength);
}

TEST_F(SessionManagerTest, TooLongPacketClosesBeforeItsBytesArrive) {
    const ConnectionId id = logged_in("alice", "secret");
    const auto too_long = static_cast<std::byte>(Manager::kMaxClientPacket + 1);
    m_->receive(id, Bytes{std::byte{0}, too_long}, 0);
    EXPECT_EQ(m_->session(id).close_reason(), CloseReason::BadLength);
}

TEST_F(SessionManagerTest, ServerPacketTypesFromAClientClose) {
    // Two packet types only a server may send, and a type that does not exist at all.
    const Bytes unknown{std::byte{0}, std::byte{1}, std::byte{'Q'}};
    for (const Bytes& bad : {testing::packet(soup::PacketType::SequencedData),
                             testing::packet(soup::PacketType::ServerHeartbeat), unknown}) {
        auto m = make_manager();
        const ConnectionId id = m->open(0);
        m->receive(id, concat({login("alice", "secret"), bad}), 0);
        EXPECT_EQ(m->session(id).close_reason(), CloseReason::UnknownPacket);
    }
}

TEST_F(SessionManagerTest, AnythingButALoginFirstCloses) {
    for (const Bytes& first :
         {unsequenced(buy(1, 10, 500'000)), testing::heartbeat(), testing::logout()}) {
        auto m = make_manager();
        const ConnectionId id = m->open(0);
        m->receive(id, first, 0);
        EXPECT_EQ(m->session(id).close_reason(), CloseReason::NotLoggedIn);
        EXPECT_TRUE(parse_packets(m->output(id)).empty());
    }
}

TEST_F(SessionManagerTest, DebugPacketsAreIgnoredEvenBeforeLogin) {
    const ConnectionId id = m_->open(0);
    m_->receive(
        id, concat({testing::debug("hello"), login("alice", "secret"), testing::debug("x")}), 0);
    EXPECT_EQ(take(id).size(), 1U);
    EXPECT_EQ(m_->session(id).state(), SessionState::LoggedIn);
}

TEST_F(SessionManagerTest, SecondLoginOnTheSameConnectionCloses) {
    const ConnectionId id = logged_in("alice", "secret");
    m_->receive(id, login("alice", "secret"), 0);
    EXPECT_EQ(m_->session(id).close_reason(), CloseReason::UnexpectedLogin);
}

TEST_F(SessionManagerTest, LoginPacketWithTheWrongLengthOrABadNumberCloses) {
    const Bytes short_login =
        testing::packet(soup::PacketType::LoginRequest, Bytes(20, std::byte{' '}));
    Bytes bad_number = login("alice", "secret");
    bad_number.back() = std::byte{'z'};
    for (const Bytes& bad : {short_login, bad_number}) {
        auto m = make_manager();
        const ConnectionId id = m->open(0);
        m->receive(id, bad, 0);
        EXPECT_EQ(m->session(id).close_reason(), CloseReason::BadLogin);
        EXPECT_TRUE(parse_packets(m->output(id)).empty());
    }
}

TEST_F(SessionManagerTest, LogoutCloses) {
    const ConnectionId id = logged_in("alice", "secret");
    m_->receive(id, testing::logout(), 0);
    EXPECT_EQ(m_->session(id).close_reason(), CloseReason::Logout);
    // Input after the decision to close is ignored.
    m_->receive(id, unsequenced(buy(1, 10, 500'000)), 0);
    EXPECT_EQ(m_->order_entry().live_orders(), 0U);
}

// TCP may cut the byte stream anywhere. Delivering the same bytes in any number of
// pieces must give exactly the same output as delivering them at once.

Bytes sample_stream() {
    return concat({testing::debug("hi"), login("alice", "secret"), unsequenced(buy(1, 10, 500'000)),
                   testing::heartbeat(), unsequenced(sell(2, 3, 600'000)),
                   unsequenced(CancelOrder{.user_ref = 1, .qty = 4}),
                   unsequenced(buy(3, 5, 600'000))});
}

Bytes output_after(const std::vector<std::span<const std::byte>>& pieces) {
    auto m = make_manager();
    const ConnectionId id = m->open(0);
    for (const auto piece : pieces) {
        m->receive(id, piece, 0);
    }
    EXPECT_FALSE(m->should_close(id));
    const auto out = m->output(id);
    return {out.begin(), out.end()};
}

TEST(SessionManagerSplits, EveryTwoWaySplitGivesTheSameOutput) {
    const Bytes stream = sample_stream();
    const std::span<const std::byte> all(stream);
    const Bytes expected = output_after({all});
    // Login Accepted, then Accepted, Accepted, Canceled (the reduce), and for the last
    // buy Accepted and a self match cancel, because it would trade with alice's own sell.
    EXPECT_EQ(parse_packets(expected).size(), 6U);
    for (std::size_t cut = 1; cut < stream.size(); ++cut) {
        EXPECT_EQ(output_after({all.first(cut), all.subspan(cut)}), expected) << "cut at " << cut;
    }
}

TEST(SessionManagerSplits, OneByteAtATimeGivesTheSameOutput) {
    const Bytes stream = sample_stream();
    const std::span<const std::byte> all(stream);
    std::vector<std::span<const std::byte>> pieces;
    pieces.reserve(stream.size());
    for (std::size_t i = 0; i < stream.size(); ++i) {
        pieces.push_back(all.subspan(i, 1));
    }
    EXPECT_EQ(output_after(pieces), output_after({all}));
}

// n Enter Orders from one account, all resting at different prices.
Bytes many_orders(protocol::UserRefNum n) {
    Bytes out;
    for (protocol::UserRefNum ref = 1; ref <= n; ++ref) {
        const Bytes p = unsequenced(buy(ref, 10, 400'000 + ref * kTick));
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

TEST_F(SessionManagerTest, OneReceiveMayBeMuchLargerThanTheInputBuffer) {
    const ConnectionId id = logged_in("alice", "secret");
    const Bytes orders = many_orders(40);  // 40 replies of 48 bytes stay below high water.
    ASSERT_GT(orders.size(), small_config().input_buffer_bytes * 10);
    EXPECT_EQ(m_->receive(id, orders, 0), orders.size());
    EXPECT_FALSE(m_->should_close(id));
    EXPECT_EQ(messages(take(id)).size(), 40U);
    EXPECT_EQ(m_->order_entry().live_orders(), 40U);
}

TEST_F(SessionManagerTest, BackedUpOutputPausesInputUntilItIsWritten) {
    const ConnectionId id = logged_in("alice", "secret");
    const Bytes orders = many_orders(200);  // 200 replies are far more than high water.
    std::span<const std::byte> rest(orders);
    std::size_t replies = 0;
    int rounds = 0;
    while (!rest.empty() && rounds < 1000) {
        ++rounds;
        const std::size_t taken = m_->receive(id, rest, 0);
        rest = rest.subspan(taken);
        // While paused the output stays below the whole buffer, so nothing was lost.
        EXPECT_LE(m_->output(id).size(), small_config().output_buffer_bytes);
        replies += messages(take(id)).size();  // Writing the output resumes the input.
    }
    replies += messages(take(id)).size();
    EXPECT_GT(rounds, 2);  // It really did pause.
    EXPECT_FALSE(m_->should_close(id));
    EXPECT_EQ(replies, 200U);
    EXPECT_EQ(m_->order_entry().live_orders(), 200U);
}

TEST_F(SessionManagerTest, ReceiveRoomIsZeroWhilePausedWithAFullInputBuffer) {
    const ConnectionId id = logged_in("alice", "secret");
    EXPECT_EQ(m_->receive_room(id), small_config().input_buffer_bytes);
    const Bytes orders = many_orders(200);
    const std::size_t taken = m_->receive(id, orders, 0);
    EXPECT_LT(taken, orders.size());
    EXPECT_EQ(m_->receive_room(id), 0U);
    EXPECT_EQ(m_->receive(id, std::span(orders).subspan(taken), 0), 0U);
    take(id);
    EXPECT_GT(m_->receive_room(id), 0U);
    EXPECT_EQ(m_->receive_room(Manager::kNoConnection), 0U);
}

// Heartbeats and timeouts

TEST_F(SessionManagerTest, ServerSendsAHeartbeatAfterOneQuietSecond) {
    const ConnectionId id = logged_in("alice", "secret", 0);
    m_->tick(kSecond - 1);
    EXPECT_TRUE(take(id).empty());
    m_->tick(kSecond);
    auto packets = take(id);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].type, soup::PacketType::ServerHeartbeat);
    // Any packet resets the clock, so a reply at 1.5 s means no heartbeat until 2.5 s.
    m_->receive(id, unsequenced(buy(1, 10, 500'000)), kSecond + kSecond / 2);
    take(id);
    m_->tick(2 * kSecond);
    EXPECT_TRUE(take(id).empty());
    m_->tick(2 * kSecond + kSecond / 2);
    EXPECT_EQ(take(id).size(), 1U);
    EXPECT_EQ(m_->stats().heartbeats_out, 2U);
}

TEST_F(SessionManagerTest, NoHeartbeatsBeforeLogin) {
    const ConnectionId id = m_->open(0);
    m_->tick(5 * kSecond);
    EXPECT_TRUE(take(id).empty());
}

TEST_F(SessionManagerTest, SilentClientIsDisconnectedAndHeartbeatsKeepItAlive) {
    const ConnectionId quiet = logged_in("alice", "secret", 0);
    const ConnectionId chatty = logged_in("bob", "hunter2", 0);
    for (Timestamp t = kSecond; t <= 15 * kSecond; t += kSecond) {
        m_->receive(chatty, testing::heartbeat(), t);
        m_->tick(t);
    }
    EXPECT_EQ(m_->session(quiet).close_reason(), CloseReason::IdleTimeout);
    EXPECT_FALSE(m_->should_close(chatty));
}

TEST_F(SessionManagerTest, ConnectionThatNeverLogsInTimesOut) {
    const ConnectionId id = m_->open(0);
    m_->tick(30 * kSecond - 1);
    EXPECT_FALSE(m_->should_close(id));
    m_->tick(30 * kSecond);
    EXPECT_EQ(m_->session(id).close_reason(), CloseReason::LoginTimeout);
}

// Disconnects

TEST_F(SessionManagerTest, CancelOnDisconnectCancelsOnlyThatAccountsOrders) {
    const ConnectionId a = logged_in("alice", "secret");
    const ConnectionId b = logged_in("bob", "hunter2");
    m_->receive(a, concat({unsequenced(buy(1, 10, 500'000)), unsequenced(buy(2, 10, 490'000))}), 0);
    m_->receive(b, unsequenced(sell(1, 10, 600'000)), 0);
    take(a);
    take(b);
    ASSERT_EQ(m_->order_entry().live_orders(), 3U);
    EXPECT_EQ(m_->next_sequence(kAlice), 3U);

    m_->close(a, CloseReason::PeerClosed, 7);
    EXPECT_EQ(m_->order_entry().open_orders(kAlice), 0U);
    EXPECT_EQ(m_->order_entry().open_orders(kBob), 1U);
    // The two cancels were produced for alice and numbered, but nobody could take them.
    EXPECT_EQ(m_->next_sequence(kAlice), 5U);
    EXPECT_EQ(m_->stats().messages_dropped, 2U);
    EXPECT_EQ(m_->stats().closed(CloseReason::PeerClosed), 1U);

    // bob did not ask for cancel on disconnect, so his order stays.
    m_->close(b, CloseReason::PeerClosed, 8);
    EXPECT_EQ(m_->order_entry().open_orders(kBob), 1U);
}

TEST_F(SessionManagerTest, MessagesForAnAccountWithNoConnectionAreNumberedAndDropped) {
    const ConnectionId b = logged_in("bob", "hunter2");
    m_->receive(b, unsequenced(sell(1, 10, 500'000)), 0);
    take(b);
    m_->close(b, CloseReason::PeerClosed, 0);
    const ConnectionId a = logged_in("alice", "secret");
    m_->receive(a, unsequenced(buy(1, 10, 500'000)), 0);  // Trades with bob's resting order.
    EXPECT_EQ(m_->next_sequence(kBob), 3U);
    EXPECT_EQ(m_->stats().messages_dropped, 1U);
}

TEST(SessionManagerSlowClient, ClientThatStopsReadingIsDisconnected) {
    SessionConfig config = small_config();
    config.output_buffer_bytes = 200;
    config.output_high_water = 100;
    auto m = make_manager(config);
    const ConnectionId a = m->open(0);
    const ConnectionId b = m->open(0);
    m->receive(b, login("bob", "hunter2"), 0);
    // alice logs in (33 bytes of output) and rests two buys (48 bytes each), which takes
    // her past high water. She never reads, so her own later orders just wait.
    m->receive(a, concat({login("alice", "secret"), many_orders(5)}), 0);
    EXPECT_FALSE(m->should_close(a));
    EXPECT_EQ(m->order_entry().live_orders(), 2U);
    // bob's sells trade with her buys, and each execution report for alice must be
    // queued. Once one no longer fits, she is disconnected.
    protocol::UserRefNum ref = 0;
    while (!m->should_close(a) && ref < 20) {
        ++ref;
        m->receive(b, unsequenced(sell(ref, 1, 400'000)), 0);
        const auto out = m->output(b);
        m->sent(b, out.size(), 0);
    }
    EXPECT_EQ(m->session(a).close_reason(), CloseReason::OutputFull);
    EXPECT_FALSE(m->should_close(b));
    EXPECT_GT(m->stats().messages_dropped, 0U);
    // What is queued for alice is still whole packets, so a client could read them all.
    EXPECT_GE(parse_packets(m->output(a)).size(), 4U);
}

TEST_F(SessionManagerTest, ConnectionLimit) {
    std::vector<ConnectionId> ids;
    for (int i = 0; i < 4; ++i) {
        ids.push_back(m_->open(0));
        EXPECT_NE(ids.back(), Manager::kNoConnection);
    }
    EXPECT_EQ(m_->open(0), Manager::kNoConnection);
    EXPECT_EQ(m_->stats().connections_refused, 1U);
    m_->close(ids[2], CloseReason::PeerClosed, 0);
    EXPECT_EQ(m_->open_connections(), 3U);
    EXPECT_EQ(m_->open(0), ids[2]);  // The freed slot is reused.
}

TEST_F(SessionManagerTest, EndOfSession) {
    const ConnectionId a = logged_in("alice", "secret");
    const ConnectionId waiting = m_->open(0);
    m_->end_session(9);
    const auto packets = take(a);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].type, soup::PacketType::EndOfSession);
    EXPECT_EQ(m_->session(a).close_reason(), CloseReason::EndOfSession);
    EXPECT_EQ(m_->session(waiting).close_reason(), CloseReason::EndOfSession);
    EXPECT_TRUE(take(waiting).empty());
    m_->close(a, CloseReason::PeerClosed, 10);
    m_->close(waiting, CloseReason::PeerClosed, 10);
    // Nobody can log in to a session that has ended.
    const ConnectionId late = m_->open(11);
    m_->receive(late, login("bob", "hunter2"), 11);
    EXPECT_EQ(rejected_reason(take(late).at(0)), soup::LoginRejectReason::SessionNotAvailable);
}

TEST_F(SessionManagerTest, CallsWithClosedOrUnknownIdsDoNothing) {
    const ConnectionId id = m_->open(0);
    m_->close(id, CloseReason::PeerClosed, 0);
    for (const ConnectionId bad : {id, ConnectionId{3}, Manager::kNoConnection}) {
        m_->receive(bad, login("alice", "secret"), 0);
        EXPECT_TRUE(m_->output(bad).empty());
        EXPECT_FALSE(m_->should_close(bad));
        m_->sent(bad, 5, 0);
        m_->close(bad, CloseReason::PeerClosed, 0);
    }
    EXPECT_EQ(m_->stats().closed(CloseReason::PeerClosed), 1U);
    EXPECT_EQ(m_->open_connections(), 0U);
}

TEST_F(SessionManagerTest, CloseKeepsTheReasonTheSessionChose) {
    const ConnectionId id = logged_in("alice", "secret");
    m_->receive(id, testing::logout(), 0);
    m_->close(id, CloseReason::PeerClosed, 0);
    EXPECT_EQ(m_->stats().closed(CloseReason::Logout), 1U);
    EXPECT_EQ(m_->stats().closed(CloseReason::PeerClosed), 0U);
}

// Startup checks

TEST(SessionManagerConfig, BadConfigsThrow) {
    const auto entry = entry_config();
    auto with = [](auto change) {
        SessionConfig c = small_config();
        change(c);
        return c;
    };
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.session_name = ""; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.session_name = "elevenchars"; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.session_name = "a b"; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.max_connections = 0; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.input_buffer_bytes = 48; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.output_buffer_bytes = 49; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.output_high_water = 0; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.output_high_water = 4096 - 49; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.heartbeat_interval = 0; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.idle_timeout = 0; }), entry),
                 std::invalid_argument);
    EXPECT_THROW(Manager(with([](SessionConfig& c) { c.login_timeout = 0; }), entry),
                 std::invalid_argument);
    // The smallest buffers that hold one packet are fine.
    EXPECT_NO_THROW(Manager(with([](SessionConfig& c) {
                                c.input_buffer_bytes = 49;
                                c.output_buffer_bytes = 51;
                                c.output_high_water = 1;
                            }),
                            entry));
}

TEST(SessionManagerConfig, BadAccountsThrow) {
    Manager m(small_config(), {.max_owners = 2, .max_live_orders = 16, .max_symbols = 1});
    EXPECT_THROW(m.add_account({.username = "", .password = "pw"}), std::invalid_argument);
    EXPECT_THROW(m.add_account({.username = "sevench", .password = "pw"}), std::invalid_argument);
    EXPECT_THROW(m.add_account({.username = "a-b", .password = "pw"}), std::invalid_argument);
    EXPECT_THROW(m.add_account({.username = "ann", .password = ""}), std::invalid_argument);
    EXPECT_THROW(m.add_account({.username = "ann", .password = "elevenchars"}),
                 std::invalid_argument);
    EXPECT_EQ(m.add_account({.username = "ann", .password = "pw"}), 1U);
    EXPECT_THROW(m.add_account({.username = "ANN", .password = "pw"}), std::invalid_argument);
    EXPECT_EQ(m.add_account({.username = "ben", .password = "pw"}), 2U);
    EXPECT_THROW(m.add_account({.username = "cy", .password = "pw"}), std::length_error);
    EXPECT_THROW((void)m.next_sequence(3), std::out_of_range);
    EXPECT_THROW((void)m.next_sequence(0), std::out_of_range);
}

}  // namespace
}  // namespace litbook::gateway
