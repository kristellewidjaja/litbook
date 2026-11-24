// Tests of the blocking order client against a real gateway in the same process. The
// gateway runs on its own thread and polls until the test stops it, so the client's
// blocking calls on the test thread get real answers. The test reads the gateway's state
// only after the thread has stopped.

#include "litbook/client/order_client.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/gateway/soup.hpp"

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#include "support/unwrap.hpp"

namespace litbook::client {
namespace {

using Manager = gateway::SessionManager<SortedOrderBook>;
using Gateway = gateway::EpollGateway<SortedOrderBook>;
using Clock = std::chrono::steady_clock;
using protocol::AcceptedMessage;
using protocol::CanceledMessage;
using protocol::ExecutedMessage;
using protocol::OutboundMessage;
using protocol::RejectedMessage;
using protocol::ReplacedMessage;
using protocol::WireCancelReason;
using protocol::WireRejectReason;

constexpr Price kPrice = 1'000'000;  // $100.00

Ticker aapl() {
    return testing::unwrap(make_ticker("AAPL"));
}

// A gateway with the accounts ALICE and BOB (password "pw") and the stock AAPL, served
// from a thread of its own.
class ServedExchange {
public:
    explicit ServedExchange(Qty max_order_qty = 1'000'000)
        : sessions_(gateway::SessionConfig{},
                    protocol::OrderEntryConfig{
                        .max_owners = 4, .max_live_orders = 1U << 12U, .max_symbols = 1}),
          gateway_(sessions_, gateway::GatewayConfig{.port = 0}) {
        protocol::RiskLimits limits;
        limits.max_order_qty = max_order_qty;
        for (const char* name : {"ALICE", "BOB"}) {
            (void)sessions_.add_account(gateway::AccountConfig{.username = name,
                                                               .password = "pw",
                                                               .cancel_on_disconnect = false,
                                                               .limits = limits});
        }
        (void)sessions_.add_symbol(aapl(), BookConfig{.tick_size = 100, .max_orders = 1U << 12U});
        server_ = std::thread([this] {
            while (!stop_.load(std::memory_order_acquire)) {
                (void)gateway_.poll_once(1);
            }
        });
    }

    ~ServedExchange() { stop(); }
    ServedExchange(const ServedExchange&) = delete;
    ServedExchange& operator=(const ServedExchange&) = delete;
    ServedExchange(ServedExchange&&) = delete;
    ServedExchange& operator=(ServedExchange&&) = delete;

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (server_.joinable()) {
            server_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const { return gateway_.port(); }

    [[nodiscard]] OrderClientConfig config(const char* user, int timeout_ms = 5000) const {
        OrderClientConfig c;
        c.port = port();
        c.username = user;
        c.password = "pw";
        c.timeout_ms = timeout_ms;
        return c;
    }

private:
    Manager sessions_;
    Gateway gateway_;
    std::atomic<bool> stop_{false};
    std::thread server_;
};

NewOrder limit(Side side, Qty qty, Price price = kPrice,
               TimeInForce tif = TimeInForce::GoodTillCancel) {
    return NewOrder{.symbol = aapl(),
                    .side = side,
                    .qty = qty,
                    .price = price,
                    .tif = tif,
                    .type = OrderType::Limit,
                    .post_only = false};
}

template <class T>
const T& reply_as(const Response& r) {
    if (r.outcome != Outcome::Answered || !r.reply) {
        throw std::logic_error(std::string("expected an answer, got ") + to_string(r.outcome));
    }
    const auto* m = std::get_if<T>(&*r.reply);
    if (m == nullptr) {
        throw std::logic_error("the answer has another message type");
    }
    return *m;
}

// Reads events until `want` of type T arrived or two seconds pass.
template <class T>
std::vector<T> wait_events(OrderClient& c, std::size_t want) {
    std::vector<T> got;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (got.size() < want && Clock::now() < deadline) {
        for (const OutboundMessage& m : c.events(50)) {
            if (const T* t = std::get_if<T>(&m)) {
                got.push_back(*t);
            }
        }
    }
    return got;
}

TEST(OrderClientTest, LogsInAndARestingOrderIsAccepted) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    EXPECT_TRUE(alice.active());
    EXPECT_EQ(alice.next_ref(), 1U);

    const Response r = alice.enter(limit(Side::Buy, 100));
    const auto& a = reply_as<AcceptedMessage>(r);
    EXPECT_EQ(r.user_ref, 1U);
    EXPECT_EQ(a.user_ref, 1U);
    EXPECT_EQ(a.qty, 100U);
    EXPECT_EQ(a.price, kPrice);
    EXPECT_EQ(alice.next_ref(), 2U);
    EXPECT_EQ(testing::unwrap_ptr(alice.orders().find(1)).open, 100U);
    EXPECT_EQ(alice.pending_events(), 0U);
}

TEST(OrderClientTest, RejectedOrdersAnswerTheEnterAndUseUpTheirNumber) {
    const ServedExchange ex(500);
    OrderClient alice(ex.config("ALICE"));

    const Response big = alice.enter(limit(Side::Buy, 501));
    EXPECT_EQ(reply_as<RejectedMessage>(big).reason, WireRejectReason::MaxQuantityExceeded);
    EXPECT_EQ(big.user_ref, 1U);

    NewOrder unlisted = limit(Side::Buy, 10);
    unlisted.symbol = testing::unwrap(make_ticker("MSFT"));
    EXPECT_EQ(reply_as<RejectedMessage>(alice.enter(unlisted)).reason,
              WireRejectReason::InvalidSymbol);

    const Response off_tick = alice.enter(limit(Side::Buy, 10, kPrice + 1));
    EXPECT_EQ(reply_as<RejectedMessage>(off_tick).reason, WireRejectReason::InvalidPrice);
    EXPECT_EQ(alice.next_ref(), 4U);
    EXPECT_EQ(alice.orders().open_count(), 0U);
}

TEST(OrderClientTest, APriceTheWireCannotCarryIsRefusedWithoutUsingANumber) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    EXPECT_EQ(alice.enter(limit(Side::Buy, 10, -100)).outcome, Outcome::Refused);
    EXPECT_EQ(alice.enter(limit(Side::Buy, 10, protocol::kMaxWirePrice + 100)).outcome,
              Outcome::Refused);
    EXPECT_EQ(alice.next_ref(), 1U);
    // The session is still fine afterwards.
    EXPECT_EQ(alice.enter(limit(Side::Buy, 10)).outcome, Outcome::Answered);
}

TEST(OrderClientTest, ExecutionsGoToTheEventsOfBothSides) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    OrderClient bob(ex.config("BOB"));
    const Response rest = alice.enter(limit(Side::Sell, 100));
    ASSERT_EQ(rest.outcome, Outcome::Answered);

    // Bob's IOC buy takes 60. Its answer is the Accepted, and the execution is an event.
    const Response take = bob.enter(limit(Side::Buy, 60, kPrice, TimeInForce::ImmediateOrCancel));
    EXPECT_EQ(reply_as<AcceptedMessage>(take).qty, 60U);
    const std::vector<ExecutedMessage> bob_fills = wait_events<ExecutedMessage>(bob, 1);
    ASSERT_EQ(bob_fills.size(), 1U);
    EXPECT_EQ(bob_fills[0].qty, 60U);
    EXPECT_EQ(bob_fills[0].liquidity, protocol::Liquidity::Removed);
    EXPECT_EQ(bob.orders().open_count(), 0U);

    const std::vector<ExecutedMessage> alice_fills = wait_events<ExecutedMessage>(alice, 1);
    ASSERT_EQ(alice_fills.size(), 1U);
    EXPECT_EQ(alice_fills[0].match, bob_fills[0].match);
    EXPECT_EQ(alice_fills[0].liquidity, protocol::Liquidity::Added);
    const OpenOrder& left = testing::unwrap_ptr(alice.orders().find(rest.user_ref));
    EXPECT_EQ(left.open, 40U);
    EXPECT_EQ(left.executed, 60U);
}

TEST(OrderClientTest, AnIocRemainderIsAnEventAndEndsTheOrder) {
    const ServedExchange ex;
    OrderClient bob(ex.config("BOB"));
    const Response r = bob.enter(limit(Side::Buy, 50, kPrice, TimeInForce::ImmediateOrCancel));
    ASSERT_EQ(r.outcome, Outcome::Answered);
    const std::vector<CanceledMessage> c = wait_events<CanceledMessage>(bob, 1);
    ASSERT_EQ(c.size(), 1U);
    EXPECT_EQ(c[0].reason, WireCancelReason::ImmediateOrCancel);
    EXPECT_EQ(c[0].qty, 50U);
    EXPECT_EQ(bob.orders().open_count(), 0U);
}

TEST(OrderClientTest, CancelOfAnOpenOrderIsAnswered) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    const Response rest = alice.enter(limit(Side::Buy, 100));
    const Response c = alice.cancel(rest.user_ref);
    const auto& m = reply_as<CanceledMessage>(c);
    EXPECT_EQ(m.qty, 100U);
    EXPECT_EQ(m.reason, WireCancelReason::UserRequested);
    EXPECT_EQ(c.user_ref, rest.user_ref);
    EXPECT_EQ(alice.orders().open_count(), 0U);
    // A cancel names an old number, so it uses none up.
    EXPECT_EQ(alice.next_ref(), 2U);
}

TEST(OrderClientTest, CancelOfAnOrderTheClientKnowsIsClosedSendsNothing) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    EXPECT_EQ(alice.cancel(42).outcome, Outcome::NotOpen);
    EXPECT_EQ(alice.replace(42, 10, kPrice).outcome, Outcome::NotOpen);
    EXPECT_EQ(alice.next_ref(), 1U);
}

// The exchange drops a cancel of an order that already filled without any reply. The
// fill's executions come first, so the client knows at once and does not wait for the
// timeout.
TEST(OrderClientTest, CancelOfAnOrderThatFilledOnItsWayIsGoneWithoutATimeout) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE", 10'000));
    OrderClient bob(ex.config("BOB"));
    const Response rest = alice.enter(limit(Side::Sell, 100));
    ASSERT_EQ(bob.enter(limit(Side::Buy, 100)).outcome, Outcome::Answered);
    ASSERT_EQ(wait_events<ExecutedMessage>(bob, 1).size(), 1U);
    // Alice has not read her execution yet, so her client still thinks the order is open.
    ASSERT_NE(alice.orders().find(rest.user_ref), nullptr);

    const auto begin = Clock::now();
    const Response c = alice.cancel(rest.user_ref);
    EXPECT_EQ(c.outcome, Outcome::Gone);
    EXPECT_LT(Clock::now() - begin, std::chrono::seconds(5));
    EXPECT_EQ(alice.orders().find(rest.user_ref), nullptr);
    // The execution that filled it is waiting in the events.
    const std::vector<ExecutedMessage> fills = wait_events<ExecutedMessage>(alice, 1);
    ASSERT_EQ(fills.size(), 1U);
    EXPECT_EQ(fills[0].qty, 100U);
}

TEST(OrderClientTest, ReplaceMovesTheOrderToItsNewNumber) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    const Response rest = alice.enter(limit(Side::Buy, 100));
    const Response r = alice.replace(rest.user_ref, 150, kPrice - 100);
    const auto& m = reply_as<ReplacedMessage>(r);
    EXPECT_EQ(r.user_ref, 2U);
    EXPECT_EQ(m.orig_user_ref, 1U);
    EXPECT_EQ(m.user_ref, 2U);
    EXPECT_EQ(m.qty, 150U);
    EXPECT_EQ(alice.orders().find(1), nullptr);
    const OpenOrder& o = testing::unwrap_ptr(alice.orders().find(2));
    EXPECT_EQ(o.open, 150U);
    EXPECT_EQ(o.price, kPrice - 100);
    EXPECT_EQ(alice.next_ref(), 3U);
}

TEST(OrderClientTest, ReplaceOfAnOrderThatFilledOnItsWayIsGone) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE", 10'000));
    OrderClient bob(ex.config("BOB"));
    const Response rest = alice.enter(limit(Side::Sell, 100));
    ASSERT_EQ(bob.enter(limit(Side::Buy, 100)).outcome, Outcome::Answered);
    ASSERT_EQ(wait_events<ExecutedMessage>(bob, 1).size(), 1U);
    const auto begin = Clock::now();
    EXPECT_EQ(alice.replace(rest.user_ref, 100, kPrice + 100).outcome, Outcome::Gone);
    EXPECT_LT(Clock::now() - begin, std::chrono::seconds(5));
    // The replace used up its number even though the exchange ignored it.
    EXPECT_EQ(alice.next_ref(), 3U);
}

// A replace that would leave nothing open fails. The exchange rejects the new number and
// then cancels the original order, and that cancel is an event.
TEST(OrderClientTest, AFailedReplaceIsRejectedAndTheOriginalCancelIsAnEvent) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    OrderClient bob(ex.config("BOB"));
    const Response rest = alice.enter(limit(Side::Sell, 100));
    ASSERT_EQ(bob.enter(limit(Side::Buy, 40)).outcome, Outcome::Answered);
    ASSERT_EQ(wait_events<ExecutedMessage>(alice, 1).size(), 1U);
    // 40 shares already executed, so a total of 40 leaves nothing open.
    const Response r = alice.replace(rest.user_ref, 40, kPrice);
    EXPECT_EQ(reply_as<RejectedMessage>(r).user_ref, 2U);
    const std::vector<CanceledMessage> c = wait_events<CanceledMessage>(alice, 1);
    ASSERT_EQ(c.size(), 1U);
    EXPECT_EQ(c[0].user_ref, rest.user_ref);
    EXPECT_EQ(c[0].qty, 60U);
    EXPECT_EQ(alice.orders().open_count(), 0U);
}

TEST(OrderClientTest, EventsWaitsForItsTimeoutWhenNothingArrives) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    const auto begin = Clock::now();
    EXPECT_TRUE(alice.events(50).empty());
    EXPECT_GE(Clock::now() - begin, std::chrono::milliseconds(50));
    EXPECT_TRUE(alice.events(0).empty());
}

// UserRefNums must rise for the whole day. A second connection that starts from 1 again
// is silently ignored by the exchange, so the client times out, and one that starts above
// the last number works.
TEST(OrderClientTest, ASecondConnectionMustStartAboveTheLastNumber) {
    const ServedExchange ex;
    protocol::UserRefNum next = 0;
    {
        OrderClient first(ex.config("ALICE"));
        ASSERT_EQ(first.enter(limit(Side::Buy, 10)).outcome, Outcome::Answered);
        next = first.next_ref();
        first.logout();
    }
    {
        OrderClient again(ex.config("ALICE", 200));
        EXPECT_EQ(again.enter(limit(Side::Buy, 10)).outcome, Outcome::TimedOut);
        again.logout();
    }
    OrderClientConfig c = ex.config("ALICE");
    c.first_ref = next + 1;  // The timed out request used up `next` on the client side.
    OrderClient third(c);
    EXPECT_EQ(third.enter(limit(Side::Buy, 10)).outcome, Outcome::Answered);
}

TEST(OrderClientTest, AWrongPasswordFailsTheLogin) {
    const ServedExchange ex;
    OrderClientConfig c = ex.config("ALICE");
    c.password = "nope";
    EXPECT_THROW(OrderClient{c}, std::runtime_error);
}

TEST(OrderClientTest, ASecondSessionOfTheSameAccountIsRefused) {
    const ServedExchange ex;
    const OrderClient alice(ex.config("ALICE"));
    EXPECT_THROW(OrderClient{ex.config("ALICE")}, std::runtime_error);
    EXPECT_TRUE(alice.active());
}

TEST(OrderClientTest, NoServerThrowsASystemError) {
    std::uint16_t port = 0;
    {
        const gateway::Fd listener = gateway::listen_tcp("127.0.0.1", 0, 1);
        port = gateway::local_port(listener.get());
    }
    OrderClientConfig c;
    c.port = port;
    c.username = "ALICE";
    c.password = "pw";
    EXPECT_THROW(OrderClient{c}, std::system_error);
}

TEST(OrderClientTest, BadConfigsAreRefused) {
    const ServedExchange ex;
    OrderClientConfig c = ex.config("ALICE");
    c.username = "TOOLONGNAME";
    EXPECT_THROW(OrderClient{c}, std::invalid_argument);
    c = ex.config("ALICE");
    c.first_ref = 0;
    EXPECT_THROW(OrderClient{c}, std::invalid_argument);
    c = ex.config("ALICE");
    c.timeout_ms = -1;
    EXPECT_THROW(OrderClient{c}, std::invalid_argument);
}

TEST(OrderClientTest, EveryCallReturnsClosedOnceTheServerIsGone) {
    auto ex = std::make_unique<ServedExchange>();
    OrderClient alice(ex->config("ALICE", 2000));
    const Response rest = alice.enter(limit(Side::Buy, 10));
    ASSERT_EQ(rest.outcome, Outcome::Answered);
    ex.reset();  // Stops the gateway and closes its sockets.
    EXPECT_EQ(alice.enter(limit(Side::Buy, 10)).outcome, Outcome::Closed);
    EXPECT_FALSE(alice.active());
    EXPECT_EQ(alice.failure(), loadgen::SessionFailure::Closed);
    EXPECT_EQ(alice.cancel(rest.user_ref).outcome, Outcome::Closed);
    EXPECT_TRUE(alice.events(0).empty());
}

// A server that accepts one connection, accepts its login, waits for the next bytes (the
// Logout Request), then waits 200 ms more before it closes. logout() promises the account
// is free when it returns, so it must take at least that long.
TEST(OrderClientTest, LogoutReturnsOnlyAfterTheServerClosedTheConnection) {
    const gateway::Fd listener = gateway::listen_tcp("127.0.0.1", 0, 1);
    const std::uint16_t port = gateway::local_port(listener.get());
    std::atomic<bool> logout_read{false};
    std::thread server([&] {
        pollfd p{.fd = listener.get(), .events = POLLIN, .revents = 0};
        if (::poll(&p, 1, 5'000) != 1) {
            return;
        }
        const gateway::Fd conn(::accept(listener.get(), nullptr, nullptr));
        if (!conn.valid()) {
            return;
        }
        std::array<std::byte, 256> in{};
        // Reads one burst of bytes, waiting up to five seconds for it.
        const auto read_some = [&] {
            pollfd q{.fd = conn.get(), .events = POLLIN, .revents = 0};
            return ::poll(&q, 1, 5'000) == 1 && ::recv(conn.get(), in.data(), in.size(), 0) > 0;
        };
        if (!read_some()) {  // The Login Request.
            return;
        }
        gateway::soup::LoginAccepted accepted;
        accepted.session.fill(' ');
        accepted.session.back() = '1';
        accepted.sequence = 1;
        std::array<std::byte, 64> out{};
        const std::size_t n = gateway::soup::encode_login_accepted(accepted, out);
        if (::send(conn.get(), out.data(), n, MSG_NOSIGNAL) != static_cast<ssize_t>(n)) {
            return;
        }
        if (read_some()) {  // The Logout Request.
            logout_read.store(true);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });  // conn closes when the lambda returns.

    OrderClientConfig c;
    c.port = port;
    c.username = "ALICE";
    c.password = "pw";
    {
        OrderClient alice(c);
        ASSERT_TRUE(alice.active());
        const auto begin = Clock::now();
        alice.logout();
        EXPECT_GE(Clock::now() - begin, std::chrono::milliseconds(150));
        EXPECT_EQ(alice.state(), loadgen::SessionState::Ended);
    }
    server.join();
    EXPECT_TRUE(logout_read.load());
}

TEST(OrderClientTest, LogoutEndsTheSessionAndLaterCallsAreClosed) {
    const ServedExchange ex;
    OrderClient alice(ex.config("ALICE"));
    alice.logout();
    EXPECT_EQ(alice.state(), loadgen::SessionState::Ended);
    EXPECT_EQ(alice.enter(limit(Side::Buy, 10)).outcome, Outcome::Closed);
    alice.logout();  // A second logout does nothing.
    // The account is free again.
    const OrderClient again(ex.config("ALICE"));
    EXPECT_TRUE(again.active());
}

}  // namespace
}  // namespace litbook::client
