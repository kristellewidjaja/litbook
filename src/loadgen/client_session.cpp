#include "litbook/loadgen/client_session.hpp"

#include <optional>
#include <stdexcept>

namespace litbook::loadgen {

namespace soup = gateway::soup;

const char* to_string(SessionFailure f) noexcept {
    switch (f) {
        case SessionFailure::None:
            return "none";
        case SessionFailure::NotAuthorized:
            return "login rejected, not authorized";
        case SessionFailure::SessionNotAvailable:
            return "login rejected, session not available";
        case SessionFailure::BadPacket:
            return "bad packet";
        case SessionFailure::UnexpectedPacket:
            return "unexpected packet";
        case SessionFailure::BadMessage:
            return "bad message";
        case SessionFailure::InputFull:
            return "input buffer full";
        case SessionFailure::Closed:
            return "connection closed";
    }
    return "unknown";
}

namespace {

soup::LoginRequest checked_login(std::string_view username, std::string_view password) {
    const std::optional<soup::LoginRequest> login =
        soup::make_login_request(username, password, "", 0);
    if (!login) {
        throw std::invalid_argument("username or password too long for SoupBinTCP");
    }
    return *login;
}

}  // namespace

ClientSession::ClientSession(std::string_view username, std::string_view password,
                             const ClientSessionConfig& config)
    : login_(checked_login(username, password)),
      config_(config),
      input_(config.input_bytes),
      output_(config.output_bytes) {}

void ClientSession::begin(std::uint64_t now_ns) noexcept {
    std::array<std::byte, soup::kHeaderSize + soup::kLoginRequestPayload> packet{};
    const std::size_t n = soup::encode_login_request(login_, packet);
    if (n == 0 || !output_.make_room(n)) {
        fail(SessionFailure::BadPacket);
        return;
    }
    (void)output_.append_some(std::span<const std::byte>(packet.data(), n));
    last_queued_ns_ = now_ns;
}

bool ClientSession::send(const protocol::InboundMessage& message, std::uint64_t now_ns) noexcept {
    if (!active()) {
        return false;
    }
    std::array<std::byte, protocol::kMaxMessageLength> frame{};
    const std::size_t n = protocol::encode(message, frame);
    return queue(soup::PacketType::UnsequencedData, std::span<const std::byte>(frame.data(), n),
                 now_ns);
}

void ClientSession::tick(std::uint64_t now_ns) noexcept {
    if (active() && now_ns - last_queued_ns_ >= config_.heartbeat_interval_ns &&
        queue(soup::PacketType::ClientHeartbeat, {}, now_ns)) {
        ++heartbeats_sent_;
    }
}

void ClientSession::logout(std::uint64_t now_ns) noexcept {
    if (finished()) {
        return;
    }
    if (active()) {
        (void)queue(soup::PacketType::LogoutRequest, {}, now_ns);
    }
    state_ = SessionState::Ended;
}

void ClientSession::fail(SessionFailure f) noexcept {
    if (finished()) {
        return;
    }
    state_ = SessionState::Failed;
    failure_ = f;
}

void ClientSession::handle_control(soup::PacketType type,
                                   std::span<const std::byte> payload) noexcept {
    switch (type) {
        case soup::PacketType::Debug:
        case soup::PacketType::ServerHeartbeat:
            return;
        case soup::PacketType::LoginAccepted:
            if (state_ == SessionState::LoggingIn) {
                if (const auto accepted = soup::decode_login_accepted(payload)) {
                    first_sequence_ = accepted->sequence;
                    state_ = SessionState::Active;
                } else {
                    fail(SessionFailure::BadPacket);
                }
                return;
            }
            break;
        case soup::PacketType::LoginRejected:
            if (state_ == SessionState::LoggingIn) {
                const auto reason = soup::decode_login_rejected(payload);
                if (!reason) {
                    fail(SessionFailure::BadPacket);
                } else if (*reason == soup::LoginRejectReason::NotAuthorized) {
                    fail(SessionFailure::NotAuthorized);
                } else {
                    fail(SessionFailure::SessionNotAvailable);
                }
                return;
            }
            break;
        case soup::PacketType::EndOfSession:
            if (active()) {
                state_ = SessionState::Ended;
                return;
            }
            break;
        default:
            break;
    }
    // Sequenced Data before the login, a second login answer, End of Session before the
    // login, or a packet type only a client may send.
    fail(SessionFailure::UnexpectedPacket);
}

bool ClientSession::queue(soup::PacketType type, std::span<const std::byte> payload,
                          std::uint64_t now_ns) noexcept {
    const std::size_t n = soup::kHeaderSize + payload.size();
    if (!output_.make_room(n)) {
        return false;
    }
    (void)soup::encode_packet(type, payload, output_.writable());
    output_.commit(n);
    last_queued_ns_ = now_ns;
    return true;
}

}  // namespace litbook::loadgen
