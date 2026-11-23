#include "litbook/loadgen/load_report.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace litbook::loadgen {
namespace {

metrics::LatencyHistogram one_value(std::uint64_t ns) {
    metrics::LatencyHistogram h;
    h.record(ns);
    return h;
}

SessionResult healthy_session(const char* name) {
    SessionResult s;
    s.username = name;
    s.state = SessionState::Ended;
    s.fence_answered = true;
    return s;
}

// Everything print_* writes, read back from a temporary file.
template <class Print>
std::string printed(Print&& print) {
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::tmpfile(), &std::fclose);
    EXPECT_NE(file, nullptr);
    print(file.get());
    EXPECT_EQ(std::fseek(file.get(), 0, SEEK_SET), 0);
    std::string text;
    std::array<char, 512> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), file.get()) != nullptr) {
        text += buffer.data();
    }
    return text;
}

TEST(LoadReportTest, MedianOfAnOddNumberOfPasses) {
    const std::vector<metrics::LatencyHistogram> passes = {one_value(30), one_value(10),
                                                           one_value(20)};
    EXPECT_EQ(median_percentile(passes, 50), 20U);
}

TEST(LoadReportTest, MedianOfAnEvenNumberIsTheUpperMiddle) {
    const std::vector<metrics::LatencyHistogram> passes = {one_value(40), one_value(10),
                                                           one_value(30), one_value(20)};
    EXPECT_EQ(median_percentile(passes, 50), 30U);
}

TEST(LoadReportTest, MedianOfNothingIsZero) {
    EXPECT_EQ(median_percentile({}, 99), 0U);
}

TEST(LoadReportTest, OneSlowPassDoesNotMoveTheMedian) {
    const std::vector<metrics::LatencyHistogram> passes = {
        one_value(20), one_value(21), one_value(20'000'000), one_value(19), one_value(22)};
    EXPECT_EQ(median_percentile(passes, 99.9), 21U);
}

TEST(LoadReportTest, TotalsAddEverySession) {
    LoadResult r;
    for (const char* name : {"A", "B"}) {
        SessionResult s = healthy_session(name);
        s.counters.sent = {5, 2, 1, 1};
        s.counters.executions = 3;
        s.counters.added = 1;
        s.counters.removed = 2;
        s.counters.max_in_flight = name[0] == 'A' ? 4 : 9;
        s.replies = 10;
        s.position = name[0] == 'A' ? 7 : -7;
        r.sessions.push_back(s);
    }
    r.sessions[1].state = SessionState::Failed;
    const LoadTotals t = totals(r);
    EXPECT_EQ(t.sessions, 2U);
    EXPECT_EQ(t.sessions_ended, 1U);
    EXPECT_EQ(t.counters.sent[0], 10U);
    EXPECT_EQ(t.counters.sent[3], 2U);
    EXPECT_EQ(t.counters.executions, 6U);
    EXPECT_EQ(t.counters.added, 2U);
    EXPECT_EQ(t.counters.removed, 4U);
    EXPECT_EQ(t.counters.max_in_flight, 9U);  // The largest, not the sum.
    EXPECT_EQ(t.replies, 20U);
    EXPECT_EQ(t.net_position, 0);
}

TEST(LoadReportTest, AllRowMergesTheThreeKinds) {
    LoadResult r;
    r.passes.resize(2);
    r.passes[0].response[0].record(100);
    r.passes[0].response[1].record(200);
    r.passes[0].response[2].record(300);
    r.passes[1].service[1].record(5);
    const auto all = per_pass(r, Measure::Response, std::nullopt);
    ASSERT_EQ(all.size(), 2U);
    EXPECT_EQ(all[0].count(), 3U);
    EXPECT_EQ(all[0].max(), 300U);
    EXPECT_EQ(all[1].count(), 0U);
    const auto cancels = per_pass(r, Measure::Service, RequestKind::Cancel);
    EXPECT_EQ(cancels[0].count(), 0U);
    EXPECT_EQ(cancels[1].max(), 5U);
    r.passes[1].send_lag.record(9);
    EXPECT_EQ(send_lag_per_pass(r)[1].max(), 9U);
}

TEST(LoadReportTest, AchievedRateCountsOnlyTheTimedPasses) {
    LoadConfig c;
    c.passes = 2;
    c.pass_ns = 500'000'000;
    LoadResult r;
    r.passes.resize(2);
    r.passes[0].sent = 300;
    r.passes[1].sent = 700;
    EXPECT_DOUBLE_EQ(achieved_rate(r, c), 1'000.0);
}

TEST(LoadReportTest, HealthProblemsNameTheSessionAndTheCause) {
    LoadResult r;
    r.logins_in_time = true;
    r.sessions.push_back(healthy_session("GOOD"));
    EXPECT_TRUE(health_problems(r).empty());

    SessionResult bad = healthy_session("BAD");
    bad.state = SessionState::Failed;
    bad.failure = SessionFailure::Closed;
    bad.fence_answered = false;
    bad.counters.unanswered = 3;
    bad.counters.ignored[static_cast<std::size_t>(RequestKind::Enter)] = 2;
    bad.unknown_replies = 4;
    r.sessions.push_back(bad);
    r.logins_in_time = false;
    const auto problems = health_problems(r);
    ASSERT_EQ(problems.size(), 6U);
    EXPECT_NE(problems[0].find("login timeout"), std::string::npos);
    EXPECT_NE(problems[1].find("BAD"), std::string::npos);
    EXPECT_NE(problems[1].find("connection closed"), std::string::npos);
    EXPECT_NE(problems[2].find("fence"), std::string::npos);
    EXPECT_NE(problems[3].find("3 requests"), std::string::npos);
    EXPECT_NE(problems[4].find("2 enters"), std::string::npos);
    EXPECT_NE(problems[5].find("4 replies"), std::string::npos);
    for (const std::string& p : problems) {
        EXPECT_EQ(p.find("GOOD"), std::string::npos) << p;
    }
}

TEST(LoadReportTest, ReportShowsEveryRowAndTheHealth) {
    LoadConfig c;
    c.passes = 1;
    LoadResult r;
    r.logins_in_time = true;
    r.passes.resize(1);
    r.passes[0].response[0].record(25'000);
    r.passes[0].service[0].record(24'000);
    r.passes[0].send_lag.record(100);
    r.passes[0].sent = 1;
    r.sessions.push_back(healthy_session("L00001"));
    const std::string text = printed([&](std::FILE* f) { print_report(f, c, r); });
    for (const char* expected :
         {"litbook_loadgen: 1 sessions", "response time from the due time", "service time",
          "send lag", "  enter", "  cancel", "  replace", "  all", "  lag", "health: ok"}) {
        EXPECT_NE(text.find(expected), std::string::npos) << expected << "\n" << text;
    }
    r.sessions[0].fence_answered = false;
    const std::string unhealthy = printed([&](std::FILE* f) { print_report(f, c, r); });
    EXPECT_NE(unhealthy.find("health: 1 problems"), std::string::npos) << unhealthy;
}

TEST(LoadReportTest, SessionTableHasALinePerSession) {
    LoadResult r;
    r.sessions.push_back(healthy_session("L00001"));
    r.sessions.push_back(healthy_session("L00002"));
    r.sessions[1].state = SessionState::Failed;
    const std::string text = printed([&](std::FILE* f) { print_sessions(f, r); });
    EXPECT_NE(text.find("L00001   ended"), std::string::npos) << text;
    EXPECT_NE(text.find("L00002   failed"), std::string::npos) << text;
}

}  // namespace
}  // namespace litbook::loadgen
