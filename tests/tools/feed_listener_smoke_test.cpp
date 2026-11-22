// End-to-end tests of litbook_feed_listener with litbook_gateway --feed. Both programs run
// as child processes. A client sends an order over TCP, the gateway publishes it on
// multicast on lo, and the listener must show it and exit at End of Session.

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gateway/soup_test_support.hpp"
#include "gateway/test_client.hpp"
#include "pipeline/pipeline_test_support.hpp"
#include "support/unwrap.hpp"
#include "tools/child_process.hpp"

namespace litbook::feed {
namespace {

using gateway::testing::login;
using gateway::testing::TestClient;
using gateway::testing::unsequenced;
using litbook::testing::ChildProcess;

constexpr const char* kIncremental = "239.255.81.1";
constexpr const char* kSnapshot = "239.255.81.2";

// The listener's first line is "litbook_feed_listener joined A:P1 and B:P2 on lo". This
// returns the two ports.
std::pair<std::uint16_t, std::uint16_t> joined_ports(const std::string& first) {
    EXPECT_NE(first.find("litbook_feed_listener joined"), std::string::npos) << first;
    const auto and_at = first.find(" and ");
    const auto on_at = first.find(" on ");
    const std::string left = first.substr(0, and_at);
    const std::string right = first.substr(and_at, on_at - and_at);
    const auto port_after_colon = [](const std::string& s) {
        return static_cast<std::uint16_t>(std::stoi(s.substr(s.rfind(':') + 1)));
    };
    return {port_after_colon(left), port_after_colon(right)};
}

// Reads lines until one contains `text`, or the program closes its output.
std::optional<std::string> line_containing(ChildProcess& p, const std::string& text) {
    for (;;) {
        std::string l = p.line();
        if (l.empty()) {
            return std::nullopt;
        }
        if (l.find(text) != std::string::npos) {
            return l;
        }
    }
}

// Waits until the client has read `count` complete packets, or about five seconds pass.
bool read_packets(TestClient& client, std::size_t count) {
    std::size_t got = 0;
    for (int i = 0; i < 5000 && got < count; ++i) {
        client.read_available();
        while (client.next_packet().status == gateway::soup::PacketStatus::Complete) {
            ++got;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return got >= count;
}

TEST(FeedListenerProgram, ShowsTheGatewaysOrdersAndEndsWithTheSession) {
    // The listener first, on ports the kernel picks, because the gateway must send to them.
    ChildProcess listener(LITBOOK_FEED_LISTENER,
                          {"--incremental", std::string(kIncremental) + ":0", "--snapshot",
                           std::string(kSnapshot) + ":0", "--every", "0"});
    const auto [incremental_port, snapshot_port] = joined_ports(listener.line());

    ChildProcess gateway(
        LITBOOK_GATEWAY,
        {"--port", "0", "--account", "alice:secret", "--symbol", "AAPL", "--feed", "lo",
         "--feed-incremental", std::string(kIncremental) + ":" + std::to_string(incremental_port),
         "--feed-snapshot", std::string(kSnapshot) + ":" + std::to_string(snapshot_port)});
    TestClient client(litbook::testing::gateway_port(gateway));
    const std::string feed_line = gateway.line();
    EXPECT_NE(feed_line.find("litbook_gateway feed on lo: incremental 239.255.81.1:"),
              std::string::npos)
        << feed_line;

    ASSERT_GT(client.send_some(login("alice", "secret")), 0U);
    ASSERT_TRUE(read_packets(client, 1));  // Login Accepted.
    // A bid of 100 shares at $50.0000. alice has no cancel on disconnect, so it stays.
    const auto order = pipeline::testing::limit(Side::Buy, 1, 100, 500'000);
    ASSERT_GT(client.send_some(unsequenced(order)), 0U);
    ASSERT_TRUE(read_packets(client, 1));  // Accepted.

    EXPECT_EQ(gateway.stop(SIGINT), 0);
    // The gateway's End of Session makes the listener print its books and stop by itself.
    // unwrap throws, and so fails the test, if the line never came.
    const std::string bids = litbook::testing::unwrap(line_containing(listener, "bids"));
    EXPECT_NE(bids.find("100 @ 50.0000 (1)"), std::string::npos) << bids;
    const std::string ended = litbook::testing::unwrap(line_containing(listener, "ended:"));
    EXPECT_NE(ended.find("gaps 0"), std::string::npos) << ended;
    EXPECT_NE(ended.find("recovering no"), std::string::npos) << ended;
    EXPECT_EQ(listener.wait_for_exit(std::chrono::seconds(10)), 0);
}

TEST(FeedListenerProgram, AGatewayThatDiesWithoutEndOfSessionLeavesItSilent) {
    ChildProcess listener(LITBOOK_FEED_LISTENER,
                          {"--incremental", "239.255.81.5:0", "--snapshot", "239.255.81.6:0",
                           "--every", "1", "--silence", "1"});
    const auto [incremental_port, snapshot_port] = joined_ports(listener.line());
    ChildProcess gateway(LITBOOK_GATEWAY,
                         {"--port", "0", "--symbol", "AAPL", "--feed", "lo", "--feed-incremental",
                          "239.255.81.5:" + std::to_string(incremental_port), "--feed-snapshot",
                          "239.255.81.6:" + std::to_string(snapshot_port)});
    (void)litbook::testing::gateway_port(gateway);

    // The listener prints a status line every second. Wait until one shows packets, so
    // the feed was really heard before the gateway dies.
    bool heard = false;
    for (int i = 0; i < 10 && !heard; ++i) {
        const std::string status = litbook::testing::unwrap(line_containing(listener, "status:"));
        heard = status.find("packets 0,") == std::string::npos;
    }
    ASSERT_TRUE(heard);

    // SIGKILL gives the gateway no chance to send End of Session, like a lost one.
    (void)gateway.stop(SIGKILL);
    const auto silent = line_containing(listener, "silent:");
    ASSERT_TRUE(silent.has_value());
    EXPECT_EQ(listener.wait_for_exit(std::chrono::seconds(10)), 3);
}

TEST(FeedListenerProgram, StopsOnSigintBeforeAnyFeed) {
    ChildProcess listener(LITBOOK_FEED_LISTENER,
                          {"--incremental", "239.255.81.3:0", "--snapshot", "239.255.81.4:0"});
    (void)joined_ports(listener.line());
    EXPECT_EQ(listener.stop(SIGINT), 0);
}

TEST(FeedListenerProgram, RejectsBadArguments) {
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"--incremental"},
          std::vector<std::string>{"--incremental", "127.0.0.1:15100"},
          std::vector<std::string>{"--snapshot", "239.255.0.2"},
          std::vector<std::string>{"--every", "x"}, std::vector<std::string>{"--depth", "0"},
          std::vector<std::string>{"--silence", "-1"},
          std::vector<std::string>{"--frobnicate", "1"}}) {
        ChildProcess listener(LITBOOK_FEED_LISTENER, args);
        EXPECT_EQ(listener.wait_for_exit(std::chrono::seconds(10)), 2) << args[0];
    }
}

TEST(GatewayProgram, RejectsBadFeedArguments) {
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"--feed-incremental", "239.255.0.1:15100"},
          std::vector<std::string>{"--feed", "lo", "--feed-snapshot", "10.0.0.1:15101"},
          std::vector<std::string>{"--feed", "lo", "--feed-incremental", "239.255.0.1:0"},
          std::vector<std::string>{"--feed"}}) {
        ChildProcess gateway(LITBOOK_GATEWAY, args);
        EXPECT_EQ(gateway.wait_for_exit(std::chrono::seconds(10)), 2) << args[0];
    }
}

TEST(GatewayProgram, AFeedOnAnUnknownInterfaceFailsToStart) {
    ChildProcess gateway(LITBOOK_GATEWAY, {"--port", "0", "--feed", "no-such-if"});
    EXPECT_EQ(gateway.wait_for_exit(std::chrono::seconds(10)), 1);
}

}  // namespace
}  // namespace litbook::feed
