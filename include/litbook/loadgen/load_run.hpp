#pragma once

// One load test against a running litbook gateway (litbook_loadgen is the command line
// front end).
//
// Every account gets one session, and the sessions are shared out over a few client
// threads. Each thread drives its sessions through one epoll instance without ever
// sleeping, so a thread can serve dozens of sessions. A thread per session would put more
// threads than CPUs on the machine and measure the scheduler.
//
// A run has four stages.
// 1. Login. Every session connects (from the calling thread, before any client thread
//    starts) and logs in. The run starts when every thread has finished its logins, or
//    login_timeout_ns has passed, whichever comes first.
// 2. Schedule. For warmup_ns plus passes * pass_ns the sessions send on one shared open
//    loop schedule of `rate` requests per second (pacer.hpp). A request that is due is sent
//    unless its session already has max_in_flight requests waiting, in which case it
//    waits and its lateness shows up as send lag. A request belongs to the pass in which
//    it was due, and requests due during the warm up are not timed.
// 3. Drain. Each session sends a fence, an order that is always rejected, so its reply
//    proves that every earlier request was handled. The thread keeps reading until every
//    session's queue is empty or drain_timeout_ns has passed. Then it keeps reading until
//    every other thread has done the same, because a session on another thread may still
//    trade with this thread's resting orders until its own fence is answered. Once all
//    threads are there, the exchange has handled every request of the run, and each
//    session sends a second fence. Its reply proves that every reply sent to the session
//    before it has been read, including the ones about those late trades.
// 4. Logout. Each session logs out and its socket is closed. No session logs out while
//    another one can still trade with it.
//
// For each request kind and pass there are two histograms. Response time runs from the
// time the request was due to the moment its first reply had been read. That is what a
// client of the exchange sees, and it includes any time the request waited because the
// exchange (or the client) fell behind. Service time runs from the moment it was really
// handed to the socket, which leaves that waiting out. Send lag is how late each request
// left compared with its schedule. All times come from std::chrono::steady_clock in
// nanoseconds. On the dev machine that clock reads the same 24 MHz counter as TickClock
// (one tick is 41.67 ns), which is far finer than a loopback round trip.

#include "litbook/loadgen/client_session.hpp"
#include "litbook/loadgen/in_flight_queue.hpp"
#include "litbook/loadgen/order_flow.hpp"
#include "litbook/metrics/latency_histogram.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace litbook::loadgen {

struct Credentials {
    std::string username;
    std::string password;
};

struct LoadConfig {
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    std::vector<Credentials> accounts;  // One session for each.
    std::uint32_t threads = 1;
    std::uint64_t rate = 10'000;  // Requests per second, all sessions together.
    std::uint64_t warmup_ns = 1'000'000'000;
    std::uint64_t pass_ns = 1'000'000'000;
    std::uint32_t passes = 5;
    std::uint32_t max_in_flight = 64;  // Per session.
    std::uint32_t max_live = 1'000;    // Live orders plus enters on their way, per session.
    // Every session uses this flow, with seed + its index as its seed.
    FlowConfig flow;
    ClientSessionConfig session;
    std::uint64_t login_timeout_ns = 5'000'000'000;
    std::uint64_t drain_timeout_ns = 5'000'000'000;
    // Client thread i runs on cpus[i] when given. Empty leaves the threads unpinned.
    std::vector<int> cpus;
};

// The request kinds that are timed, which are the first three RequestKinds.
inline constexpr std::size_t kTimedKinds = 3;

struct SessionCounters {
    std::array<std::uint64_t, kRequestKinds> sent{};
    std::array<std::uint64_t, kRequestKinds> answered{};
    // Requests the exchange handled without a reply (see InFlightQueue).
    std::array<std::uint64_t, kRequestKinds> ignored{};
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;  // Not counting fences.
    std::uint64_t rejected_open_limit = 0;
    std::uint64_t replaced = 0;
    std::uint64_t canceled = 0;  // Every Order Canceled, whatever the reason.
    std::uint64_t executions = 0;
    std::uint64_t executed_shares = 0;
    std::uint64_t added = 0;    // Executions where the order was resting.
    std::uint64_t removed = 0;  // Executions where the order was the aggressor.
    // Requests that were due before the schedule ended but were never sent.
    std::uint64_t unsent = 0;
    // Requests still waiting for a reply when the drain ended.
    std::uint64_t unanswered = 0;
    std::uint64_t max_in_flight = 0;
};

struct SessionResult {
    std::string username;
    SessionState state = SessionState::LoggingIn;
    SessionFailure failure = SessionFailure::None;
    SessionCounters counters;
    std::uint64_t first_sequence = 0;  // From Login Accepted.
    std::uint64_t replies = 0;         // Sequenced messages received.
    std::uint64_t unknown_replies = 0;
    std::int64_t position = 0;
    std::size_t live_at_end = 0;
    bool fence_answered = false;  // The session's last fence got its reply.
    // The UserRefNum a later run of the same account must start from.
    protocol::UserRefNum next_ref = 0;
};

// The histograms of one timed pass, merged over every session.
struct PassLatency {
    std::array<metrics::LatencyHistogram, kTimedKinds> response{};
    std::array<metrics::LatencyHistogram, kTimedKinds> service{};
    metrics::LatencyHistogram send_lag;
    std::uint64_t sent = 0;  // Requests due in this pass that were sent.

    void merge(const PassLatency& other) noexcept;
};

struct LoadResult {
    std::vector<SessionResult> sessions;
    std::vector<PassLatency> passes;
    // From the end of the logins to the end of the schedule, as measured.
    std::uint64_t schedule_ns = 0;
    std::uint64_t drain_ns = 0;  // Both fences and the wait between them.
    // True if every client thread finished its logins before login_timeout_ns.
    bool logins_in_time = false;
};

// Runs one load test. Throws std::invalid_argument for a config that cannot work and
// std::system_error if a connection cannot be made.
[[nodiscard]] LoadResult run_load(const LoadConfig& config);

// The problems a run had, one line each. Empty means the run is healthy. A healthy run
// logged in and ended every session cleanly, answered every fence, left nothing
// unanswered, never left an Enter without a reply, and never got a reply about an order
// the flow did not know.
[[nodiscard]] std::vector<std::string> health_problems(const LoadResult& result);

}  // namespace litbook::loadgen
