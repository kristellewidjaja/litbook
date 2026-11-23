// End-to-end tests of litbook_loadgen. They start the real litbook_gateway with load
// accounts, run the load generator against it as a second program and read what it
// prints.

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "tools/child_process.hpp"

namespace litbook {
namespace {

class GatewayProcess : public testing::ChildProcess {
public:
    explicit GatewayProcess(std::vector<std::string> args)
        : ChildProcess(LITBOOK_GATEWAY, std::move(args)) {}
    std::uint16_t port() { return testing::gateway_port(*this); }
};

struct ProgramRun {
    int exit_code = -1;
    std::string output;
};

// Runs litbook_loadgen to the end and collects everything it printed.
ProgramRun run_loadgen(std::vector<std::string> args) {
    testing::ChildProcess loadgen(LITBOOK_LOADGEN, std::move(args));
    ProgramRun run;
    for (std::string line = loadgen.line(); !line.empty(); line = loadgen.line()) {
        run.output += line + "\n";
    }
    run.exit_code = loadgen.wait_for_exit(std::chrono::seconds(120));
    return run;
}

std::vector<std::string> short_run(std::uint16_t port, std::vector<std::string> extra = {}) {
    std::vector<std::string> args = {"--port",      std::to_string(port),
                                     "--sessions",  "4",
                                     "--threads",   "2",
                                     "--rate",      "2000",
                                     "--warmup-ms", "100",
                                     "--pass-ms",   "100",
                                     "--passes",    "3"};
    args.insert(args.end(), extra.begin(), extra.end());
    return args;
}

// The number after "next first ref: " on the last line.
std::string next_first_ref(const std::string& output) {
    const std::string key = "next first ref: ";
    const auto at = output.rfind(key);
    if (at == std::string::npos) {
        return "";
    }
    const auto end = output.find('\n', at);
    return output.substr(at + key.size(), end - at - key.size());
}

TEST(LoadgenProgram, HealthyRunAgainstTheGateway) {
    GatewayProcess gateway({"--port", "0", "--load-accounts", "4"});
    const ProgramRun run = run_loadgen(short_run(gateway.port(), {"--per-session", "on"}));
    EXPECT_EQ(run.exit_code, 0) << run.output;
    for (const char* expected :
         {"litbook_loadgen: 4 sessions on 2 threads, 2000 requests/s", "response time",
          "service time", "send lag", "health: ok", "L00004   ended", "next first ref: "}) {
        EXPECT_NE(run.output.find(expected), std::string::npos) << expected << "\n" << run.output;
    }
    EXPECT_EQ(gateway.stop(SIGINT), 0);
    // Cancel on disconnect left the book empty.
    EXPECT_NE(gateway.line().find("live orders 0"), std::string::npos);
}

TEST(LoadgenProgram, RatesRunOneAfterTheOtherAndCarryTheUserRefNums) {
    GatewayProcess gateway({"--port", "0", "--load-accounts", "2"});
    const std::uint16_t port = gateway.port();
    const ProgramRun sweep =
        run_loadgen({"--port", std::to_string(port), "--sessions", "2", "--rates", "1000,3000",
                     "--warmup-ms", "50", "--pass-ms", "50", "--passes", "2"});
    EXPECT_EQ(sweep.exit_code, 0) << sweep.output;
    EXPECT_NE(sweep.output.find("1000 requests/s"), std::string::npos) << sweep.output;
    EXPECT_NE(sweep.output.find("3000 requests/s"), std::string::npos) << sweep.output;

    // A second program started from 1 again sends numbers the accounts already used, so
    // the exchange ignores them and the run is not healthy.
    const ProgramRun again =
        run_loadgen({"--port", std::to_string(port), "--sessions", "2", "--rate", "1000",
                     "--warmup-ms", "0", "--pass-ms", "50", "--passes", "1"});
    EXPECT_EQ(again.exit_code, 1) << again.output;
    EXPECT_NE(again.output.find("no reply"), std::string::npos) << again.output;

    // Starting from the number the sweep printed works.
    const std::string next = next_first_ref(sweep.output);
    ASSERT_FALSE(next.empty()) << sweep.output;
    const ProgramRun fixed =
        run_loadgen({"--port", std::to_string(port), "--sessions", "2", "--rate", "1000",
                     "--warmup-ms", "0", "--pass-ms", "50", "--passes", "1", "--first-ref",
                     std::to_string(std::stoul(next) + 1000)});
    EXPECT_EQ(fixed.exit_code, 0) << fixed.output;
    EXPECT_EQ(gateway.stop(SIGINT), 0);
}

TEST(LoadgenProgram, WrongAccountIsUnhealthy) {
    GatewayProcess gateway({"--port", "0", "--load-accounts", "1"});
    const ProgramRun run =
        run_loadgen({"--port", std::to_string(gateway.port()), "--account", "L00001:nope",
                     "--warmup-ms", "0", "--pass-ms", "50", "--passes", "1"});
    EXPECT_EQ(run.exit_code, 1) << run.output;
    EXPECT_NE(run.output.find("not authorized"), std::string::npos) << run.output;
    EXPECT_EQ(gateway.stop(SIGINT), 0);
}

TEST(LoadgenProgram, NoGatewayExitsWithOne) {
    // Port 1 is never a litbook gateway, and nothing listens there in the container.
    const ProgramRun run = run_loadgen({"--port", "1", "--passes", "1"});
    EXPECT_EQ(run.exit_code, 1);
}

TEST(LoadgenProgram, RejectsBadArguments) {
    for (const std::vector<std::string>& args : {
             std::vector<std::string>{},
             std::vector<std::string>{"--sessions", "2"},  // No port.
             std::vector<std::string>{"--port", "15000", "--rate"},
             std::vector<std::string>{"--port", "0"},
             std::vector<std::string>{"--port", "15000", "--rate", "0"},
             std::vector<std::string>{"--port", "15000", "--rate", "1,2"},
             std::vector<std::string>{"--port", "15000", "--rates", "100,0"},
             std::vector<std::string>{"--port", "15000", "--sessions", "0"},
             std::vector<std::string>{"--port", "15000", "--sessions", "2", "--account", "a:b"},
             std::vector<std::string>{"--port", "15000", "--account", "nopassword"},
             std::vector<std::string>{"--port", "15000", "--mix", "10,10"},
             std::vector<std::string>{"--port", "15000", "--live-target", "50", "--max-live", "10"},
             std::vector<std::string>{"--port", "15000", "--per-session", "yes"},
             std::vector<std::string>{"--port", "15000", "--symbol", "toolongticker"},
             std::vector<std::string>{"--port", "15000", "--first-ref", "0"},
             std::vector<std::string>{"--port", "15000", "--frobnicate", "1"},
         }) {
        const ProgramRun run = run_loadgen(args);
        EXPECT_EQ(run.exit_code, 2) << (args.empty() ? "no arguments" : args.back());
    }
}

}  // namespace
}  // namespace litbook
