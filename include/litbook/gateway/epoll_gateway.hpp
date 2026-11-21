#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/gateway/session.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/socket.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace litbook::gateway {

struct GatewayConfig {
    std::string_view address = "127.0.0.1";
    std::uint16_t port = 0;  // 0 lets the kernel pick a free port. port() says which.
    int backlog = 128;
    // The most bytes read from one socket per wakeup, so one busy client cannot keep
    // the loop away from the others for long.
    std::size_t read_chunk_bytes = std::size_t{64} * 1024;
    // How often SessionManager::tick() runs (heartbeats and timeouts).
    Timestamp tick_interval = 100'000'000;
    // The kernel send and receive buffers of each accepted socket, or 0 for the kernel's
    // defaults. Tests make them small so backpressure and a client that stops reading
    // show up after a few kilobytes.
    int socket_send_buffer = 0;
    int socket_receive_buffer = 0;
    // The clock for timestamps and timeouts. Tests replace it with a fake one.
    Timestamp (*clock)() = &nanoseconds_since_midnight;
};

// Extra work the gateway loop does for a threaded pipeline (see pipeline/reply_pump.hpp).
// drain() runs once per round, after the reads and before the writes, and moves replies
// that other threads produced into the session layer. wake_fd() is a descriptor that
// becomes readable when such replies arrive, or -1 for none. Before a round that may
// sleep in epoll_wait, prepare_to_sleep() is asked whether sleeping is allowed, and
// woke_up() is called after such a sleep.
template <class P>
concept GatewayPump = requires(P& pump, const P& const_pump, Timestamp now) {
    { pump.prepare_to_sleep() } -> std::same_as<bool>;
    pump.woke_up();
    pump.drain(now);
    { const_pump.wake_fd() } -> std::same_as<int>;
};

// The pump of the single-threaded gateway, which has nothing extra to do.
struct NoPump {
    [[nodiscard]] static bool prepare_to_sleep() noexcept { return true; }
    static void woke_up() noexcept {}
    static void drain(Timestamp /*now*/) noexcept {}
    [[nodiscard]] static int wake_fd() noexcept { return -1; }
};

// A single-threaded, non-blocking TCP server in front of a session layer. It owns the
// sockets and moves bytes between them and the session layer, and the session layer
// makes every protocol decision.
//
// Each call to poll_once() waits on epoll, accepts new connections, reads from the
// sockets that have data, runs tick() when it is due, and then writes out every
// connection's output and closes the connections the session layer gave up on. epoll is
// level triggered. A socket's interest in reading is turned off while the session layer
// has no room for its input (backpressure), and interest in writing is turned on only
// while some output could not be written yet. That keeps epoll from waking the loop for
// sockets it cannot do anything with.
//
// Every buffer and table is allocated in the constructor, so serving clients never
// allocates. stop() may be called from any thread or from a signal handler.
//
// With a pump (a threaded pipeline), the replies come from another thread. The pump's
// descriptor is watched too, so a sleeping loop wakes up when replies arrive, and every
// round drains the replies before writing the sockets.
//
// These invariants hold between calls. A socket is stored for a ConnectionId exactly when
// the session layer has that connection open, and the epoll interest recorded for it is
// the one registered with the kernel.
template <class Sessions, GatewayPump Pump = NoPump>
class BasicEpollGateway {
public:
    using ConnectionId = typename Sessions::ConnectionId;

    // Opens the listening socket. Throws std::system_error if a system call fails.
    BasicEpollGateway(Sessions& sessions, const GatewayConfig& config)
        requires std::same_as<Pump, NoPump>
        : BasicEpollGateway(sessions, config, no_pump()) {}

    // The same, with a pump that the gateway calls every round. The pump must outlive
    // the gateway.
    BasicEpollGateway(Sessions& sessions, const GatewayConfig& config, Pump& pump)
        : sessions_(&sessions),
          pump_(&pump),
          config_(config),
          epoll_(::epoll_create1(EPOLL_CLOEXEC)),
          wake_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
          sockets_(sessions.max_connections()),
          interest_(sessions.max_connections(), 0),
          events_(static_cast<std::size_t>(sessions.max_connections()) + 2),
          read_buffer_(config.read_chunk_bytes) {
        if (!epoll_.valid() || !wake_.valid()) {
            throw std::system_error(errno, std::generic_category(), "epoll_create1 or eventfd");
        }
        listen_ = listen_tcp(config.address, config.port, config.backlog);
        watch(listen_.get(), EPOLLIN, kListenTag);
        watch(wake_.get(), EPOLLIN, kWakeTag);
        if (pump.wake_fd() >= 0) {
            watch(pump.wake_fd(), EPOLLIN, kPumpTag);
        }
        last_tick_ = config_.clock();
    }

    BasicEpollGateway(const BasicEpollGateway&) = delete;
    BasicEpollGateway& operator=(const BasicEpollGateway&) = delete;
    BasicEpollGateway(BasicEpollGateway&&) = delete;
    BasicEpollGateway& operator=(BasicEpollGateway&&) = delete;

    // Closes every client connection and tells the session layer, so accounts with
    // cancel on disconnect get their orders canceled.
    ~BasicEpollGateway() { close_all(config_.clock()); }

    // Closes every client connection now, as if each client had hung up.
    void close_all(Timestamp now) {
        for (ConnectionId id = 0; id < sockets_.size(); ++id) {
            if (sockets_[id].valid()) {
                close_connection(id, CloseReason::PeerClosed, now);
            }
        }
    }

    [[nodiscard]] std::uint16_t port() const { return local_port(listen_.get()); }

    // Waits up to timeout_ms milliseconds (0 returns at once, -1 waits forever) for
    // socket activity and handles it. Returns the number of epoll events handled, or -1
    // if epoll_wait failed for a reason other than a signal.
    int poll_once(int timeout_ms) {
        // A loop that may sleep asks the pump first. If replies are already on their
        // way, the pump says no and this round only looks without waiting.
        bool may_sleep = timeout_ms != 0;
        if (may_sleep && !pump_->prepare_to_sleep()) {
            timeout_ms = 0;
            may_sleep = false;
        }
        const int n = ::epoll_wait(epoll_.get(), events_.data(), static_cast<int>(events_.size()),
                                   timeout_ms);
        if (may_sleep) {
            pump_->woke_up();
        }
        if (n < 0) {
            return errno == EINTR ? 0 : -1;
        }
        const Timestamp now = config_.clock();
        for (int i = 0; i < n; ++i) {
            const epoll_event& e = events_[static_cast<std::size_t>(i)];
            if (e.data.u64 == kListenTag) {
                accept_all(now);
            } else if (e.data.u64 == kWakeTag) {
                drain_wake();
            } else if (e.data.u64 == kPumpTag) {
                continue;  // The pump's descriptor only wakes the loop. drain() is below.
            } else {
                read_from(static_cast<ConnectionId>(e.data.u64), now);
            }
        }
        pump_->drain(now);
        // A clock that went backwards (midnight) also runs tick() and starts over.
        if (now < last_tick_ || now - last_tick_ >= config_.tick_interval) {
            sessions_->tick(now);
            last_tick_ = now;
        }
        service_all(now);
        return n;
    }

    // Serves clients until stop() is called, sleeping in epoll_wait for at most one
    // tick interval at a time.
    void run() {
        const auto timeout_ms = static_cast<int>(config_.tick_interval / 1'000'000);
        run(timeout_ms < 1 ? 1 : timeout_ms);
    }

    // The same with a chosen epoll_wait timeout. A timeout of 0 never sleeps, so the
    // loop spins on one core for the lowest latency.
    void run(int timeout_ms) {
        while (!stopping_.load(std::memory_order_acquire)) {
            if (poll_once(timeout_ms) < 0) {
                return;
            }
        }
    }

    // Makes run() return soon. It is safe to call from another thread or a signal
    // handler, because it only stores to a lock-free atomic and writes to an eventfd.
    void stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        const std::uint64_t one = 1;
        // The only way this write can fail is a counter already near its limit, and then
        // the eventfd is readable anyway, so the loop still wakes up. GCC refuses a
        // (void) cast on write() when glibc's fortify checks are on, so the result is
        // checked explicitly.
        if (::write(wake_.get(), &one, sizeof(one)) < 0) {
            return;
        }
    }

    // Ends the trading session. Clients get End of Session and are disconnected.
    void end_session() {
        const Timestamp now = config_.clock();
        sessions_->end_session(now);
        service_all(now);
    }

    [[nodiscard]] std::uint32_t open_sockets() const noexcept { return open_sockets_; }

private:
    // epoll stores one 64-bit number per watched descriptor. For client sockets it is
    // the ConnectionId, and these two values (above any ConnectionId) mark the others.
    static constexpr std::uint64_t kListenTag = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint64_t kWakeTag = kListenTag - 1;
    static constexpr std::uint64_t kPumpTag = kListenTag - 2;

    // The pump used when none is given. It has no state, so one shared object is enough.
    static Pump& no_pump() noexcept {
        static NoPump pump;
        return pump;
    }

    // Resets the eventfd so it is no longer readable. Its value (how many stop() calls
    // there were) does not matter, and a failed read only means it was already reset.
    void drain_wake() noexcept {
        std::uint64_t count = 0;
        if (::read(wake_.get(), &count, sizeof(count)) < 0) {
            return;
        }
    }

    void watch(int fd, std::uint32_t events, std::uint64_t tag) {
        epoll_event e{};
        e.events = events;
        e.data.u64 = tag;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &e) != 0) {
            throw std::system_error(errno, std::generic_category(), "epoll_ctl");
        }
    }

    // Accepts every connection that is waiting. A connection the session layer has no
    // slot for is closed at once, and the client sees the connection end.
    void accept_all(Timestamp now) noexcept {
        for (;;) {
            Fd socket(::accept4(listen_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (!socket.valid()) {
                if (errno == EINTR || errno == ECONNABORTED) {
                    continue;
                }
                // EAGAIN means nobody else is waiting. Any other error (such as running
                // out of descriptors) also ends this round, and the listening socket
                // stays readable, so the next call tries again.
                return;
            }
            (void)set_no_delay(socket.get());
            if (config_.socket_send_buffer > 0) {
                (void)set_send_buffer(socket.get(), config_.socket_send_buffer);
            }
            if (config_.socket_receive_buffer > 0) {
                (void)set_receive_buffer(socket.get(), config_.socket_receive_buffer);
            }
            const ConnectionId id = sessions_->open(now);
            if (id == Sessions::kNoConnection) {
                continue;  // The Fd destructor closes the socket.
            }
            epoll_event e{};
            e.events = EPOLLIN;
            e.data.u64 = id;
            if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, socket.get(), &e) != 0) {
                sessions_->close(id, CloseReason::PeerClosed, now);
                continue;
            }
            sockets_[id] = std::move(socket);
            interest_[id] = EPOLLIN;
            ++open_sockets_;
        }
    }

    // Reads once from a socket that epoll reported. Level triggering reports it again if
    // more bytes are waiting, so there is no need to read until the socket is empty.
    void read_from(ConnectionId id, Timestamp now) {
        // An event can belong to a connection that was closed earlier in the same batch.
        if (id >= sockets_.size() || !sockets_[id].valid()) {
            return;
        }
        const std::size_t room = sessions_->receive_room(id);
        if (room == 0) {
            // The input is paused. A hang up still shows as readable, and recv() of 0
            // bytes cannot tell it apart from a pause, so look for that case directly.
            char probe = 0;
            if (::recv(sockets_[id].get(), &probe, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
                close_connection(id, CloseReason::PeerClosed, now);
            }
            return;
        }
        const std::size_t want = room < read_buffer_.size() ? room : read_buffer_.size();
        const ssize_t got = ::recv(sockets_[id].get(), read_buffer_.data(), want, 0);
        if (got > 0) {
            (void)sessions_->receive(
                id, std::span<const std::byte>(read_buffer_.data(), static_cast<std::size_t>(got)),
                now);
            return;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }
        // 0 bytes means the client closed its end, and any other error means the socket
        // is broken.
        close_connection(id, CloseReason::PeerClosed, now);
    }

    // Writes as much output as the kernel takes. Returns false if the socket broke and
    // the connection was closed.
    bool flush(ConnectionId id, Timestamp now) {
        for (;;) {
            const std::span<const std::byte> out = sessions_->output(id);
            if (out.empty()) {
                return true;
            }
            // MSG_NOSIGNAL turns a write to a closed socket into an EPIPE error instead of
            // a SIGPIPE signal, whose default action would kill the whole exchange.
            const ssize_t sent = ::send(sockets_[id].get(), out.data(), out.size(), MSG_NOSIGNAL);
            if (sent > 0) {
                // This may let paused input run again and queue more output, which the
                // loop then writes too.
                sessions_->sent(id, static_cast<std::size_t>(sent), now);
                continue;
            }
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;  // The kernel buffer is full. EPOLLOUT says when to go on.
            }
            close_connection(id, CloseReason::PeerClosed, now);
            return false;
        }
    }

    // One client's message can queue replies for many others, so after every round each
    // open connection gets its output written and its epoll interest brought up to date.
    void service_all(Timestamp now) {
        for (ConnectionId id = 0; id < sockets_.size(); ++id) {
            if (!sockets_[id].valid()) {
                continue;
            }
            if (!flush(id, now)) {
                continue;
            }
            if (sessions_->should_close(id)) {
                // flush() already tried to write what was left, such as a Login Rejected
                // or End of Session packet. Anything the kernel did not take is dropped.
                close_connection(id, CloseReason::PeerClosed, now);
                continue;
            }
            update_interest(id);
        }
    }

    void update_interest(ConnectionId id) noexcept {
        std::uint32_t want = 0;
        if (sessions_->receive_room(id) > 0) {
            want |= EPOLLIN;
        }
        if (!sessions_->output(id).empty()) {
            want |= EPOLLOUT;
        }
        if (want == interest_[id]) {
            return;
        }
        // Even with no interest at all, epoll still reports errors and hang ups.
        epoll_event e{};
        e.events = want;
        e.data.u64 = id;
        if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, sockets_[id].get(), &e) == 0) {
            interest_[id] = want;
        }
    }

    void close_connection(ConnectionId id, CloseReason reason, Timestamp now) {
        sessions_->close(id, reason, now);
        // Closing the descriptor also removes it from epoll.
        sockets_[id].reset();
        interest_[id] = 0;
        --open_sockets_;
    }

    Sessions* sessions_;
    Pump* pump_;
    GatewayConfig config_;
    Fd epoll_;
    Fd wake_;  // An eventfd that stop() writes to, so a waiting epoll_wait returns.
    Fd listen_;
    std::vector<Fd> sockets_;              // Indexed by ConnectionId.
    std::vector<std::uint32_t> interest_;  // The epoll events registered per connection.
    std::vector<epoll_event> events_;
    std::vector<std::byte> read_buffer_;
    Timestamp last_tick_ = 0;
    std::uint32_t open_sockets_ = 0;
    std::atomic<bool> stopping_{false};
};

// The single-threaded gateway of Phase 6.
template <OrderBook Book>
using EpollGateway = BasicEpollGateway<SessionManager<Book>>;

}  // namespace litbook::gateway
