#include "litbook/gateway/session.hpp"

#include <ostream>

namespace litbook::gateway {

std::ostream& operator<<(std::ostream& os, CloseReason reason) {
    switch (reason) {
        case CloseReason::None:
            return os << "None";
        case CloseReason::PeerClosed:
            return os << "PeerClosed";
        case CloseReason::Logout:
            return os << "Logout";
        case CloseReason::LoginRejected:
            return os << "LoginRejected";
        case CloseReason::LoginTimeout:
            return os << "LoginTimeout";
        case CloseReason::IdleTimeout:
            return os << "IdleTimeout";
        case CloseReason::BadLength:
            return os << "BadLength";
        case CloseReason::UnknownPacket:
            return os << "UnknownPacket";
        case CloseReason::NotLoggedIn:
            return os << "NotLoggedIn";
        case CloseReason::UnexpectedLogin:
            return os << "UnexpectedLogin";
        case CloseReason::BadLogin:
            return os << "BadLogin";
        case CloseReason::MalformedMessage:
            return os << "MalformedMessage";
        case CloseReason::OutputFull:
            return os << "OutputFull";
        case CloseReason::EndOfSession:
            return os << "EndOfSession";
    }
    return os << "CloseReason(" << static_cast<int>(reason) << ")";
}

Session::Session(std::size_t input_capacity, std::size_t output_capacity)
    : input_(input_capacity), output_(output_capacity) {}

void Session::open(Timestamp now) noexcept {
    input_.clear();
    output_.clear();
    in_use_ = true;
    state_ = SessionState::AwaitingLogin;
    owner_ = kNoOwner;
    close_reason_ = CloseReason::None;
    opened_at_ = now;
    last_received_ = now;
    last_sent_ = now;
}

void Session::release() noexcept {
    input_.clear();
    output_.clear();
    in_use_ = false;
}

void Session::log_in(OwnerId owner) noexcept {
    owner_ = owner;
    state_ = SessionState::LoggedIn;
}

void Session::close(CloseReason reason) noexcept {
    if (state_ == SessionState::Closing) {
        return;
    }
    state_ = SessionState::Closing;
    close_reason_ = reason;
}

bool Session::send_packet(soup::PacketType type, std::span<const std::byte> payload,
                          Timestamp now) noexcept {
    if (closing()) {
        return false;
    }
    const std::size_t size = soup::kHeaderSize + payload.size();
    if (!output_.make_room(size)) {
        close(CloseReason::OutputFull);
        return false;
    }
    const std::size_t written = soup::encode_packet(type, payload, output_.writable());
    if (written == 0) {
        // Only a payload too long for the length field gets here, and no caller sends one.
        return false;
    }
    output_.commit(written);
    last_sent_ = now;
    return true;
}

bool Session::send_bytes(std::span<const std::byte> packet, Timestamp now) noexcept {
    if (closing()) {
        return false;
    }
    if (!output_.append(packet)) {
        close(CloseReason::OutputFull);
        return false;
    }
    last_sent_ = now;
    return true;
}

}  // namespace litbook::gateway
