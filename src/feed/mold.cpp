#include "litbook/feed/mold.hpp"

#include "litbook/protocol/byte_order.hpp"

#include <ostream>
#include <stdexcept>

namespace litbook::feed {

namespace {

using protocol::load_be;
using protocol::store_be;

constexpr std::size_t kSequenceOffset = 10;
constexpr std::size_t kCountOffset = 18;
static_assert(kCountOffset + 2 == kHeaderLength);

bool is_alnum(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

}  // namespace

std::optional<SessionName> make_session_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kSessionLength) {
        return std::nullopt;
    }
    for (const char c : name) {
        if (!is_alnum(c)) {
            return std::nullopt;
        }
    }
    SessionName out{};
    out.fill(' ');
    const std::size_t pad = kSessionLength - name.size();
    for (std::size_t i = 0; i < name.size(); ++i) {
        out.at(pad + i) = name[i];
    }
    return out;
}

CheckedPacket check_packet(std::span<const std::byte> packet) noexcept {
    CheckedPacket result;
    if (packet.size() < kHeaderLength) {
        result.status = PacketStatus::TooShort;
        return result;
    }
    for (std::size_t i = 0; i < kSessionLength; ++i) {
        result.header.session.at(i) = static_cast<char>(packet[i]);
    }
    result.header.sequence = load_be<std::uint64_t>(packet, kSequenceOffset);
    result.header.count = load_be<std::uint16_t>(packet, kCountOffset);

    // Walk the blocks the count promises. Every block must fit inside the packet and hold
    // a message that decodes.
    std::size_t offset = kHeaderLength;
    for (std::uint16_t i = 0; i < result.header.messages(); ++i) {
        if (packet.size() - offset < kBlockLengthSize) {
            result.status = PacketStatus::Truncated;
            return result;
        }
        const std::size_t length = load_be<std::uint16_t>(packet, offset);
        offset += kBlockLengthSize;
        if (packet.size() - offset < length) {
            result.status = PacketStatus::Truncated;
            return result;
        }
        const FeedDecoded decoded = decode_message(packet.subspan(offset, length));
        if (decoded.status != FeedDecodeStatus::Ok) {
            result.status = PacketStatus::BadMessage;
            result.message_status = decoded.status;
            return result;
        }
        offset += length;
    }
    result.status = offset == packet.size() ? PacketStatus::Ok : PacketStatus::TrailingBytes;
    return result;
}

BlockReader::BlockReader(std::span<const std::byte> packet) noexcept : packet_(packet) {
    if (packet.size() >= kHeaderLength) {
        PacketHeader header;
        header.count = load_be<std::uint16_t>(packet, kCountOffset);
        left_ = header.messages();
    }
}

bool BlockReader::next(std::span<const std::byte>& message) noexcept {
    if (left_ == 0 || packet_.size() - offset_ < kBlockLengthSize) {
        return false;
    }
    const std::size_t length = load_be<std::uint16_t>(packet_, offset_);
    offset_ += kBlockLengthSize;
    if (packet_.size() - offset_ < length) {
        left_ = 0;
        return false;
    }
    message = packet_.subspan(offset_, length);
    offset_ += length;
    --left_;
    return true;
}

PacketBuilder::PacketBuilder(const SessionName& session, std::size_t limit) : limit_(limit) {
    if (limit < kMinPacketLength || limit > kMaxPacketLength) {
        throw std::invalid_argument(
            "packet limit must be between kMinPacketLength and kMaxPacketLength");
    }
    for (std::size_t i = 0; i < kSessionLength; ++i) {
        buffer_.at(i) = static_cast<std::byte>(session.at(i));
    }
    start(1);
}

void PacketBuilder::start(Sequence first) noexcept {
    first_ = first;
    size_ = kHeaderLength;
    count_ = 0;
}

void PacketBuilder::write_block_length(std::size_t length) noexcept {
    store_be<std::uint16_t>(buffer_, size_, static_cast<std::uint16_t>(length));
}

std::span<const std::byte> PacketBuilder::finish() noexcept {
    store_be<std::uint64_t>(buffer_, kSequenceOffset, first_);
    store_be<std::uint16_t>(buffer_, kCountOffset, count_);
    return std::span<const std::byte>(buffer_).first(size_);
}

std::span<const std::byte> PacketBuilder::control(Sequence next, std::uint16_t count) noexcept {
    start(next);
    store_be<std::uint64_t>(buffer_, kSequenceOffset, next);
    store_be<std::uint16_t>(buffer_, kCountOffset, count);
    return std::span<const std::byte>(buffer_).first(kHeaderLength);
}

std::span<const std::byte> PacketBuilder::heartbeat(Sequence next) noexcept {
    return control(next, kHeartbeatCount);
}

std::span<const std::byte> PacketBuilder::end_of_session(Sequence next) noexcept {
    return control(next, kEndOfSessionCount);
}

std::ostream& operator<<(std::ostream& os, PacketStatus status) {
    switch (status) {
        case PacketStatus::Ok:
            return os << "Ok";
        case PacketStatus::TooShort:
            return os << "TooShort";
        case PacketStatus::Truncated:
            return os << "Truncated";
        case PacketStatus::TrailingBytes:
            return os << "TrailingBytes";
        case PacketStatus::BadMessage:
            return os << "BadMessage";
    }
    return os << "PacketStatus(" << static_cast<int>(status) << ")";
}

}  // namespace litbook::feed
