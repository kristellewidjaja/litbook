#pragma once

#include "litbook/gateway/byte_buffer.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>

namespace litbook::gateway {

// Nanoseconds. It is the same type as protocol::Timestamp, which session_manager.hpp
// checks, so a time read from the clock can go straight into an outbound message.
using Timestamp = std::uint64_t;

// Why a connection was closed. The first reason recorded is the one that is kept.
enum class CloseReason : std::uint8_t {
    None,              // The connection is still open.
    PeerClosed,        // The client closed the socket, or the socket failed.
    Logout,            // The client sent a Logout Request.
    LoginRejected,     // The login was refused, and a Login Rejected was sent.
    LoginTimeout,      // No Login Request arrived in time.
    IdleTimeout,       // Nothing arrived from a logged in client for too long.
    BadLength,         // A packet length was 0 or above the limit.
    UnknownPacket,     // The packet type is not one a client may send.
    NotLoggedIn,       // A client packet other than a login arrived before the login.
    UnexpectedLogin,   // A second Login Request arrived on a logged in connection.
    BadLogin,          // The Login Request had the wrong length or a bad number.
    MalformedMessage,  // An OUCH message inside Unsequenced Data did not decode.
    OutputFull,        // The client did not read fast enough and its output buffer filled.
    EndOfSession,      // The exchange ended the session.
};
inline constexpr std::size_t kCloseReasonCount = 14;

std::ostream& operator<<(std::ostream& os, CloseReason reason);

enum class SessionState : std::uint8_t {
    AwaitingLogin,  // Connected, but no successful login yet.
    LoggedIn,       // Logged in as an account. OUCH messages may flow.
    Closing,        // Decided to close. Input is ignored and nothing new is queued.
};

// The state of one client connection. It holds the connection's input and output byte
// queues, its protocol state, its account once it has logged in, and the times used for
// heartbeats and timeouts. It knows how to queue a SoupBinTCP packet. The protocol
// rules themselves are in SessionManager.
//
// Sessions are created once, at startup, and reused for one connection after another
// through open() and release(), so the buffers are never reallocated.
//
// These invariants hold between calls. owner() is kNoOwner unless the session logged in.
// close_reason() is None exactly when the state is not Closing. Only whole packets are
// ever queued in output().
class Session {
public:
    // Throws std::invalid_argument for a zero buffer size.
    Session(std::size_t input_capacity, std::size_t output_capacity);

    // Starts a new connection in this slot.
    void open(Timestamp now) noexcept;
    // Frees the slot after the connection is gone.
    void release() noexcept;

    void log_in(OwnerId owner) noexcept;

    // Moves to Closing. If the session is already closing, the first reason is kept.
    void close(CloseReason reason) noexcept;

    // Records that bytes arrived at time `now`.
    void note_received(Timestamp now) noexcept { last_received_ = now; }

    // Queues one packet with the given type and payload. Returns false if the session is
    // closing, or if the output queue has no room, in which case the session closes with
    // OutputFull. A slow client is disconnected rather than letting its queue grow.
    bool send_packet(soup::PacketType type, std::span<const std::byte> payload,
                     Timestamp now) noexcept;
    // Queues a packet that is already encoded, with the same rules as send_packet().
    bool send_bytes(std::span<const std::byte> packet, Timestamp now) noexcept;

    [[nodiscard]] bool in_use() const noexcept { return in_use_; }
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] bool closing() const noexcept { return state_ == SessionState::Closing; }
    [[nodiscard]] OwnerId owner() const noexcept { return owner_; }
    [[nodiscard]] CloseReason close_reason() const noexcept { return close_reason_; }
    [[nodiscard]] Timestamp opened_at() const noexcept { return opened_at_; }
    [[nodiscard]] Timestamp last_received() const noexcept { return last_received_; }
    [[nodiscard]] Timestamp last_sent() const noexcept { return last_sent_; }

    [[nodiscard]] ByteBuffer& input() noexcept { return input_; }
    [[nodiscard]] const ByteBuffer& input() const noexcept { return input_; }
    [[nodiscard]] ByteBuffer& output() noexcept { return output_; }
    [[nodiscard]] const ByteBuffer& output() const noexcept { return output_; }

private:
    ByteBuffer input_;
    ByteBuffer output_;
    bool in_use_ = false;
    SessionState state_ = SessionState::AwaitingLogin;
    OwnerId owner_ = kNoOwner;
    CloseReason close_reason_ = CloseReason::None;
    Timestamp opened_at_ = 0;
    Timestamp last_received_ = 0;
    Timestamp last_sent_ = 0;
};

}  // namespace litbook::gateway
