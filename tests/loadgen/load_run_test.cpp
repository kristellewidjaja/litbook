// End-to-end tests of run_load against a real gateway in the same process. The test thread
// drives the gateway with poll_once while run_load runs on its own thread, so the gateway
// is only ever touched by one thread and its state can be read afterwards without a race.

#include "litbook/loadgen/load_run.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/loadgen/load_report.hpp"
#include "litbook/loadgen/pacer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace litbook::loadgen {
namespace {

using Manager = gateway::SessionManager<SortedOrderBook>;
using Gateway = gateway::EpollGateway<SortedOrderBook>;
using Clock = std::chrono::steady_clock;

std::string load_name(std::size_t i) {
    std::array<char, 8> name{};
    (void)std::snprintf(name.data(), name.size(), "L%05zu", i);
    return name.data();
}

class LocalExchange {
public:
    explicit LocalExchange(std::size_t accounts, const protocol::RiskLimits& limits = {})
        : sessions_(gateway::SessionConfig{},
                    protocol::OrderEntryConfig{.max_owners = static_cast<OwnerId>(accounts),
                                               .max_live_orders = 1U << 16U,
                                               .max_symbols = 1}),
          gateway_(sessions_, gateway::GatewayConfig{.port = 0}) {
        for (std::size_t i = 1; i <= accounts; ++i) {
            const std::string name = load_name(i);
            (void)sessions_.add_account(gateway::AccountConfig{.username = name,
                                                               .password = "load",
                                                               .cancel_on_disconnect = true,
                                                               .limits = limits});
        }
        (void)sessions_.add_symbol(FlowConfig{}.symbol,
                                   BookConfig{.tick_size = 100, .max_orders = 1U << 16U});
    }

    [[nodiscard]] std::uint16_t port() const { return gateway_.port(); }

    // Runs the load on another thread and serves it from this one. Afterwards keeps
    // serving until the disconnects have canceled every order, or a deadline passes.
    LoadResult run(const LoadConfig& config) {
        std::atomic<bool> done{false};
        LoadResult result;
        std::exception_ptr error;
        std::thread client([&] {
            try {
                result = run_load(config);
            } catch (...) {
                error = std::current_exception();
            }
            done.store(true, std::memory_order_release);
        });
        while (!done.load(std::memory_order_acquire)) {
            (void)gateway_.poll_once(1);
        }
        client.join();
        if (error) {
            std::rethrow_exception(error);
        }
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while ((live_orders() > 0 || gateway_.open_sockets() > 0) && Clock::now() < deadline) {
            (void)gateway_.poll_once(1);
        }
        return result;
    }

    [[nodiscard]] std::uint32_t live_orders() const {
        return sessions_.order_entry().live_orders();
    }
    [[nodiscard]] const gateway::SessionStats& stats() const { return sessions_.stats(); }

private:
    Manager sessions_;
    Gateway gateway_;
};

LoadConfig small_config(std::uint16_t port, std::size_t sessions, std::uint32_t threads) {
    LoadConfig c;
    c.port = port;
    for (std::size_t i = 1; i <= sessions; ++i) {
        c.accounts.push_back(Credentials{.username = load_name(i), .password = "load"});
    }
    c.threads = threads;
    c.rate = 4'000;
    c.warmup_ns = 100'000'000;
    c.pass_ns = 100'000'000;
    c.passes = 3;
    c.flow.live_target = 30;
    c.flow.band_ticks = 4;
    c.drain_timeout_ns = 10'000'000'000;
    return c;
}

std::uint64_t scheduled_count(const LoadConfig& c) {
    return sends_due(c.warmup_ns + c.pass_ns * c.passes - 1, c.rate);
}

std::string describe(const std::vector<std::string>& problems) {
    std::string out;
    for (const std::string& p : problems) {
        out += p + "\n";
    }
    return out;
}

TEST(LoadRunTest, HealthyRunAgreesWithTheExchange) {
    LocalExchange exchange(8);
    const LoadConfig config = small_config(exchange.port(), 8, 2);
    const LoadResult result = exchange.run(config);

    const auto problems = health_problems(result);
    EXPECT_TRUE(problems.empty()) << describe(problems);
    ASSERT_EQ(result.sessions.size(), 8U);
    EXPECT_TRUE(result.logins_in_time);
    EXPECT_EQ(exchange.stats().logins_accepted, 8U);

    std::uint64_t scheduled = 0;
    std::uint64_t pass_sent = 0;
    for (const PassLatency& p : result.passes) {
        pass_sent += p.sent;
    }
    std::uint64_t timed_answers = 0;
    for (const SessionResult& s : result.sessions) {
        EXPECT_EQ(s.state, SessionState::Ended) << s.username;
        EXPECT_TRUE(s.fence_answered) << s.username;
        EXPECT_EQ(s.counters.unanswered, 0U);
        EXPECT_EQ(s.unknown_replies, 0U);
        for (std::size_t k = 0; k < kRequestKinds; ++k) {
            // Every request was either answered or handled without a reply.
            EXPECT_EQ(s.counters.sent[k], s.counters.answered[k] + s.counters.ignored[k])
                << s.username << " kind " << k;
        }
        EXPECT_EQ(s.counters.sent[static_cast<std::size_t>(RequestKind::Fence)], 1U);
        EXPECT_EQ(s.counters.ignored[static_cast<std::size_t>(RequestKind::Enter)], 0U);
        EXPECT_LE(s.counters.max_in_flight, config.max_in_flight);
        scheduled += s.counters.unsent;
        for (std::size_t k = 0; k < kTimedKinds; ++k) {
            scheduled += s.counters.sent[k];
        }
        EXPECT_GT(s.first_sequence, 0U);
        EXPECT_EQ(s.next_ref,
                  s.counters.sent[0] + s.counters.sent[2] + 2);  // From 1, plus the fence.
    }
    // Every due request was sent or counted as unsent, and none was made up.
    EXPECT_EQ(scheduled, scheduled_count(config));

    const LoadTotals t = totals(result);
    EXPECT_EQ(t.sessions_ended, 8U);
    EXPECT_GT(t.counters.executions, 0U);
    EXPECT_EQ(t.counters.added, t.counters.removed);  // Both sides of every trade are ours.
    EXPECT_EQ(t.net_position, 0);
    EXPECT_GT(t.counters.sent[static_cast<std::size_t>(RequestKind::Cancel)], 0U);
    EXPECT_GT(t.counters.sent[static_cast<std::size_t>(RequestKind::Replace)], 0U);

    // Service time can never be above response time, request by request, so neither can
    // any percentile.
    for (const PassLatency& pass : result.passes) {
        for (std::size_t k = 0; k < kTimedKinds; ++k) {
            const auto& response = pass.response[k];
            const auto& service = pass.service[k];
            EXPECT_EQ(response.count(), service.count());
            EXPECT_LE(service.percentile(50), response.percentile(50));
            EXPECT_LE(service.max(), response.max());
            timed_answers += response.count();
        }
    }
    EXPECT_GT(timed_answers, 0U);
    EXPECT_LE(timed_answers, pass_sent);

    // Cancel on disconnect took away every order that was still resting.
    EXPECT_EQ(exchange.live_orders(), 0U);
}

TEST(LoadRunTest, WrongPasswordFailsOnlyThatSession) {
    LocalExchange exchange(2);
    LoadConfig config = small_config(exchange.port(), 2, 1);
    config.accounts[1].password = "wrong";
    const LoadResult result = exchange.run(config);
    ASSERT_EQ(result.sessions.size(), 2U);
    EXPECT_EQ(result.sessions[0].state, SessionState::Ended);
    EXPECT_EQ(result.sessions[1].state, SessionState::Failed);
    EXPECT_EQ(result.sessions[1].failure, SessionFailure::NotAuthorized);
    const auto problems = health_problems(result);
    ASSERT_FALSE(problems.empty());
    EXPECT_NE(describe(problems).find("L00002"), std::string::npos) << describe(problems);
    EXPECT_EQ(describe(problems).find("L00001"), std::string::npos) << describe(problems);
}

TEST(LoadRunTest, ReusedUserRefNumsAreIgnoredAndReported) {
    LocalExchange exchange(1);
    LoadConfig config = small_config(exchange.port(), 1, 1);
    config.passes = 1;
    const LoadResult first = exchange.run(config);
    ASSERT_TRUE(health_problems(first).empty()) << describe(health_problems(first));

    // The same numbers again are retransmissions to the exchange, so nothing is answered.
    config.drain_timeout_ns = 300'000'000;
    const LoadResult second = exchange.run(config);
    const auto problems = health_problems(second);
    EXPECT_NE(describe(problems).find("no reply"), std::string::npos) << describe(problems);
    EXPECT_FALSE(second.sessions[0].fence_answered);

    // Starting above every number used so far works again.
    config.flow.first_ref = std::max(first.sessions[0].next_ref, second.sessions[0].next_ref);
    const LoadResult third = exchange.run(config);
    EXPECT_TRUE(health_problems(third).empty()) << describe(health_problems(third));
}

TEST(LoadRunTest, OverloadShowsUpAsLagAndUnsentRequests) {
    // One request in flight per session and far more requests than that allows. The
    // requests that could not go out are counted, not lost, and the run is still healthy.
    LocalExchange exchange(2);
    LoadConfig config = small_config(exchange.port(), 2, 1);
    config.rate = 1'000'000;
    config.max_in_flight = 1;
    config.passes = 2;
    // With a warm up the whole run would be spent on requests that were due during it, and
    // those are never timed.
    config.warmup_ns = 0;
    const LoadResult result = exchange.run(config);
    EXPECT_TRUE(health_problems(result).empty()) << describe(health_problems(result));
    const LoadTotals t = totals(result);
    EXPECT_GT(t.counters.unsent, 0U);
    EXPECT_EQ(t.counters.max_in_flight, 1U);
    EXPECT_LT(achieved_rate(result, config), static_cast<double>(config.rate));
    std::uint64_t scheduled = t.counters.unsent;
    for (std::size_t k = 0; k < kTimedKinds; ++k) {
        scheduled += t.counters.sent[k];
    }
    EXPECT_EQ(scheduled, scheduled_count(config));
    // Requests waited for their turn, so the response time includes that wait and the
    // service time does not.
    const auto response = per_pass(result, Measure::Response, std::nullopt);
    const auto service = per_pass(result, Measure::Service, std::nullopt);
    EXPECT_GT(median_percentile(response, 50), median_percentile(service, 50));
}

TEST(LoadRunTest, OpenOrderLimitRejectsAreCountedAndTheFlowRecovers) {
    // Each account may only have 5 live orders while the flow aims for 30, so many enters
    // are rejected. Each rejected enter must free its slot again, and the run stays healthy.
    LocalExchange exchange(2, protocol::RiskLimits{.max_open_orders = 5});
    LoadConfig config = small_config(exchange.port(), 2, 1);
    config.max_live = 40;
    const LoadResult result = exchange.run(config);
    EXPECT_TRUE(health_problems(result).empty()) << describe(health_problems(result));
    const LoadTotals t = totals(result);
    EXPECT_GT(t.counters.rejected_open_limit, 0U);
    EXPECT_EQ(t.counters.rejected, t.counters.rejected_open_limit);
    for (const SessionResult& s : result.sessions) {
        EXPECT_LE(s.live_at_end, 5U);
        for (std::size_t k = 0; k < kRequestKinds; ++k) {
            EXPECT_EQ(s.counters.sent[k], s.counters.answered[k] + s.counters.ignored[k]);
        }
    }
    // The run kept going after the rejects, so later passes still sent requests.
    EXPECT_GT(result.passes.back().sent, 0U);
}

TEST(LoadRunTest, PinnedClientThreadStillWorks) {
    LocalExchange exchange(1);
    LoadConfig config = small_config(exchange.port(), 1, 1);
    config.passes = 1;
    config.cpus = {0};
    const LoadResult result = exchange.run(config);
    EXPECT_TRUE(health_problems(result).empty()) << describe(health_problems(result));
}

TEST(LoadRunTest, ConfigsThatCannotWorkAreRefused) {
    const LoadConfig good = small_config(1, 2, 1);
    auto refused = [](const LoadConfig& c) {
        EXPECT_THROW((void)run_load(c), std::invalid_argument);
    };
    LoadConfig c = good;
    c.accounts.clear();
    refused(c);
    c = good;
    c.threads = 0;
    refused(c);
    c = good;
    c.threads = 3;  // More threads than sessions.
    refused(c);
    c = good;
    c.rate = 0;
    refused(c);
    c = good;
    c.rate = kMaxRate + 1;
    refused(c);
    c = good;
    c.passes = 0;
    refused(c);
    c = good;
    c.pass_ns = 0;
    refused(c);
    c = good;
    c.max_in_flight = 0;
    refused(c);
    c = good;
    c.threads = 2;
    c.cpus = {0};  // One CPU for two threads.
    refused(c);
}

TEST(LoadRunTest, NoServerIsASystemError) {
    std::uint16_t port = 0;
    {
        const gateway::Fd listener = gateway::listen_tcp("127.0.0.1", 0, 1);
        port = gateway::local_port(listener.get());
    }
    EXPECT_THROW((void)run_load(small_config(port, 1, 1)), std::system_error);
}

}  // namespace
}  // namespace litbook::loadgen
