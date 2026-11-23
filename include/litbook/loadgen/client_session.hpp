#pragma once

// The client side of one SoupBinTCP session, without the socket. The owner reads bytes
// from its socket into input(), calls process_input() to turn whole packets into OUCH
// replies, and writes whatever output() holds to the socket. Keeping the protocol rules
// apart from the socket is the same split the server uses (DECISIONS.md D-044), and it
// lets the tests feed bytes one at a time or many packets at once.
//
// The session starts by queuing a Login Request (begin). After Login Accepted it is
// Active, and send() queues order entry messages. Every Sequenced Data packet must hold
// one outbound OUCH message that decodes, and the session counts them, which is the
// sequence number the server would give the next one. A Server Heartbeat changes nothing.
// End of Session ends the session normally. Login Rejected, a packet a server may not
// send, a bad length or a message that does not decode make it Failed, and the reason
// says which. A client heartbeat is queued when nothing was sent for heartbeat_interval.
//
// Both buffers are allocated in the constructor and never grow. Nothing after that
// allocates or throws.
//
// These invariants always hold. Once the state is Ended or Failed it never changes again,
// and process_input() and send() do nothing. failure() is None unless the state is Failed.

#include "litbook/gateway/byte_buffer.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace litbook::loadgen {

enum class SessionState : std::uint8_t {
    LoggingIn,  // The Login Request is queued or sent, and no answer has come yet.
    Active,     // Logged in.
    Ended,      // The server sent End of Session, or the client logged out.
    Failed,     // Something went wrong, see failure().
};

enum class SessionFailure : std::uint8_t {
    None,
    NotAuthorized,        // Login Rejected A.
    SessionNotAvailable,  // Login Rejected S.
    BadPacket,            // A length field of 0 or above the limit, or a bad login reply.
    UnexpectedPacket,     // A packet type the server may not send now.
    BadMessage,           // Sequenced Data that is not one outbound OUCH message.
    InputFull,            // A packet bigger than the input buffer.
    Closed,               // The socket closed before End of Session (set by the owner).
};

[[nodiscard]] const char* to_string(SessionFailure f) noexcept;

struct ClientSessionConfig {
    std::size_t input_bytes = std::size_t{64} * 1024;
    std::size_t output_bytes = std::size_t{64} * 1024;
    std::uint64_t heartbeat_interval_ns = 1'000'000'000;
};

class ClientSession {
public:
    // Throws std::invalid_argument if the username or password is too long for its field.
    ClientSession(std::string_view username, std::string_view password,
                  const ClientSessionConfig& config = {});

    // Queues the Login Request, which asks for the current session from now on.
    void begin(std::uint64_t now_ns) noexcept;

    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] SessionFailure failure() const noexcept { return failure_; }
    [[nodiscard]] bool active() const noexcept { return state_ == SessionState::Active; }
    [[nodiscard]] bool finished() const noexcept {
        return state_ == SessionState::Ended || state_ == SessionState::Failed;
    }

    // The bytes read from the socket go into input(), and the bytes in output() go out.
    [[nodiscard]] gateway::ByteBuffer& input() noexcept { return input_; }
    [[nodiscard]] gateway::ByteBuffer& output() noexcept { return output_; }

    // Handles every whole packet in input(). Calls on_reply(const OutboundMessage&) for each
    // OUCH reply, in order. Stops at the first packet that makes the session fail.
    template <class OnReply>
    void process_input(OnReply&& on_reply) {
        while (!finished()) {
            const gateway::soup::Packet p =
                gateway::soup::next_packet(input_.readable(), gateway::soup::kMaxLengthField);
            if (p.status == gateway::soup::PacketStatus::BadLength) {
                fail(SessionFailure::BadPacket);
                return;
            }
            if (p.status == gateway::soup::PacketStatus::Incomplete) {
                if (input_.free_space() == 0) {
                    fail(SessionFailure::InputFull);
                }
                return;
            }
            const auto type = static_cast<gateway::soup::PacketType>(static_cast<char>(p.type));
            if (type == gateway::soup::PacketType::SequencedData && active()) {
                const protocol::Decoded<protocol::OutboundMessage> d =
                    protocol::decode_outbound(p.payload);
                if (!d.message) {
                    fail(SessionFailure::BadMessage);
                    return;
                }
                ++replies_;
                input_.consume(p.size);
                on_reply(*d.message);
                continue;
            }
            handle_control(type, p.payload);
            input_.consume(p.size);
        }
    }

    // True if send() would take any order entry message now.
    [[nodiscard]] bool can_send() const noexcept {
        return active() &&
               output_.free_space() >= gateway::soup::kHeaderSize + protocol::kMaxMessageLength;
    }

    // Queues one order entry message. Returns false and queues nothing if the session is
    // not Active or the output buffer has no room.
    bool send(const protocol::InboundMessage& message, std::uint64_t now_ns) noexcept;

    // Queues a Client Heartbeat if the session is Active and nothing was queued for
    // heartbeat_interval.
    void tick(std::uint64_t now_ns) noexcept;

    // Queues a Logout Request and ends the session. The server closes the connection when
    // it reads it.
    void logout(std::uint64_t now_ns) noexcept;

    // For the owner, when the socket closed or broke.
    void fail(SessionFailure f) noexcept;

    // The sequence number the server gave in Login Accepted.
    [[nodiscard]] std::uint64_t first_sequence() const noexcept { return first_sequence_; }
    // Sequenced messages received since the login.
    [[nodiscard]] std::uint64_t replies() const noexcept { return replies_; }
    [[nodiscard]] std::uint64_t heartbeats_sent() const noexcept { return heartbeats_sent_; }

private:
    void handle_control(gateway::soup::PacketType type,
                        std::span<const std::byte> payload) noexcept;
    bool queue(gateway::soup::PacketType type, std::span<const std::byte> payload,
               std::uint64_t now_ns) noexcept;

    gateway::soup::LoginRequest login_;
    ClientSessionConfig config_;
    gateway::ByteBuffer input_;
    gateway::ByteBuffer output_;
    SessionState state_ = SessionState::LoggingIn;
    SessionFailure failure_ = SessionFailure::None;
    std::uint64_t last_queued_ns_ = 0;
    std::uint64_t first_sequence_ = 0;
    std::uint64_t replies_ = 0;
    std::uint64_t heartbeats_sent_ = 0;
};

}  // namespace litbook::loadgen
