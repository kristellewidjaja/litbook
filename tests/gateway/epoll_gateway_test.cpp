#include "litbook/gateway/epoll_gateway.hpp"

#include "litbook/book/sorted_order_book.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "book/test_limits.hpp"
#include "gateway/soup_test_support.hpp"
#include "gateway/test_client.hpp"
#include "support/unwrap.hpp"

namespace litbook::gateway {
namespace {

using testing::Bytes;
using testing::concat;
using testing::login;
using testing::ServerPacket;
using testing::TestClient;
using testing::unsequenced;
using Manager = SessionManager<SortedOrderBook>;
using Gateway = EpollGateway<SortedOrderBook>;
using protocol::EnterOrder;
using protocol::OutboundMessage;

constexpr Timestamp kSecond = 1'000'000'000;

// The tests control time through this clock, so timeouts need no real waiting.
std::atomic<Timestamp> g_fake_now{0};
Timestamp fake_clock() {
    return g_fake_now.load();
}

EnterOrder order(Side side, protocol::UserRefNum ref, Qty qty, Price price) {
    return EnterOrder{.user_ref = ref,
                      .side = side,
                      .qty = qty,
                      .symbol = litbook::testing::unwrap(make_ticker("AAPL")),
                      .price = price,
                      .tif = TimeInForce::GoodTillCancel,
                      .type = OrderType::Limit,
                      .post_only = false};
}

// A client plus every packet it has received so far, copied out of its buffer.
struct Peer {
    std::unique_ptr<TestClient> client;
    std::vector<ServerPacket> packets;

    void collect() {
        client->read_available();
        for (;;) {
            const soup::Packet p = client->next_packet();
            if (p.status != soup::PacketStatus::Complete) {
                return;
            }
            packets.push_back(
                ServerPacket{.type = static_cast<soup::PacketType>(static_cast<char>(p.type)),
                             .payload = Bytes(p.payload.begin(), p.payload.end())});
        }
    }

    [[nodiscard]] std::size_t count(soup::PacketType type) const {
        std::size_t n = 0;
        for (const ServerPacket& p : packets) {
            n += p.type == type ? 1 : 0;
        }
        return n;
    }

    [[nodiscard]] std::vector<OutboundMessage> messages() const {
        std::vector<OutboundMessage> out;
        for (const ServerPacket& p : packets) {
            if (const auto m = p.message()) {
                out.push_back(*m);
            }
        }
        return out;
    }
};

SessionConfig session_config() {
    return SessionConfig{
        .max_connections = 40, .output_buffer_bytes = 8192, .output_high_water = 4096};
}

class EpollGatewayTest : public ::testing::Test {
protected:
    explicit EpollGatewayTest(SessionConfig config = session_config(), int socket_buffer = 0) {
        g_fake_now = 0;
        sessions_ = std::make_unique<Manager>(
            config, protocol::OrderEntryConfig{
                        .max_owners = 40, .max_live_orders = 8192, .max_symbols = 1});
        sessions_->add_account(
            {.username = "alice", .password = "secret", .cancel_on_disconnect = true});
        sessions_->add_account({.username = "bob", .password = "hunter2"});
        names_.reserve(38);
        for (int i = 3; i <= 40; ++i) {
            const std::string name = "u" + std::to_string(i);
            names_.push_back(name);
            sessions_->add_account({.username = names_.back(), .password = "pw"});
        }
        sessions_->add_symbol(litbook::testing::unwrap(make_ticker("AAPL")),
                              litbook::testing::with_test_limits(BookConfig{.tick_size = 100}));
        gateway_ = std::make_unique<Gateway>(*sessions_,
                                             GatewayConfig{.socket_send_buffer = socket_buffer,
                                                           .socket_receive_buffer = socket_buffer,
                                                           .clock = &fake_clock});
    }

    Peer connect(int receive_buffer = 0) {
        Peer p;
        p.client =
            std::make_unique<TestClient>(gateway_->port(), std::size_t{1} << 20, receive_buffer);
        return p;
    }

    // Runs the gateway and lets every peer collect what arrived, until `done` is true or
    // about two seconds have passed. Returns whether `done` became true.
    bool pump(const std::vector<Peer*>& peers, const std::function<bool()>& done) {
        for (int round = 0; round < 400; ++round) {
            gateway_->poll_once(5);
            for (Peer* p : peers) {
                p->collect();
            }
            if (done()) {
                return true;
            }
        }
        return false;
    }

    // Connects and logs in, and waits for Login Accepted.
    Peer logged_in(std::string_view user, std::string_view password) {
        Peer p = connect();
        const Bytes l = login(user, password);
        EXPECT_EQ(p.client->send_some(l), l.size());
        EXPECT_TRUE(pump({&p}, [&] { return p.count(soup::PacketType::LoginAccepted) == 1; }));
        return p;
    }

    void send(Peer& p, const Bytes& bytes) { EXPECT_EQ(p.client->send_some(bytes), bytes.size()); }

    std::vector<std::string> names_;  // The usernames of accounts 3 to 40.
    std::unique_ptr<Manager> sessions_;
    std::unique_ptr<Gateway> gateway_;
};

TEST_F(EpollGatewayTest, LoginAndOrderOverARealSocket) {
    Peer alice = logged_in("alice", "secret");
    send(alice, unsequenced(order(Side::Buy, 1, 10, 500'000)));
    ASSERT_TRUE(pump({&alice}, [&] { return alice.messages().size() == 1; }));
    EXPECT_TRUE(std::holds_alternative<protocol::AcceptedMessage>(alice.messages()[0]));
    EXPECT_EQ(gateway_->open_sockets(), 1U);
    EXPECT_EQ(sessions_->order_entry().live_orders(), 1U);
}

TEST_F(EpollGatewayTest, TwoClientsTrade) {
    Peer alice = logged_in("alice", "secret");
    Peer bob = logged_in("bob", "hunter2");
    send(alice, unsequenced(order(Side::Buy, 1, 10, 500'000)));
    ASSERT_TRUE(pump({&alice}, [&] { return alice.messages().size() == 1; }));
    send(bob, unsequenced(order(Side::Sell, 1, 10, 500'000)));
    ASSERT_TRUE(pump({&alice, &bob},
                     [&] { return alice.messages().size() == 2 && bob.messages().size() == 2; }));
    EXPECT_TRUE(std::holds_alternative<protocol::ExecutedMessage>(alice.messages()[1]));
    EXPECT_TRUE(std::holds_alternative<protocol::ExecutedMessage>(bob.messages()[1]));
}

TEST_F(EpollGatewayTest, ManyClientsAtOnce) {
    std::vector<Peer> peers;
    peers.reserve(names_.size());
    for (const std::string& name : names_) {
        peers.push_back(connect());
        send(peers.back(), login(name, "pw"));
    }
    std::vector<Peer*> all;
    all.reserve(peers.size());
    for (Peer& p : peers) {
        all.push_back(&p);
    }
    ASSERT_TRUE(pump(all, [&] {
        for (const Peer& p : peers) {
            if (p.count(soup::PacketType::LoginAccepted) != 1) {
                return false;
            }
        }
        return true;
    }));
    // Every client rests 5 buys, then half of them sell into the others.
    for (std::size_t i = 0; i < peers.size(); ++i) {
        for (protocol::UserRefNum ref = 1; ref <= 5; ++ref) {
            const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
            const Price price = side == Side::Buy ? 490'000 : 510'000;
            send(peers[i], unsequenced(order(side, ref, 10, price)));
        }
    }
    ASSERT_TRUE(
        pump(all, [&] { return sessions_->order_entry().live_orders() == 5 * peers.size(); }));
    for (std::size_t i = 1; i < peers.size(); i += 2) {
        send(peers[i], unsequenced(order(Side::Sell, 6, 10, 490'000)));
    }
    const std::size_t sellers = peers.size() / 2;
    ASSERT_TRUE(
        pump(all, [&] { return sessions_->order_entry().engine().last_match_id() == sellers; }));
    EXPECT_EQ(gateway_->open_sockets(), peers.size());
}

TEST_F(EpollGatewayTest, ClientWritingOneByteAtATime) {
    Peer alice = connect();
    const Bytes stream =
        concat({login("alice", "secret"), unsequenced(order(Side::Buy, 1, 10, 500'000)),
                unsequenced(order(Side::Buy, 2, 10, 490'000))});
    for (const std::byte b : stream) {
        send(alice, Bytes{b});
        gateway_->poll_once(0);
    }
    ASSERT_TRUE(pump({&alice}, [&] { return alice.messages().size() == 2; }));
    EXPECT_EQ(alice.count(soup::PacketType::LoginAccepted), 1U);
}

TEST_F(EpollGatewayTest, MalformedInputEndsTheConnection) {
    Peer alice = logged_in("alice", "secret");
    send(alice, Bytes{std::byte{0}, std::byte{0}});  // A zero length.
    ASSERT_TRUE(pump({&alice}, [&] { return alice.client->closed(); }));
    EXPECT_EQ(gateway_->open_sockets(), 0U);
    EXPECT_EQ(sessions_->stats().closed(CloseReason::BadLength), 1U);
}

TEST_F(EpollGatewayTest, RejectedLoginIsDeliveredBeforeTheClose) {
    Peer p = connect();
    send(p, login("alice", "wrong"));
    ASSERT_TRUE(pump({&p}, [&] { return p.client->closed(); }));
    ASSERT_EQ(p.packets.size(), 1U);
    EXPECT_EQ(p.packets[0].type, soup::PacketType::LoginRejected);
}

TEST_F(EpollGatewayTest, ClientHangingUpCancelsItsOrders) {
    Peer alice = logged_in("alice", "secret");
    Peer bob = logged_in("bob", "hunter2");
    send(alice, unsequenced(order(Side::Buy, 1, 10, 500'000)));
    send(bob, unsequenced(order(Side::Buy, 1, 10, 490'000)));
    ASSERT_TRUE(pump({&alice, &bob}, [&] { return sessions_->order_entry().live_orders() == 2; }));
    alice.client->disconnect();
    bob.client->disconnect();
    ASSERT_TRUE(pump({}, [&] { return gateway_->open_sockets() == 0; }));
    // alice asked for cancel on disconnect and bob did not.
    EXPECT_EQ(sessions_->order_entry().open_orders(1), 0U);
    EXPECT_EQ(sessions_->order_entry().open_orders(2), 1U);
    EXPECT_EQ(sessions_->stats().closed(CloseReason::PeerClosed), 2U);
}

TEST_F(EpollGatewayTest, ConnectionsBeyondTheLimitAreClosed) {
    std::vector<Peer> peers;
    peers.reserve(41);
    for (int i = 0; i < 41; ++i) {
        peers.push_back(connect());
    }
    std::vector<Peer*> all;
    all.reserve(peers.size());
    for (Peer& p : peers) {
        all.push_back(&p);
    }
    ASSERT_TRUE(pump(all, [&] { return peers.back().client->closed(); }));
    EXPECT_EQ(gateway_->open_sockets(), 40U);
    EXPECT_EQ(sessions_->stats().connections_refused, 1U);
}

TEST_F(EpollGatewayTest, HeartbeatsAndIdleTimeoutFollowTheClock) {
    Peer alice = logged_in("alice", "secret");
    g_fake_now = kSecond;
    ASSERT_TRUE(
        pump({&alice}, [&] { return alice.count(soup::PacketType::ServerHeartbeat) == 1; }));
    g_fake_now = 15 * kSecond;
    ASSERT_TRUE(pump({&alice}, [&] { return alice.client->closed(); }));
    EXPECT_EQ(sessions_->stats().closed(CloseReason::IdleTimeout), 1U);
}

TEST_F(EpollGatewayTest, EndOfSessionReachesEveryClient) {
    Peer alice = logged_in("alice", "secret");
    Peer bob = logged_in("bob", "hunter2");
    gateway_->end_session();
    ASSERT_TRUE(
        pump({&alice, &bob}, [&] { return alice.client->closed() && bob.client->closed(); }));
    EXPECT_EQ(alice.count(soup::PacketType::EndOfSession), 1U);
    EXPECT_EQ(bob.count(soup::PacketType::EndOfSession), 1U);
}

// A client that sends a big burst without reading is paused, not dropped, and gets every
// reply once it reads.
TEST_F(EpollGatewayTest, BurstFromAClientIsSlowedDownNotDropped) {
    Peer alice = logged_in("alice", "secret");
    Bytes burst;
    // 1,000 replies of 48 bytes are far above the 4 KiB high water mark, and the orders
    // still fit in the test book (1,024 orders).
    constexpr protocol::UserRefNum kOrders = 1000;
    for (protocol::UserRefNum ref = 1; ref <= kOrders; ++ref) {
        const Bytes p = unsequenced(order(Side::Buy, ref, 1, 100'000 + ref * 100));
        burst.insert(burst.end(), p.begin(), p.end());
    }
    std::span<const std::byte> rest(burst);
    for (int round = 0; round < 2000 && !rest.empty(); ++round) {
        rest = rest.subspan(alice.client->send_some(rest));
        gateway_->poll_once(1);
    }
    // Only now does alice start reading.
    ASSERT_TRUE(rest.empty());
    ASSERT_TRUE(pump({&alice}, [&] { return alice.messages().size() == kOrders; }));
    EXPECT_FALSE(alice.client->closed());
    EXPECT_EQ(sessions_->stats().closed(CloseReason::OutputFull), 0U);
}

// With small socket buffers, a client that never reads while others trade against it
// fills its output buffer and is disconnected.
class SlowReaderTest : public EpollGatewayTest {
protected:
    SlowReaderTest()
        : EpollGatewayTest(
              SessionConfig{
                  .max_connections = 40, .output_buffer_bytes = 2048, .output_high_water = 1024},
              4096) {}
};

TEST_F(SlowReaderTest, ClientThatStopsReadingIsDisconnected) {
    Peer slow = connect(4096);
    send(slow, login("alice", "secret"));
    Peer fast = logged_in("bob", "hunter2");
    send(slow, unsequenced(order(Side::Buy, 1, 1'000'000, 500'000)));
    ASSERT_TRUE(pump({&fast}, [&] { return sessions_->order_entry().live_orders() == 1; }));
    // bob sells one share at a time, so every trade queues an execution for alice.
    protocol::UserRefNum ref = 0;
    const bool dropped = pump({&fast}, [&] {
        for (int i = 0; i < 20; ++i) {
            send(fast, unsequenced(order(Side::Sell, ++ref, 1, 500'000)));
        }
        return sessions_->stats().closed(CloseReason::OutputFull) == 1;
    });
    EXPECT_TRUE(dropped) << "after " << ref << " sells";
    EXPECT_EQ(sessions_->connection_of(1), Manager::kNoConnection);
    EXPECT_FALSE(fast.client->closed());
}

// Here the kernel buffers are too small to soak up the replies, so the burst really does
// pause the client's input, and every reply still arrives once the client reads.
TEST_F(SlowReaderTest, BurstPausesTheClientAndEveryReplyArrives) {
    Peer alice = connect(4096);
    send(alice, login("alice", "secret"));
    ASSERT_TRUE(pump({&alice}, [&] { return alice.count(soup::PacketType::LoginAccepted) == 1; }));
    const auto id = sessions_->connection_of(1);
    Bytes burst;
    constexpr protocol::UserRefNum kOrders = 1000;
    for (protocol::UserRefNum ref = 1; ref <= kOrders; ++ref) {
        const Bytes p = unsequenced(order(Side::Buy, ref, 1, 100'000 + ref * 100));
        burst.insert(burst.end(), p.begin(), p.end());
    }
    std::span<const std::byte> rest(burst);
    bool paused = false;
    for (int round = 0; round < 200; ++round) {
        rest = rest.subspan(alice.client->send_some(rest));
        gateway_->poll_once(1);
        paused = paused || sessions_->receive_room(id) == 0;
    }
    // The gateway stopped taking alice's input. Whether her own send() calls block too
    // depends on how big the kernel lets her send buffer grow, which is Linux's business,
    // so the test only checks the part the gateway controls.
    EXPECT_TRUE(paused);
    // Now alice reads, the gateway catches up, and any rest of the burst goes out.
    ASSERT_TRUE(pump({&alice}, [&] {
        rest = rest.subspan(alice.client->send_some(rest));
        return alice.messages().size() == kOrders;
    }));
    EXPECT_TRUE(rest.empty());
    EXPECT_EQ(sessions_->stats().closed(CloseReason::OutputFull), 0U);
}

// The gateway on its own thread, as it runs for real. ThreadSanitizer checks that the
// only things shared between the threads are the sockets and stop().
TEST(EpollGatewayThreaded, ServesClientsFromItsOwnThread) {
    Manager sessions(
        SessionConfig{.max_connections = 4},
        protocol::OrderEntryConfig{.max_owners = 2, .max_live_orders = 1024, .max_symbols = 1});
    sessions.add_account({.username = "alice", .password = "secret"});
    sessions.add_symbol(litbook::testing::unwrap(make_ticker("AAPL")),
                        litbook::testing::with_test_limits(BookConfig{.tick_size = 100}));
    Gateway gateway(sessions, GatewayConfig{.tick_interval = 10'000'000});
    const std::uint16_t port = gateway.port();
    std::thread server([&] { gateway.run(); });

    Peer alice{.client = std::make_unique<TestClient>(port), .packets = {}};
    EXPECT_EQ(alice.client->send_some(login("alice", "secret")), 49U);
    for (protocol::UserRefNum ref = 1; ref <= 100; ++ref) {
        const Bytes p = unsequenced(order(Side::Buy, ref, 1, 400'000 + ref * 100));
        EXPECT_EQ(alice.client->send_some(p), p.size());
    }
    bool done = false;
    for (int i = 0; i < 2000 && !done; ++i) {
        alice.collect();
        done = alice.messages().size() == 100;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    gateway.stop();
    server.join();
    EXPECT_TRUE(done);
    EXPECT_EQ(sessions.order_entry().live_orders(), 100U);
}

TEST(EpollGatewayConfig, BadAddressThrows) {
    Manager sessions(
        SessionConfig{.max_connections = 1},
        protocol::OrderEntryConfig{.max_owners = 1, .max_live_orders = 16, .max_symbols = 1});
    EXPECT_THROW(Gateway(sessions, GatewayConfig{.address = "not an address"}),
                 std::invalid_argument);
}

TEST(EpollGatewayConfig, PortInUseThrows) {
    Manager sessions(
        SessionConfig{.max_connections = 1},
        protocol::OrderEntryConfig{.max_owners = 1, .max_live_orders = 16, .max_symbols = 1});
    const Gateway first(sessions, GatewayConfig{});
    EXPECT_THROW(Gateway(sessions, GatewayConfig{.port = first.port()}), std::system_error);
}

}  // namespace
}  // namespace litbook::gateway
