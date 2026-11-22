// End-to-end tests of the litbook_gateway program. Each test starts the real binary,
// talks to it over TCP like a client would, and stops it with a signal.

#include "litbook/engine/ticker.hpp"

#include <gtest/gtest.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "gateway/soup_test_support.hpp"
#include "gateway/test_client.hpp"
#include "support/unwrap.hpp"
#include "tools/child_process.hpp"

namespace litbook::gateway {
namespace {

using testing::TestClient;

// The gateway program as a child process.
class GatewayProcess : public litbook::testing::ChildProcess {
public:
    explicit GatewayProcess(std::vector<std::string> args)
        : ChildProcess(LITBOOK_GATEWAY, std::move(args)) {}
    std::uint16_t port() { return litbook::testing::gateway_port(*this); }
};

// Reads from the client until a packet arrives or about two seconds pass.
std::optional<testing::ServerPacket> wait_for_packet(TestClient& c) {
    for (int i = 0; i < 2000; ++i) {
        c.read_available();
        const soup::Packet p = c.next_packet();
        if (p.status == soup::PacketStatus::Complete) {
            return testing::ServerPacket{
                .type = static_cast<soup::PacketType>(static_cast<char>(p.type)),
                .payload = testing::Bytes(p.payload.begin(), p.payload.end())};
        }
        if (c.closed()) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

TEST(GatewayProgram, AcceptsALoginAndAnOrderThenStopsCleanly) {
    GatewayProcess gateway(
        {"--port", "0", "--account", "alice:secret:cod", "--symbol", "AAPL", "--symbol", "MSFT"});
    TestClient client(gateway.port());
    EXPECT_EQ(client.send_some(testing::login("alice", "secret")), 49U);
    const auto accepted = wait_for_packet(client);
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(litbook::testing::unwrap(accepted).type, soup::PacketType::LoginAccepted);

    const protocol::EnterOrder order{.user_ref = 1,
                                     .side = Side::Buy,
                                     .qty = 100,
                                     .symbol = litbook::testing::unwrap(make_ticker("MSFT")),
                                     .price = 3'000'000,
                                     .tif = TimeInForce::GoodTillCancel,
                                     .type = OrderType::Limit,
                                     .post_only = false};
    const testing::Bytes frame = testing::unsequenced(order);
    EXPECT_EQ(client.send_some(frame), frame.size());
    const auto reply = wait_for_packet(client);
    ASSERT_TRUE(reply.has_value());
    const auto message = litbook::testing::unwrap(reply).message();
    ASSERT_TRUE(message.has_value());
    EXPECT_TRUE(
        std::holds_alternative<protocol::AcceptedMessage>(litbook::testing::unwrap(message)));

    EXPECT_EQ(gateway.stop(SIGINT), 0);
    // The client gets End of Session before the connection closes.
    const auto last = wait_for_packet(client);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(litbook::testing::unwrap(last).type, soup::PacketType::EndOfSession);
    const std::string summary = gateway.line();
    EXPECT_NE(summary.find("stopped: logins 1"), std::string::npos) << summary;
    // alice had cancel on disconnect, so her order is gone.
    EXPECT_NE(summary.find("live orders 0"), std::string::npos) << summary;
}

TEST(GatewayProgram, StopsOnSigterm) {
    GatewayProcess gateway({"--port", "0"});
    (void)gateway.port();
    EXPECT_EQ(gateway.stop(SIGTERM), 0);
}

TEST(GatewayProgram, RejectsBadArguments) {
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"--port"}, std::vector<std::string>{"--port", "70000"},
          std::vector<std::string>{"--account", "nopassword"},
          std::vector<std::string>{"--symbol", "toolongticker"},
          std::vector<std::string>{"--frobnicate", "1"}}) {
        GatewayProcess gateway(args);
        // The program exits by itself. Signal 0 sends nothing, so stop() just waits.
        EXPECT_EQ(gateway.stop(0), 2) << args[0];
    }
}

}  // namespace
}  // namespace litbook::gateway
