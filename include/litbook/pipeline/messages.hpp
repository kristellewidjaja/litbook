#pragma once

#include "litbook/engine/engine_listener.hpp"
#include "litbook/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace litbook::pipeline {

// The messages that travel between the threads of the pipeline. Each one is a small
// fixed-size struct that is copied into a queue slot, so nothing is ever allocated when a
// message moves, and each fits in one 64-byte cache line. The gateway thread sends
// Request to the matching thread, the matching thread answers with Reply, and it sends
// MarketEvent to the publisher thread.

// Nanoseconds since midnight, the same type as Timestamp. This header has its
// own name for it so it does not need the OUCH message definitions, and queue_backend.hpp
// checks that the two agree.
using Timestamp = std::uint64_t;

// The largest OUCH message in either direction (protocol::kMaxMessageLength, checked in
// queue_backend.hpp).
inline constexpr std::size_t kMaxFrame = 47;

enum class RequestKind : std::uint8_t {
    Frame,      // One inbound OUCH message from an account, already checked to decode.
    CancelAll,  // Cancel every live order of the account (cancel on disconnect).
    Stop,       // The last request. The matching thread answers with Reply Stopped.
};

// From the gateway thread to the matching thread. The gateway puts the time on each
// request, so the matching thread never reads a clock and the order of this queue alone
// decides what happens. Phase 9 will journal exactly this stream.
struct Request {
    Timestamp now = 0;
    OwnerId owner = kNoOwner;
    RequestKind kind = RequestKind::Frame;
    std::uint8_t length = 0;  // How many bytes of frame are used.
    std::array<std::byte, kMaxFrame> frame{};

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {frame.data(), length};
    }
};

enum class ReplyKind : std::uint8_t {
    Message,      // One encoded outbound OUCH message for the account.
    RequestDone,  // Every reply to one Frame request of the account has been sent.
    Stopped,      // The matching thread handled Stop and is about to exit.
};

// From the matching thread to the gateway thread.
struct Reply {
    OwnerId owner = kNoOwner;
    ReplyKind kind = ReplyKind::Message;
    std::uint8_t length = 0;
    std::array<std::byte, kMaxFrame> frame{};

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {frame.data(), length};
    }
};

enum class MarketEventKind : std::uint8_t {
    Trade,
    LevelUpdate,
    Stop,  // The last event. The publisher thread exits after it.
};

// From the matching thread to the publisher thread. A trade fills price, qty, side (the
// aggressor's side) and match_id. A level update fills side, price, qty (the level's
// total) and order_count.
struct MarketEvent {
    MarketEventKind kind = MarketEventKind::Trade;
    Side side = Side::Buy;
    SymbolId symbol = 0;
    std::uint32_t order_count = 0;
    Price price = 0;
    AggregateQty qty = 0;
    MatchId match_id = 0;
};

static_assert(sizeof(Request) <= 64, "a request should fit in one 64-byte line");
static_assert(sizeof(Reply) <= 64, "a reply should fit in one 64-byte line");
static_assert(sizeof(MarketEvent) <= 64, "a market event should fit in one 64-byte line");

// Builds a Frame request. The caller makes sure frame is at most kMaxFrame bytes, and any
// extra bytes are cut off.
[[nodiscard]] inline Request make_frame_request(OwnerId owner, std::span<const std::byte> frame,
                                                Timestamp now) noexcept {
    Request r;
    r.now = now;
    r.owner = owner;
    r.kind = RequestKind::Frame;
    const std::size_t n = std::min(frame.size(), kMaxFrame);
    std::copy_n(frame.begin(), n, r.frame.begin());
    r.length = static_cast<std::uint8_t>(n);
    return r;
}

[[nodiscard]] inline Request make_cancel_all_request(OwnerId owner, Timestamp now) noexcept {
    Request r;
    r.now = now;
    r.owner = owner;
    r.kind = RequestKind::CancelAll;
    return r;
}

[[nodiscard]] inline Request make_stop_request(Timestamp now) noexcept {
    Request r;
    r.now = now;
    r.kind = RequestKind::Stop;
    return r;
}

// Builds a Message reply, with the same rule about the length as make_frame_request.
[[nodiscard]] inline Reply make_message_reply(OwnerId owner,
                                              std::span<const std::byte> frame) noexcept {
    Reply r;
    r.owner = owner;
    r.kind = ReplyKind::Message;
    const std::size_t n = std::min(frame.size(), kMaxFrame);
    std::copy_n(frame.begin(), n, r.frame.begin());
    r.length = static_cast<std::uint8_t>(n);
    return r;
}

[[nodiscard]] inline Reply make_done_reply(OwnerId owner) noexcept {
    Reply r;
    r.owner = owner;
    r.kind = ReplyKind::RequestDone;
    return r;
}

[[nodiscard]] inline Reply make_stopped_reply() noexcept {
    Reply r;
    r.kind = ReplyKind::Stopped;
    return r;
}

}  // namespace litbook::pipeline
