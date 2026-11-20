// Fuzz target for the order entry codec.
//
// The first input byte picks the direction (its lowest bit, 0 for inbound and 1 for
// outbound) and the rest of the input is one frame. The decoder must never crash or read
// outside the frame (the sanitizers catch that). When a frame decodes, three more things
// must hold, and the target aborts if any of them fails.
//
// 1. The frame has exactly the documented length for its type.
// 2. Encoding the decoded message gives back exactly the same bytes. This is the
//    canonical encoding property, so two different frames never mean the same message.
// 3. Decoding those bytes again gives the same message.

#include "litbook/protocol/codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

#include "fuzz_target.hpp"

namespace litbook::fuzz {

namespace {

void check(bool ok) {
    if (!ok) {
        std::abort();
    }
}

template <class Message, class Decode, class Length>
void check_frame(std::span<const std::byte> frame, Decode decode, Length length) {
    const auto decoded = decode(frame);
    check(decoded.message.has_value() == (decoded.status == protocol::DecodeStatus::Ok));
    if (!decoded.message) {
        return;
    }
    check(frame.size() == length(frame[0]));
    std::array<std::byte, protocol::kMaxMessageLength> buffer{};
    const std::size_t n = protocol::encode(*decoded.message, buffer);
    check(n == frame.size());
    check(std::memcmp(buffer.data(), frame.data(), n) == 0);
    const auto again = decode(std::span<const std::byte>(buffer.data(), n));
    check(again.message == decoded.message);
}

}  // namespace

int run_one(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const bool outbound = (data[0] & 1U) != 0;
    const std::span<const std::byte> frame(reinterpret_cast<const std::byte*>(data) + 1, size - 1);
    if (outbound) {
        check_frame<protocol::OutboundMessage>(
            frame, [](auto f) { return protocol::decode_outbound(f); },
            [](std::byte t) { return protocol::outbound_length(t); });
    } else {
        check_frame<protocol::InboundMessage>(
            frame, [](auto f) { return protocol::decode_inbound(f); },
            [](std::byte t) { return protocol::inbound_length(t); });
    }
    return 0;
}

}  // namespace litbook::fuzz
