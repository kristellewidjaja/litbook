#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/session.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
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
    std::array<std::uint64_t, kCloseReasonCount> closes{};  // Indexed by CloseReason.

    [[nodiscard]] std::uint64_t closed(CloseReason reason) const noexcept {
        return closes[static_cast<std::size_t>(reason)];
    }
};

// The SoupBinTCP session layer in front of the order entry layer. It knows nothing about
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
// Every connection slot and buffer is allocated in the constructor, so handling bytes,
// ticks and closes never allocates.
//
// These invariants hold between calls. An account's connection, if it has one, is an
// open session that is logged in as that account (or closing after being logged in).
// Every account's next_sequence() is 1 plus the number of sequenced messages produced
// for it. open_connections() is the number of sessions in use.
template <OrderBook Book>
class SessionManager final : private protocol::OutboundSink {
public:
    using ConnectionId = std::uint32_t;
    static constexpr ConnectionId kNoConnection = std::numeric_limits<ConnectionId>::max();

    // Packets longer than this (the value of the length field) close the connection. It
    // is the size of a Login Request, the largest packet a client needs to send.
    static constexpr std::size_t kMaxClientPacket = 1 + soup::kLoginRequestPayload;

    // Throws std::invalid_argument for a bad session name, zero connections, buffers too
    // small to hold one packet, or a zero time limit, and whatever OrderEntry throws.
    SessionManager(const SessionConfig& config, const protocol::OrderEntryConfig& order_entry)
        : config_(checked(config)),
          session_name_(padded_left<soup::kSessionLength>(config.session_name)),
          order_entry_(*this, order_entry),
          accounts_(static_cast<std::size_t>(order_entry.max_owners) + 1) {
        sessions_.reserve(config.max_connections);
        free_.reserve(config.max_connections);
        for (ConnectionId i = 0; i < config.max_connections; ++i) {
            sessions_.emplace_back(config.input_buffer_bytes, config.output_buffer_bytes);
        }
        for (ConnectionId i = config.max_connections; i > 0; --i) {
            free_.push_back(i - 1);  // So open() hands out 0, 1, 2, ... first.
        }
    }

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&) = delete;
    SessionManager& operator=(SessionManager&&) = delete;
    ~SessionManager() override = default;

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
        order_entry_.set_risk_limits(owner, account.limits);
        return owner;
    }

    SymbolId add_symbol(Ticker ticker, const BookConfig& config) {
        return order_entry_.add_symbol(ticker, config);
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
        if (a.cancel_on_disconnect) {
            order_entry_.cancel_all(owner, now);
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
    [[nodiscard]] const protocol::OrderEntry<Book>& order_entry() const noexcept {
        return order_entry_;
    }
    // Throws std::out_of_range for an owner that is not an account.
    [[nodiscard]] std::uint64_t next_sequence(OwnerId owner) const {
        return account_at(owner).next_sequence;
    }
    [[nodiscard]] ConnectionId connection_of(OwnerId owner) const {
        return account_at(owner).connection;
    }

private:
    // Everything kept per account. Unlike a Session, this outlives any one connection.
    struct Account {
        soup::Username username{};  // Upper case, padded with spaces.
        soup::Password password{};  // Upper case, padded with spaces.
        bool cancel_on_disconnect = false;
        std::uint64_t next_sequence = 1;  // The number the next sequenced message gets.
        ConnectionId connection = kNoConnection;
    };

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

    // True while the connection's input waits because its output is backed up.
    bool paused(const Session& s) const noexcept {
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
            ++stats_.packets_in;
            handle_packet(id, s, static_cast<soup::PacketType>(static_cast<char>(packet.type)),
                          packet.payload);
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

    void handle_packet(ConnectionId id, Session& s, soup::PacketType type,
                       std::span<const std::byte> payload) {
        if (!is_client_packet(type)) {
            s.close(CloseReason::UnknownPacket);
            return;
        }
        if (type == soup::PacketType::Debug) {
            return;  // The specification says to ignore debug packets.
        }
        if (s.state() == SessionState::AwaitingLogin) {
            if (type == soup::PacketType::LoginRequest) {
                handle_login(id, s, payload);
            } else {
                s.close(CloseReason::NotLoggedIn);
            }
            return;
        }
        switch (type) {
            case soup::PacketType::UnsequencedData:
                if (order_entry_.on_frame(s.owner(), payload, now_) != protocol::DecodeStatus::Ok) {
                    s.close(CloseReason::MalformedMessage);
                }
                return;
            case soup::PacketType::ClientHeartbeat:
                return;  // Its only job was to show the client is alive.
            case soup::PacketType::LogoutRequest:
                s.close(CloseReason::Logout);
                return;
            default:
                s.close(CloseReason::UnexpectedLogin);
                return;
        }
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

    // The order entry layer calls this for every reply. Each reply gets the account's
    // next sequence number, whether or not a connection can take it.
    void send(OwnerId owner, std::span<const std::byte> frame) override {
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
    protocol::OrderEntry<Book> order_entry_;
    std::vector<Account> accounts_;  // Indexed by OwnerId. Slot 0 is unused.
    std::size_t account_count_ = 0;
    std::vector<Session> sessions_;   // Indexed by ConnectionId.
    std::vector<ConnectionId> free_;  // Free connection slots, used as a stack.
    std::uint32_t open_connections_ = 0;
    SessionStats stats_;
    Timestamp now_ = 0;  // The time of the call being handled.
    bool ended_ = false;
};

}  // namespace litbook::gateway
