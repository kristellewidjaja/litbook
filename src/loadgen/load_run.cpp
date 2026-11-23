#include "litbook/loadgen/load_run.hpp"

#include "litbook/concurrency/thread_affinity.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/loadgen/pacer.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <variant>

namespace litbook::loadgen {

void PassLatency::merge(const PassLatency& other) noexcept {
    for (std::size_t k = 0; k < kTimedKinds; ++k) {
        response[k].merge(other.response[k]);
        service[k].merge(other.service[k]);
    }
    send_lag.merge(other.send_lag);
    sent += other.sent;
}

namespace {

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t elapsed_since(std::uint64_t from, std::uint64_t to) noexcept {
    return to > from ? to - from : 0;
}

// How many of the first `due` requests of the whole schedule belong to session `index`
// of `sessions`. Session s sends the requests whose number leaves remainder s.
std::uint64_t share_of(std::uint64_t due, std::uint64_t index, std::uint64_t sessions) noexcept {
    return due > index ? (due - index + sessions - 1) / sessions : 0;
}

void check_config(const LoadConfig& c) {
    if (c.accounts.empty()) {
        throw std::invalid_argument("a load run needs at least one account");
    }
    if (c.threads == 0 || c.threads > c.accounts.size()) {
        throw std::invalid_argument("threads must be between 1 and the number of sessions");
    }
    if (c.rate == 0 || c.rate > kMaxRate) {
        throw std::invalid_argument("rate must be between 1 and 1e9 per second");
    }
    if (c.passes == 0 || c.pass_ns == 0) {
        throw std::invalid_argument("a load run needs at least one pass of some length");
    }
    if (c.max_in_flight == 0) {
        throw std::invalid_argument("max_in_flight must be at least 1");
    }
    if (!c.cpus.empty() && c.cpus.size() < c.threads) {
        throw std::invalid_argument("give one CPU for every client thread");
    }
}

// Everything one session needs on its client thread.
struct SessionSlot {
    SessionSlot(const LoadConfig& config, std::size_t number)
        : username(config.accounts[number].username),
          session(config.accounts[number].username, config.accounts[number].password,
                  config.session),
          flow(flow_config(config, number), config.max_live),
          // One more slot than the limit, so the fence always fits.
          in_flight(std::size_t{config.max_in_flight} + 1),
          index(number) {}

    static FlowConfig flow_config(const LoadConfig& config, std::size_t number) {
        FlowConfig f = config.flow;
        f.seed += number;
        return f;
    }

    std::string username;
    gateway::Fd socket;
    ClientSession session;
    OrderFlow flow;
    InFlightQueue in_flight;
    std::size_t index;       // Its number among all sessions, which picks its requests.
    std::uint64_t sent = 0;  // How many scheduled requests it has sent.
    SessionCounters counters;
    bool fence_sent = false;
    bool fence_answered = false;
};

// The sessions of one client thread and the epoll instance that watches their sockets.
class Worker {
public:
    Worker(const LoadConfig& config, std::size_t first, std::size_t count)
        : config_(config),
          epoll_(::epoll_create1(EPOLL_CLOEXEC)),
          events_(count),
          passes_(config.passes) {
        if (!epoll_.valid()) {
            throw std::system_error(errno, std::generic_category(), "epoll_create1");
        }
        slots_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto slot = std::make_unique<SessionSlot>(config, first + i);
            slot->socket = gateway::connect_tcp(config.address, config.port);
            if (!gateway::set_nonblocking(slot->socket.get())) {
                throw std::system_error(errno, std::generic_category(), "set_nonblocking");
            }
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.u64 = i;
            if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, slot->socket.get(), &ev) != 0) {
                throw std::system_error(errno, std::generic_category(), "epoll_ctl");
            }
            slots_.push_back(std::move(slot));
        }
    }

    // Logs every session in. Returns false if some were still waiting at the deadline.
    bool log_in(std::uint64_t deadline) noexcept {
        std::uint64_t now = now_ns();
        for (auto& s : slots_) {
            s->session.begin(now);
        }
        for (;;) {
            bool waiting = false;
            for (auto& s : slots_) {
                flush(*s);
                waiting = waiting || s->session.state() == SessionState::LoggingIn;
            }
            if (!waiting) {
                return true;
            }
            if (now >= deadline) {
                return false;
            }
            poll();
            now = now_ns();
        }
    }

    // The schedule, the drain and the logout (stages 2 to 4 in load_run.hpp).
    void run(std::uint64_t start) noexcept {
        start_ = start;
        schedule_end_ = config_.warmup_ns + config_.pass_ns * config_.passes;
        while (now_ns() < start_) {
            poll();  // Keeps reading heartbeats while the other threads finish logging in.
        }
        for (;;) {
            const std::uint64_t now = now_ns();
            if (now >= start_ + schedule_end_) {
                break;
            }
            for (auto& s : slots_) {
                if (s->session.active()) {
                    send_due(*s, now - start_, now);
                    s->session.tick(now);
                    flush(*s);
                }
            }
            poll();
        }
        const std::uint64_t due = sends_due(schedule_end_ - 1, config_.rate);
        for (auto& s : slots_) {
            const std::uint64_t share = share_of(due, s->index, config_.accounts.size());
            s->counters.unsent = share > s->sent ? share - s->sent : 0;
        }
        drain();
        const std::uint64_t now = now_ns();
        for (auto& s : slots_) {
            s->counters.unanswered = s->in_flight.size();
            s->session.logout(now);
            flush(*s);
            s->socket.reset();
        }
    }

    [[nodiscard]] std::uint64_t drain_ns() const noexcept { return drain_ns_; }
    [[nodiscard]] const std::vector<PassLatency>& passes() const noexcept { return passes_; }

    void collect(std::vector<SessionResult>& out) const {
        for (const auto& s : slots_) {
            SessionResult r;
            r.username = s->username;
            r.state = s->session.state();
            r.failure = s->session.failure();
            r.counters = s->counters;
            r.first_sequence = s->session.first_sequence();
            r.replies = s->session.replies();
            r.unknown_replies = s->flow.unknown_replies();
            r.position = s->flow.position();
            r.live_at_end = s->flow.live();
            r.fence_answered = s->fence_answered;
            r.next_ref = s->flow.next_ref();
            out.push_back(r);
        }
    }

private:
    // Sends every request of this session that is due by `elapsed` and fits.
    void send_due(SessionSlot& s, std::uint64_t elapsed, std::uint64_t now) noexcept {
        const std::uint64_t sessions = config_.accounts.size();
        while (!s.in_flight.full() && s.in_flight.size() < config_.max_in_flight &&
               s.session.can_send()) {
            const std::uint64_t number = s.index + s.sent * sessions;
            const std::uint64_t offset = scheduled_offset_ns(number, config_.rate);
            if (offset > elapsed || offset >= schedule_end_) {
                return;
            }
            const std::optional<FlowRequest> request = s.flow.next();
            if (!request || !s.session.send(request->message, now)) {
                return;
            }
            const std::uint64_t scheduled = start_ + offset;
            const std::uint32_t pass =
                offset < config_.warmup_ns
                    ? kWarmupPass
                    : static_cast<std::uint32_t>((offset - config_.warmup_ns) / config_.pass_ns);
            (void)s.in_flight.push(InFlight{.kind = request->kind,
                                            .ref = request->ref,
                                            .pass = pass,
                                            .scheduled_ns = scheduled,
                                            .sent_ns = now});
            ++s.counters.sent[static_cast<std::size_t>(request->kind)];
            s.counters.max_in_flight =
                std::max<std::uint64_t>(s.counters.max_in_flight, s.in_flight.size());
            if (pass != kWarmupPass) {
                passes_[pass].sent += 1;
                passes_[pass].send_lag.record(elapsed_since(scheduled, now));
            }
            ++s.sent;
        }
    }

    // Sends each session a fence and reads until every queue is empty or time is up.
    void drain() noexcept {
        const std::uint64_t begin = now_ns();
        const std::uint64_t deadline = begin + config_.drain_timeout_ns;
        for (auto& s : slots_) {
            if (s->session.can_send()) {
                const FlowRequest fence = s->flow.fence();
                (void)s->session.send(fence.message, begin);
                (void)s->in_flight.push(InFlight{.kind = RequestKind::Fence,
                                                 .ref = fence.ref,
                                                 .pass = kWarmupPass,
                                                 .scheduled_ns = begin,
                                                 .sent_ns = begin});
                ++s->counters.sent[static_cast<std::size_t>(RequestKind::Fence)];
                s->fence_sent = true;
            }
        }
        std::uint64_t now = begin;
        for (;;) {
            bool waiting = false;
            for (auto& s : slots_) {
                if (s->session.active()) {
                    s->session.tick(now);
                    flush(*s);
                    waiting = waiting || !s->in_flight.empty();
                }
            }
            if (!waiting || now >= deadline) {
                break;
            }
            poll();
            now = now_ns();
        }
        drain_ns_ = now_ns() - begin;
    }

    // Reads every socket that has something to read, without waiting.
    void poll() noexcept {
        const int n =
            ::epoll_wait(epoll_.get(), events_.data(), static_cast<int>(events_.size()), 0);
        for (int i = 0; i < n; ++i) {
            read(*slots_[events_[static_cast<std::size_t>(i)].data.u64]);
        }
    }

    void read(SessionSlot& s) noexcept {
        if (!s.socket.valid()) {
            return;
        }
        gateway::ByteBuffer& in = s.session.input();
        bool closed = false;
        for (;;) {
            if (!in.make_room(1)) {
                process(s);
                if (s.session.finished() || !in.make_room(1)) {
                    s.session.fail(SessionFailure::InputFull);
                    break;
                }
            }
            const auto space = in.writable();
            const ssize_t n = ::recv(s.socket.get(), space.data(), space.size(), 0);
            if (n > 0) {
                in.commit(static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            closed = true;  // 0 is an orderly close, anything else an error.
            break;
        }
        process(s);
        if (closed) {
            s.session.fail(SessionFailure::Closed);  // Does nothing after End of Session.
        }
        if (s.session.finished()) {
            s.socket.reset();
        }
    }

    void process(SessionSlot& s) noexcept {
        const std::uint64_t arrival = now_ns();
        s.session.process_input(
            [this, &s, arrival](const protocol::OutboundMessage& m) { on_reply(s, m, arrival); });
    }

    void on_reply(SessionSlot& s, const protocol::OutboundMessage& reply,
                  std::uint64_t arrival) noexcept {
        count(s.counters, reply);
        Answer answer;
        if (const std::optional<ReplyKey> key = reply_key(reply)) {
            answer = s.in_flight.answer(key->kind, key->ref);
        }
        for (std::size_t k = 0; k < kRequestKinds; ++k) {
            s.counters.ignored[k] += answer.ignored[k];
        }
        if (answer.found) {
            const InFlight& r = answer.request;
            const auto kind = static_cast<std::size_t>(r.kind);
            ++s.counters.answered[kind];
            if (r.kind == RequestKind::Fence) {
                s.fence_answered = true;
                --s.counters.rejected;  // The fence is meant to be rejected.
            } else if (r.pass != kWarmupPass && r.pass < passes_.size()) {
                passes_[r.pass].response[kind].record(elapsed_since(r.scheduled_ns, arrival));
                passes_[r.pass].service[kind].record(elapsed_since(r.sent_ns, arrival));
            }
        }
        s.flow.on_reply(reply);
    }

    static void count(SessionCounters& c, const protocol::OutboundMessage& reply) noexcept {
        if (std::holds_alternative<protocol::AcceptedMessage>(reply)) {
            ++c.accepted;
        } else if (std::holds_alternative<protocol::ReplacedMessage>(reply)) {
            ++c.replaced;
        } else if (std::holds_alternative<protocol::CanceledMessage>(reply)) {
            ++c.canceled;
        } else if (const auto* e = std::get_if<protocol::ExecutedMessage>(&reply)) {
            ++c.executions;
            c.executed_shares += e->qty;
            if (e->liquidity == protocol::Liquidity::Added) {
                ++c.added;
            } else {
                ++c.removed;
            }
        } else if (const auto* r = std::get_if<protocol::RejectedMessage>(&reply)) {
            ++c.rejected;
            if (r->reason == protocol::WireRejectReason::MaxOpenOrdersExceeded) {
                ++c.rejected_open_limit;
            }
        }
    }

    // Writes as much of the session's output as the socket takes now.
    void flush(SessionSlot& s) noexcept {
        if (!s.socket.valid()) {
            return;
        }
        gateway::ByteBuffer& out = s.session.output();
        while (!out.empty()) {
            const auto bytes = out.readable();
            const ssize_t n = ::send(s.socket.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (n > 0) {
                out.consume(static_cast<std::size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            } else {
                s.session.fail(SessionFailure::Closed);
                s.socket.reset();
                return;
            }
        }
    }

    const LoadConfig& config_;
    gateway::Fd epoll_;
    std::vector<std::unique_ptr<SessionSlot>> slots_;
    std::vector<epoll_event> events_;
    std::vector<PassLatency> passes_;
    std::uint64_t start_ = 0;
    std::uint64_t schedule_end_ = 0;
    std::uint64_t drain_ns_ = 0;
};

}  // namespace

LoadResult run_load(const LoadConfig& config) {
    check_config(config);
    const std::size_t sessions = config.accounts.size();
    std::vector<std::unique_ptr<Worker>> workers;
    workers.reserve(config.threads);
    for (std::size_t w = 0; w < config.threads; ++w) {
        const std::size_t first = w * sessions / config.threads;
        const std::size_t end = (w + 1) * sessions / config.threads;
        workers.push_back(std::make_unique<Worker>(config, first, end - first));
    }

    std::atomic<std::uint32_t> ready{0};
    std::atomic<std::uint64_t> start{0};
    std::vector<char> in_time(config.threads, 0);
    const std::uint64_t login_deadline = now_ns() + config.login_timeout_ns;
    std::vector<std::thread> threads;
    threads.reserve(config.threads);
    for (std::size_t w = 0; w < config.threads; ++w) {
        threads.emplace_back([&, w] {
            if (!config.cpus.empty()) {
                (void)concurrency::pin_current_thread(config.cpus[w]);
            }
            in_time[w] = workers[w]->log_in(login_deadline) ? 1 : 0;
            ready.fetch_add(1, std::memory_order_release);
            std::uint64_t s = 0;
            while ((s = start.load(std::memory_order_acquire)) == 0) {
                std::this_thread::yield();
            }
            workers[w]->run(s);
        });
    }
    while (ready.load(std::memory_order_acquire) < config.threads) {
        std::this_thread::yield();
    }
    // A short gap so every thread is spinning on the start time before it arrives.
    const std::uint64_t begin = now_ns() + 1'000'000;
    start.store(begin, std::memory_order_release);
    for (std::thread& t : threads) {
        t.join();
    }

    LoadResult result;
    result.passes.resize(config.passes);
    result.logins_in_time = std::ranges::all_of(in_time, [](char c) { return c != 0; });
    result.schedule_ns = config.warmup_ns + config.pass_ns * config.passes;
    for (const auto& w : workers) {
        w->collect(result.sessions);
        for (std::size_t p = 0; p < config.passes; ++p) {
            result.passes[p].merge(w->passes()[p]);
        }
        result.drain_ns = std::max(result.drain_ns, w->drain_ns());
    }
    return result;
}

std::vector<std::string> health_problems(const LoadResult& result) {
    std::vector<std::string> problems;
    if (!result.logins_in_time) {
        problems.emplace_back("some sessions were not logged in before the login timeout");
    }
    for (const SessionResult& s : result.sessions) {
        const std::string who = "session " + s.username + ": ";
        if (s.state != SessionState::Ended) {
            problems.push_back(who + "did not end cleanly (" + to_string(s.failure) + ")");
        }
        if (!s.fence_answered) {
            problems.push_back(who + "the fence got no reply");
        }
        if (s.counters.unanswered != 0) {
            problems.push_back(who + std::to_string(s.counters.unanswered) +
                               " requests had no reply when the drain ended");
        }
        const auto enters = s.counters.ignored[static_cast<std::size_t>(RequestKind::Enter)] +
                            s.counters.ignored[static_cast<std::size_t>(RequestKind::Fence)];
        if (enters != 0) {
            problems.push_back(who + std::to_string(enters) +
                               " enters got no reply (UserRefNums already used? see --first-ref)");
        }
        if (s.unknown_replies != 0) {
            problems.push_back(who + std::to_string(s.unknown_replies) +
                               " replies named orders the session did not know");
        }
    }
    return problems;
}

}  // namespace litbook::loadgen
