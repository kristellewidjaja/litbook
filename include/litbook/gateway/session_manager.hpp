#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/inline_backend.hpp"
#include "litbook/gateway/order_backend.hpp"
#include "litbook/gateway/session.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/metrics/latency_histogram.hpp"
#include "litbook/metrics/tick_clock.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/types.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace litbook::gateway {

static_assert(std::is_same_v<Timestamp, protocol::Timestamp>,
              "the session layer and the order entry layer must use the same time type");

// One client account, set up at startup.
struct AccountConfig {
    std::string_view username;  // 1 to 6 letters or digits. Case does not matter.
    std::string_view password;  // 1 to 10 letters or digits. Case does not matter.
    // Cancel all of the account's live orders when its connection goes away.
    bool cancel_on_disconnect = false;
    protocol::RiskLimits limits{};
};

// Times are in nanoseconds, like every Timestamp.
struct SessionConfig {
    // The SoupBinTCP session name sent in Login Accepted. 1 to 10 letters or digits.
    std::string_view session_name = "1";
    std::uint32_t max_connections = 64;
    std::size_t input_buffer_bytes = 4096;
    std::size_t output_buffer_bytes = std::size_t{64} * 1024;
    // When this many bytes wait in a connection's output, its input is not processed
    // until the network code has written some of them out. The rest of the output
    // buffer is room for replies that cannot be held back, such as executions caused by
    // other accounts' orders.
    std::size_t output_high_water = std::size_t{32} * 1024;
    // The most requests of one account that may wait at an asynchronous backend (the
    // matching thread) at the same time. Only used when the backend is asynchronous.
    std::uint32_t max_in_flight = 64;
    // Every Nth order taken by the backend is timed, from the moment it is handed over
    // until every reply to it is in the output buffer (see request_latency()). 0 turns
    // the sampling off. It exists for the benchmarks.
    std::uint32_t sample_every = 0;
    // A heartbeat goes out when nothing else was sent to a client for this long.
    Timestamp heartbeat_interval = 1'000'000'000;
    // A logged in client that sends nothing for this long is disconnected.
    Timestamp idle_timeout = 15'000'000'000;
    // A connection that has not logged in after this long is disconnected.
    Timestamp login_timeout = 30'000'000'000;
};

// Counters for tests and operators.
struct SessionStats {
    std::uint64_t logins_accepted = 0;
    std::uint64_t logins_rejected = 0;
    std::uint64_t connections_refused = 0;  // open() found no free connection slot.
    std::uint64_t packets_in = 0;           // Whole packets received from clients.
    std::uint64_t messages_out = 0;         // Sequenced messages produced for accounts.
    // Sequenced messages whose account had no connection that could take them. They still
    // used up a sequence number (see docs/PROTOCOL.md).
    std::uint64_t messages_dropped = 0;
    std::uint64_t heartbeats_out = 0;
    // Times a packet had to wait because the backend's queue was full.
    std::uint64_t backend_busy = 0;
    // Cancels on disconnect that could not be queued at once and were sent later.
    std::uint64_t cancels_deferred = 0;
    std::array<std::uint64_t, kCloseReasonCount> closes{};  // Indexed by CloseReason.

    [[nodiscard]] std::uint64_t closed(CloseReason reason) const noexcept {
        return closes[static_cast<std::size_t>(reason)];
    }
};

// The SoupBinTCP session layer in front of an order backend (see order_backend.hpp). The
// backend is either the order entry layer itself, called on this thread (InlineBackend,
// which the SessionManager<Book> alias uses), or a queue to a matching thread
// (QueueBackend). The session layer knows nothing about
// sockets. The network code (or a test) tells it that a connection opened, hands it the
// bytes that arrived, calls tick() regularly so it can send heartbeats and notice
// timeouts, writes out whatever output() holds, and calls close() when the connection is
// gone. Keeping the rules apart from the system calls means every rule can be tested
// with plain byte arrays and a made up clock.
//
// The rules are listed in docs/PROTOCOL.md. In short, a connection must log in first,
// with the username and password of a configured account, and an account can have only
// one connection at a time. After that each Unsequenced Data packet carries one OUCH
// message for the order entry layer, and each reply goes back in a Sequenced Data
// packet. Anything malformed closes the connection.
//
// Backpressure works like this. A connection's input is only processed while its output
// holds fewer than output_high_water bytes. Past that, new bytes wait in the input
// buffer, and once that is full receive() takes no more, so the network code stops
// reading the socket and TCP slows the client down. Writing output out (sent()) resumes
// the processing. A connection is only dropped for OutputFull when replies that cannot
// be held back fill its whole output buffer, which means the client stopped reading.
//
// With an asynchronous backend the replies to a request arrive later, so the output alone
// does not show how much is still coming. The session layer then also counts each
// account's requests in flight (forwarded but not yet reported done through
// request_done()). The next packet is only forwarded while the output plus one reply's
// worth of room per request in flight stays below the high water mark, and while fewer
// than max_in_flight requests are in flight. If the backend's queue is full, the packet
// stays in the input buffer and retry_waiting() offers it again later. The gateway thread
// therefore never waits for the matching thread.
//
// Every connection slot and buffer is allocated in the constructor, so handling bytes,
// ticks and closes never allocates.
//
// These invariants hold between calls. An account's connection, if it has one, is an
// open session that is logged in as that account (or closing after being logged in).
// Every account's next_sequence() is 1 plus the number of sequenced messages produced
// for it. open_connections() is the number of sessions in use. With an asynchronous
// backend, in_flight(owner) is the number of the account's requests that were forwarded
// and not yet reported done, and is always 0 with an inline backend. With sampling on, at most one
// sampled order is being timed at a time.
template <OrderBackend Backend>
class BasicSessionManager final : private ReplyTarget {
public:
    using ConnectionId = std::uint32_t;
    static constexpr ConnectionId kNoConnection = std::numeric_limits<ConnectionId>::max();

    // Packets longer than this (the value of the length field) close the connection. It
    // is the size of a Login Request, the largest packet a client needs to send.
    static constexpr std::size_t kMaxClientPacket = 1 + soup::kLoginRequestPayload;

    // Builds the backend from backend_args, for example a QueueBackend from its queue.
    // Throws std::invalid_argument for a bad session name, zero connections, buffers too
    // small to hold one packet, a zero time limit or a zero max_in_flight, and whatever
    // the backend throws.
    template <class... BackendArgs>
    BasicSessionManager(const SessionConfig& config, std::in_place_t /*build backend*/,
                        BackendArgs&&... backend_args)
        : config_(checked(config)),
          session_name_(padded_left<soup::kSessionLength>(config.session_name)),
          backend_(std::forward<BackendArgs>(backend_args)...),
          accounts_(static_cast<std::size_t>(backend_.max_owners()) + 1) {
        backend_.attach(*this);
        sessions_.reserve(config.max_connections);
        free_.reserve(config.max_connections);
        for (ConnectionId i = 0; i < config.max_connections; ++i) {
            sessions_.emplace_back(config.input_buffer_bytes, config.output_buffer_bytes);
        }
        for (ConnectionId i = config.max_connections; i > 0; --i) {
            free_.push_back(i - 1);  // So open() hands out 0, 1, 2, ... first.
        }
    }

    // The usual way to build the single-threaded session layer, whose InlineBackend only
    // needs the order entry configuration.
    BasicSessionManager(const SessionConfig& config, const protocol::OrderEntryConfig& order_entry)
        requires std::constructible_from<Backend, const protocol::OrderEntryConfig&>
        : BasicSessionManager(config, std::in_place, order_entry) {}

    // The backend holds a reference to this object, so it must never move.
    BasicSessionManager(const BasicSessionManager&) = delete;
    BasicSessionManager& operator=(const BasicSessionManager&) = delete;
    BasicSessionManager(BasicSessionManager&&) = delete;
    BasicSessionManager& operator=(BasicSessionManager&&) = delete;
    ~BasicSessionManager() override = default;

    // Startup calls.

    // Adds an account and returns its OwnerId (1, 2, 3, ...). Throws
    // std::invalid_argument for a bad or duplicate username or a bad password, and
    // std::length_error when every account number is taken.
    OwnerId add_account(const AccountConfig& account) {
        if (!is_name(account.username, soup::kUsernameLength) ||
            !is_name(account.password, soup::kPasswordLength)) {
            throw std::invalid_argument(
                "username must be 1 to 6 and password 1 to 10 letters or digits");
        }
        const auto username = upper(padded_right<soup::kUsernameLength>(account.username));
        if (find_account(username) != kNoOwner) {
            throw std::invalid_argument("duplicate username");
        }
        if (account_count_ + 1 >= accounts_.size()) {
            throw std::length_error("no account number left (raise OrderEntryConfig::max_owners)");
        }
        const auto owner = static_cast<OwnerId>(++account_count_);
        Account& a = accounts_[owner];
        a.username = username;
        a.password = upper(padded_right<soup::kPasswordLength>(account.password));
        a.cancel_on_disconnect = account.cancel_on_disconnect;
        backend_.set_risk_limits(owner, account.limits);
        return owner;
    }

    SymbolId add_symbol(Ticker ticker, const BookConfig& config) {
        return backend_.add_symbol(ticker, config);
    }

    // Sets the number the account's next sequenced message gets. Recovery calls it after
    // replaying the journal, with 1 plus the replies the account had before the restart,
    // so the numbers carry on where the clients left off. This is a startup call. Throws
    // std::invalid_argument for 0, std::out_of_range for an owner that is not an account,
    // and std::logic_error while the account has a connection.
    void restore_next_sequence(OwnerId owner, std::uint64_t next) {
        Account& a = account_at(owner);
        if (next == 0) {
            throw std::invalid_argument("sequence numbers start at 1");
        }
        if (a.connection != kNoConnection) {
            throw std::logic_error(
                "an account's sequence can only be restored while it is logged out");
        }
        a.next_sequence = next;
    }

    // Connection events, called by the network code.

    // A new connection arrived. Returns its id, or kNoConnection if every slot is in use,
    // in which case the caller closes the socket at once.
    ConnectionId open(Timestamp now) noexcept {
        if (free_.empty()) {
            ++stats_.connections_refused;
            return kNoConnection;
        }
        const ConnectionId id = free_.back();
        free_.pop_back();
        sessions_[id].open(now);
        ++open_connections_;
        return id;
    }

    // Bytes arrived on a connection. They may hold any number of packets, and the first
    // or last packet may be cut off. A cut off packet waits in the input buffer for the
    // rest of its bytes.
    //
    // Returns how many of the bytes were taken. That is fewer than all of them only when
    // processing is paused for backpressure and the input buffer is full, and the caller
    // must then offer the rest again later. Reading at most receive_room(id) bytes from
    // the socket avoids having any left over. Bytes for a connection that is closing or
    // not open are thrown away and count as taken.
    std::size_t receive(ConnectionId id, std::span<const std::byte> bytes, Timestamp now) {
        if (!is_open(id) || sessions_[id].closing()) {
            return bytes.size();
        }
        if (bytes.empty()) {
            return 0;
        }
        Session& s = sessions_[id];
        now_ = now;
        s.note_received(now);
        std::size_t taken = 0;
        while (taken < bytes.size() && !s.closing()) {
            const std::size_t n = s.input().append_some(bytes.subspan(taken));
            taken += n;
            process_input(id, s);
            // process_input() removes every whole packet unless it is paused, and the
            // input buffer holds at least one whole packet, so no room means paused.
            if (n == 0) {
                break;
            }
        }
        return s.closing() ? bytes.size() : taken;
    }

    // How many bytes receive() would take right now. It is 0 while the connection is
    // paused with a full input buffer, and the network code can then stop reading the
    // socket until sent() has made room.
    [[nodiscard]] std::size_t receive_room(ConnectionId id) const noexcept {
        if (!is_open(id) || sessions_[id].closing()) {
            return 0;
        }
        return sessions_[id].input().free_space();
    }

    // Sends heartbeats that are due and closes connections that timed out. Call this
    // regularly, for example every 100 milliseconds.
    void tick(Timestamp now) noexcept {
        now_ = now;
        for (Session& s : sessions_) {
            if (!s.in_use() || s.closing()) {
                continue;
            }
            if (s.state() == SessionState::AwaitingLogin) {
                if (elapsed(s.opened_at(), now) >= config_.login_timeout) {
                    s.close(CloseReason::LoginTimeout);
                }
                continue;
            }
            if (elapsed(s.last_received(), now) >= config_.idle_timeout) {
                s.close(CloseReason::IdleTimeout);
                continue;
            }
            if (elapsed(s.last_sent(), now) >= config_.heartbeat_interval) {
                if (s.send_packet(soup::PacketType::ServerHeartbeat, {}, now)) {
                    ++stats_.heartbeats_out;
                }
            }
        }
    }

    // Ends the trading session. Every logged in client gets an End of Session packet, and
    // every connection is marked for closing. Later logins are refused.
    void end_session(Timestamp now) noexcept {
        now_ = now;
        ended_ = true;
        for (Session& s : sessions_) {
            if (!s.in_use() || s.closing()) {
                continue;
            }
            if (s.state() == SessionState::LoggedIn) {
                s.send_packet(soup::PacketType::EndOfSession, {}, now);
            }
            s.close(CloseReason::EndOfSession);
        }
    }

    // The bytes waiting to be written to the connection's socket.
    [[nodiscard]] std::span<const std::byte> output(ConnectionId id) const noexcept {
        if (!is_open(id)) {
            return {};
        }
        return sessions_[id].output().readable();
    }

    // The network code wrote the first n bytes of output(id) at time `now`. If input was
    // waiting because of backpressure, it is processed now, as far as the new room
    // allows.
    void sent(ConnectionId id, std::size_t n, Timestamp now) {
        if (!is_open(id)) {
            return;
        }
        Session& s = sessions_[id];
        s.output().consume(n);
        if (!s.closing() && !s.input().empty()) {
            now_ = now;
            process_input(id, s);
        }
    }

    // True once the session has decided to close. The network code should write out
    // what is left in output() if it can, and then call close().
    [[nodiscard]] bool should_close(ConnectionId id) const noexcept {
        return is_open(id) && sessions_[id].closing();
    }

    // The connection is gone. `reason` is used only if the session had not already
    // decided why it closed (a peer that hangs up is PeerClosed). If the account asked
    // for cancel on disconnect, its live orders are canceled now.
    void close(ConnectionId id, CloseReason reason, Timestamp now) {
        if (!is_open(id)) {
            return;
        }
        now_ = now;
        Session& s = sessions_[id];
        s.close(reason);
        ++stats_.closes[static_cast<std::size_t>(s.close_reason())];
        const OwnerId owner = s.owner();
        s.release();
        free_.push_back(id);
        --open_connections_;
        if (owner == kNoOwner) {
            return;
        }
        Account& a = accounts_[owner];
        if (a.connection != id) {
            return;
        }
        // The account is detached first, so the cancel messages below are counted as
        // dropped instead of being queued on a connection that no longer exists.
        a.connection = kNoConnection;
        if (a.cancel_on_disconnect && !backend_.cancel_all(owner, now)) {
            // The backend's queue is full. retry_waiting() sends the cancel later, and
            // until then the account's input is held back, so no new order of the
            // account can overtake its cancel.
            a.cancel_pending = true;
            retry_needed_ = true;
            ++stats_.cancels_deferred;
        }
    }

    // Replies from an asynchronous backend, called by the code that drains its queue.

    // A reply for `owner` arrived. It gets the account's next sequence number and goes
    // to the account's connection, or is counted as dropped if there is none.
    void deliver(OwnerId owner, std::span<const std::byte> frame, Timestamp now) {
        now_ = now;
        on_reply(owner, frame);
    }

    // The backend finished one request of `owner`, and every reply to it has been
    // delivered. If the connection was held back by requests in flight, its waiting
    // input is processed now.
    void request_done(OwnerId owner, Timestamp now) {
        if (owner == kNoOwner || owner > account_count_) {
            return;
        }
        Account& a = accounts_[owner];
        if (a.in_flight > 0) {
            --a.in_flight;
            if (owner == sample_owner_) {
                end_sample_if_done();
            }
        }
        if (a.connection == kNoConnection) {
            return;
        }
        Session& s = sessions_[a.connection];
        if (!s.closing() && !s.input().empty()) {
            now_ = now;
            process_input(a.connection, s);
        }
    }

    // Offers again whatever had to wait because the backend was busy, first the deferred
    // cancels on disconnect and then the waiting input of every connection. It does
    // nothing unless something had to wait, so it is cheap to call after every drain of
    // the reply queue.
    void retry_waiting(Timestamp now) {
        if (!retry_needed_) {
            return;
        }
        retry_needed_ = false;
        now_ = now;
        for (std::size_t owner = 1; owner <= account_count_; ++owner) {
            Account& a = accounts_[owner];
            if (!a.cancel_pending) {
                continue;
            }
            if (!backend_.cancel_all(static_cast<OwnerId>(owner), now)) {
                retry_needed_ = true;
                return;  // Still busy. Input must not overtake the cancel, so stop here.
            }
            a.cancel_pending = false;
        }
        for (ConnectionId id = 0; id < sessions_.size(); ++id) {
            Session& s = sessions_[id];
            if (s.in_use() && !s.closing() && !s.input().empty()) {
                process_input(id, s);
            }
        }
    }

    // Queries.

    [[nodiscard]] bool is_open(ConnectionId id) const noexcept {
        return id < sessions_.size() && sessions_[id].in_use();
    }
    // Throws std::out_of_range for an id that is not a slot.
    [[nodiscard]] const Session& session(ConnectionId id) const { return sessions_.at(id); }
    [[nodiscard]] std::uint32_t open_connections() const noexcept { return open_connections_; }
    [[nodiscard]] std::uint32_t max_connections() const noexcept {
        return static_cast<std::uint32_t>(sessions_.size());
    }
    [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }
    // The sampled times in nanoseconds (see SessionConfig::sample_every). With an inline
    // backend a sample is the forward() call itself, during which the replies are written.
    // With an asynchronous backend it ends when request_done() reports the order finished.
    [[nodiscard]] const metrics::LatencyHistogram& request_latency() const noexcept {
        return request_latency_;
    }
    [[nodiscard]] const Backend& backend() const noexcept { return backend_; }
    [[nodiscard]] Backend& backend() noexcept { return backend_; }
    // Only for a backend that owns the order entry layer, such as InlineBackend.
    [[nodiscard]] const auto& order_entry() const noexcept
        requires requires(const Backend& b) { b.order_entry(); }
    {
        return backend_.order_entry();
    }
    // Throws std::out_of_range for an owner that is not an account.
    [[nodiscard]] std::uint32_t in_flight(OwnerId owner) const {
        return account_at(owner).in_flight;
    }
    [[nodiscard]] bool cancel_pending(OwnerId owner) const {
        return account_at(owner).cancel_pending;
    }
    // True while a packet or a cancel on disconnect waits for room at the backend, until
    // retry_waiting() gets it through.
    [[nodiscard]] bool waiting_for_backend() const noexcept { return retry_needed_; }
    // Throws std::out_of_range for an owner that is not an account.
    [[nodiscard]] std::uint64_t next_sequence(OwnerId owner) const {
        return account_at(owner).next_sequence;
    }
    [[nodiscard]] ConnectionId connection_of(OwnerId owner) const {
        return account_at(owner).connection;
    }
    // How many accounts were added. Their OwnerIds are 1 to this number.
    [[nodiscard]] std::size_t account_count() const noexcept { return account_count_; }
    [[nodiscard]] bool cancels_on_disconnect(OwnerId owner) const {
        return account_at(owner).cancel_on_disconnect;
    }

private:
    // Everything kept per account. Unlike a Session, this outlives any one connection.
    struct Account {
        soup::Username username{};  // Upper case, padded with spaces.
        soup::Password password{};  // Upper case, padded with spaces.
        bool cancel_on_disconnect = false;
        std::uint64_t next_sequence = 1;  // The number the next sequenced message gets.
        ConnectionId connection = kNoConnection;
        // Requests forwarded to an asynchronous backend and not reported done yet.
        std::uint32_t in_flight = 0;
        // A cancel on disconnect is waiting for room in the backend's queue.
        bool cancel_pending = false;
    };

    // The most bytes one reply can take in an output buffer, a Sequenced Data packet with
    // the longest OUCH message.
    static constexpr std::size_t kLargestReply = soup::kHeaderSize + protocol::kMaxMessageLength;

    static SessionConfig checked(const SessionConfig& config) {
        if (!is_name(config.session_name, soup::kSessionLength)) {
            throw std::invalid_argument("session name must be 1 to 10 letters or digits");
        }
        if (config.max_connections == 0 || config.max_connections == kNoConnection) {
            throw std::invalid_argument("max_connections out of range");
        }
        // The input buffer must hold the largest packet, and the output buffer the
        // largest packet the server sends (a Sequenced Data with the longest OUCH reply).
        constexpr std::size_t kLargestOut = soup::kHeaderSize + protocol::kMaxMessageLength;
        if (config.input_buffer_bytes < soup::kLengthFieldSize + kMaxClientPacket ||
            config.output_buffer_bytes < kLargestOut) {
            throw std::invalid_argument("connection buffers are too small for one packet");
        }
        // Below the high water mark there must be room for at least one more reply, or a
        // connection could pause with nothing to write and never resume.
        if (config.output_high_water == 0 ||
            config.output_high_water > config.output_buffer_bytes - kLargestOut) {
            throw std::invalid_argument("output_high_water must leave room for one packet");
        }
        if (config.heartbeat_interval == 0 || config.idle_timeout == 0 ||
            config.login_timeout == 0) {
            throw std::invalid_argument("time limits must be above 0");
        }
        if (config.max_in_flight == 0) {
            throw std::invalid_argument("max_in_flight must be above 0");
        }
        return config;
    }

    // True if `text` is 1 to max_length ASCII letters or digits.
    static bool is_name(std::string_view text, std::size_t max_length) noexcept {
        if (text.empty() || text.size() > max_length) {
            return false;
        }
        return std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        });
    }

    // The padded field for a startup string. The caller has checked its length already,
    // so the throw is only a safety net.
    template <std::size_t N>
    static std::array<char, N> padded_right(std::string_view text) {
        const auto field = soup::pad_right<N>(text);
        if (!field) {
            throw std::invalid_argument("text too long for its field");
        }
        return *field;
    }

    template <std::size_t N>
    static std::array<char, N> padded_left(std::string_view text) {
        const auto field = soup::pad_left<N>(text);
        if (!field) {
            throw std::invalid_argument("text too long for its field");
        }
        return *field;
    }

    static char upper(char c) noexcept {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }

    template <std::size_t N>
    static std::array<char, N> upper(std::array<char, N> field) noexcept {
        for (char& c : field) {
            c = upper(c);
        }
        return field;
    }

    static Timestamp elapsed(Timestamp since, Timestamp now) noexcept {
        return now > since ? now - since : 0;
    }

    const Account& account_at(OwnerId owner) const {
        if (owner == kNoOwner || owner > account_count_) {
            throw std::out_of_range("not an account");
        }
        return accounts_[owner];
    }
    Account& account_at(OwnerId owner) {
        if (owner == kNoOwner || owner > account_count_) {
            throw std::out_of_range("not an account");
        }
        return accounts_[owner];
    }

    // Finds an account by its upper case username. Logins are rare and there are few
    // accounts, so a linear scan is simpler than another hash table.
    OwnerId find_account(const soup::Username& username) const noexcept {
        for (std::size_t owner = 1; owner <= account_count_; ++owner) {
            if (accounts_[owner].username == username) {
                return static_cast<OwnerId>(owner);
            }
        }
        return kNoOwner;
    }

    // True while the connection's input waits because its output is backed up. With an
    // asynchronous backend, each request in flight reserves room for one reply, because
    // its replies are still on the way. The account's input also waits while it has too
    // many requests in flight or a cancel on disconnect that is not queued yet.
    bool paused(const Session& s) const noexcept {
        if constexpr (Backend::kAsynchronous) {
            if (s.owner() != kNoOwner) {
                const Account& a = accounts_[s.owner()];
                if (a.cancel_pending || a.in_flight >= config_.max_in_flight) {
                    return true;
                }
                const std::size_t reserved = static_cast<std::size_t>(a.in_flight) * kLargestReply;
                return s.output().size() + reserved >= config_.output_high_water;
            }
        }
        return s.output().size() >= config_.output_high_water;
    }

    // Handles the whole packets in the session's input buffer, until there are none left
    // or the output backs up, and leaves the rest there for later.
    void process_input(ConnectionId id, Session& s) {
        while (!s.closing() && !paused(s)) {
            const soup::Packet packet = soup::next_packet(s.input().readable(), kMaxClientPacket);
            if (packet.status == soup::PacketStatus::Incomplete) {
                return;
            }
            if (packet.status == soup::PacketStatus::BadLength) {
                s.close(CloseReason::BadLength);
                return;
            }
            const bool handled =
                handle_packet(id, s, static_cast<soup::PacketType>(static_cast<char>(packet.type)),
                              packet.payload);
            if (!handled) {
                return;  // The backend was busy. The packet stays for retry_waiting().
            }
            ++stats_.packets_in;
            // The payload points into the input buffer, so it is only removed after it
            // has been handled.
            s.input().consume(packet.size);
        }
    }

    static bool is_client_packet(soup::PacketType type) noexcept {
        switch (type) {
            case soup::PacketType::Debug:
            case soup::PacketType::LoginRequest:
            case soup::PacketType::UnsequencedData:
            case soup::PacketType::ClientHeartbeat:
            case soup::PacketType::LogoutRequest:
                return true;
            default:
                return false;
        }
    }

    // Handles one whole packet. Returns false only if the packet must wait because the
    // backend is busy, and then nothing has changed.
    bool handle_packet(ConnectionId id, Session& s, soup::PacketType type,
                       std::span<const std::byte> payload) {
        if (!is_client_packet(type)) {
            s.close(CloseReason::UnknownPacket);
            return true;
        }
        if (type == soup::PacketType::Debug) {
            return true;  // The specification says to ignore debug packets.
        }
        if (s.state() == SessionState::AwaitingLogin) {
            if (type == soup::PacketType::LoginRequest) {
                handle_login(id, s, payload);
            } else {
                s.close(CloseReason::NotLoggedIn);
            }
            return true;
        }
        switch (type) {
            case soup::PacketType::UnsequencedData:
                return forward_order(s, payload);
            case soup::PacketType::ClientHeartbeat:
                return true;  // Its only job was to show the client is alive.
            case soup::PacketType::LogoutRequest:
                s.close(CloseReason::Logout);
                return true;
            default:
                s.close(CloseReason::UnexpectedLogin);
                return true;
        }
    }

    // Passes one OUCH message to the backend. Returns false if the backend is busy.
    bool forward_order(Session& s, std::span<const std::byte> payload) {
        const bool timed = sample_due();
        std::uint64_t start = 0;
        if (timed) {
            start = metrics::TickClock::now();
        }
        const ForwardResult result = backend_.forward(s.owner(), payload, now_);
        if (result == ForwardResult::Busy) {
            ++stats_.backend_busy;
            retry_needed_ = true;
            return false;
        }
        if (result == ForwardResult::Malformed) {
            s.close(CloseReason::MalformedMessage);
            return true;
        }
        const OwnerId owner = s.owner();
        if (config_.sample_every != 0) {
            ++orders_since_sample_;
        }
        if (timed) {
            orders_since_sample_ = 0;
            if constexpr (Backend::kAsynchronous) {
                // Requests of one account finish in the order they were sent, so this one
                // is done after the ones already in flight.
                sample_owner_ = owner;
                sample_dones_left_ = accounts_[owner].in_flight + 1;
                sample_start_ = start;
            } else {
                record_sample(start);  // The replies were written during forward().
            }
        }
        if constexpr (Backend::kAsynchronous) {
            ++accounts_[owner].in_flight;
        }
        return true;
    }

    // True if the next order taken by the backend should be timed.
    [[nodiscard]] bool sample_due() const noexcept {
        if (config_.sample_every == 0 || sample_owner_ != kNoOwner) {
            return false;
        }
        return orders_since_sample_ + 1 >= config_.sample_every;
    }

    // Called when one request of the sampled account finished.
    void end_sample_if_done() noexcept {
        --sample_dones_left_;
        if (sample_dones_left_ == 0) {
            record_sample(sample_start_);
            sample_owner_ = kNoOwner;
        }
    }

    void record_sample(std::uint64_t start) noexcept {
        const std::uint64_t ticks = metrics::TickClock::now() - start;
        request_latency_.record(metrics::TickClock::to_ns(ticks));
    }

    void handle_login(ConnectionId id, Session& s, std::span<const std::byte> payload) {
        const auto request = soup::decode_login_request(payload);
        if (!request) {
            s.close(CloseReason::BadLogin);
            return;
        }
        const OwnerId owner = find_account(upper(request->username));
        if (owner == kNoOwner || accounts_[owner].password != upper(request->password)) {
            reject_login(s, soup::LoginRejectReason::NotAuthorized);
            return;
        }
        Account& a = accounts_[owner];
        // These are the cases where the account is real but the session cannot start. The
        // exchange has ended the session, the client asked for some other session, the
        // account is already logged in on another connection, or the client asked to
        // start before the next message, which would need messages that were already
        // produced. Replaying old messages comes with the journal in Phase 9.
        const std::string_view wanted = soup::trimmed(request->session);
        const bool wrong_session = !wanted.empty() && wanted != soup::trimmed(session_name_);
        const bool asks_for_old = request->sequence != 0 && request->sequence < a.next_sequence;
        if (ended_ || wrong_session || a.connection != kNoConnection || asks_for_old) {
            reject_login(s, soup::LoginRejectReason::SessionNotAvailable);
            return;
        }
        std::array<std::byte, soup::kHeaderSize + soup::kLoginAcceptedPayload> packet{};
        const std::size_t n = soup::encode_login_accepted(
            soup::LoginAccepted{.session = session_name_, .sequence = a.next_sequence}, packet);
        if (!s.send_bytes(std::span<const std::byte>(packet.data(), n), now_)) {
            return;  // The output buffer is full, so the session is already closing.
        }
        s.log_in(owner);
        a.connection = id;
        ++stats_.logins_accepted;
    }

    void reject_login(Session& s, soup::LoginRejectReason reason) {
        std::array<std::byte, soup::kHeaderSize + soup::kLoginRejectedPayload> packet{};
        const std::size_t n = soup::encode_login_rejected(reason, packet);
        s.send_bytes(std::span<const std::byte>(packet.data(), n), now_);
        s.close(CloseReason::LoginRejected);
        ++stats_.logins_rejected;
    }

    // Every reply comes through here, straight from an inline backend or through
    // deliver(). Each reply gets the account's next sequence number, whether or not a
    // connection can take it.
    void on_reply(OwnerId owner, std::span<const std::byte> frame) override {
        Account& a = accounts_[owner];
        ++a.next_sequence;
        ++stats_.messages_out;
        if (a.connection == kNoConnection ||
            !sessions_[a.connection].send_packet(soup::PacketType::SequencedData, frame, now_)) {
            ++stats_.messages_dropped;
        }
    }

    SessionConfig config_;
    soup::SessionName session_name_;  // Padded on the left, as sent in Login Accepted.
    Backend backend_;
    std::vector<Account> accounts_;  // Indexed by OwnerId. Slot 0 is unused.
    std::size_t account_count_ = 0;
    std::vector<Session> sessions_;   // Indexed by ConnectionId.
    std::vector<ConnectionId> free_;  // Free connection slots, used as a stack.
    std::uint32_t open_connections_ = 0;
    SessionStats stats_;
    Timestamp now_ = 0;  // The time of the call being handled.
    bool ended_ = false;
    // Something had to wait for the backend, so retry_waiting() has work to do.
    bool retry_needed_ = false;
    // Sampling (see SessionConfig::sample_every).
    metrics::LatencyHistogram request_latency_;
    std::uint32_t orders_since_sample_ = 0;
    // The account of the sample being timed with an asynchronous backend, or kNoOwner.
    OwnerId sample_owner_ = kNoOwner;
    std::uint32_t sample_dones_left_ = 0;  // request_done() calls until it is finished.
    std::uint64_t sample_start_ = 0;       // In TickClock ticks.
};

// The single-threaded session layer of Phase 6, which owns the order entry layer.
template <OrderBook Book>
using SessionManager = BasicSessionManager<InlineBackend<Book>>;

}  // namespace litbook::gateway
