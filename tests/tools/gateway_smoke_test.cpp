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
#include <variant>
#include <vector>

#include "gateway/soup_test_support.hpp"
#include "gateway/test_client.hpp"
#include "support/unwrap.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration): posix_spawn needs it.

namespace litbook::gateway {
namespace {

using testing::TestClient;

// The gateway program running as a child process, with its standard output in a pipe.
class GatewayProcess {
public:
    explicit GatewayProcess(std::vector<std::string> args) {
        args.insert(args.begin(), LITBOOK_GATEWAY);
        std::array<int, 2> pipe_fds{};
        if (::pipe(pipe_fds.data()) != 0) {
            throw std::runtime_error("pipe failed");
        }
        posix_spawn_file_actions_t actions{};
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& a : args) {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        const int rc = posix_spawn(&pid_, args[0].c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipe_fds[1]);
        if (rc != 0) {
            ::close(pipe_fds[0]);
            throw std::runtime_error("posix_spawn failed");
        }
        out_.reset(::fdopen(pipe_fds[0], "r"));
    }

    GatewayProcess(const GatewayProcess&) = delete;
    GatewayProcess& operator=(const GatewayProcess&) = delete;
    GatewayProcess(GatewayProcess&&) = delete;
    GatewayProcess& operator=(GatewayProcess&&) = delete;

    ~GatewayProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
    }

    // The next line the program printed, without its newline, or "" at the end.
    std::string line() {
        std::array<char, 512> buffer{};
        if (std::fgets(buffer.data(), static_cast<int>(buffer.size()), out_.get()) == nullptr) {
            return "";
        }
        std::string s(buffer.data());
        if (!s.empty() && s.back() == '\n') {
            s.pop_back();
        }
        return s;
    }

    // Reads the "listening on 127.0.0.1:PORT" line and returns the port.
    std::uint16_t port() {
        const std::string first = line();
        const auto colon = first.rfind(':');
        EXPECT_NE(colon, std::string::npos) << first;
        EXPECT_NE(first.find("listening on 127.0.0.1:"), std::string::npos) << first;
        return static_cast<std::uint16_t>(std::stoi(first.substr(colon + 1)));
    }

    // Sends a signal and returns the exit status, or -1 if it did not exit normally.
    int stop(int signal) {
        ::kill(pid_, signal);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

private:
    pid_t pid_ = -1;
    // The read end of the pipe as a FILE, closed by fclose when the object goes away.
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out_{nullptr, &std::fclose};
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
