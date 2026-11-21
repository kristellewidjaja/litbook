#pragma once

// Helpers shared by the pipeline tests. They build OUCH frames for orders and decode the
// frames that come back.

#include "litbook/engine/ticker.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/types.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "support/unwrap.hpp"

namespace litbook::pipeline::testing {

using Bytes = std::vector<std::byte>;

inline Ticker aapl() {
    return litbook::testing::unwrap(make_ticker("AAPL"));
}

inline protocol::EnterOrder limit(Side side, protocol::UserRefNum ref, Qty qty, Price price) {
    return protocol::EnterOrder{.user_ref = ref,
                                .side = side,
                                .qty = qty,
                                .symbol = aapl(),
                                .price = price,
                                .tif = TimeInForce::GoodTillCancel,
                                .type = OrderType::Limit,
                                .post_only = false};
}

// The encoded OUCH bytes of one inbound message.
inline Bytes frame_of(const protocol::InboundMessage& message) {
    std::array<std::byte, protocol::kMaxMessageLength> buffer{};
    const std::size_t n = protocol::encode(message, buffer);
    EXPECT_GT(n, 0U);
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n)};
}

inline Request request_of(OwnerId owner, const protocol::InboundMessage& message,
                          protocol::Timestamp now = 0) {
    return make_frame_request(owner, frame_of(message), now);
}

// The OUCH message inside a Message reply, or nothing for other replies.
inline std::optional<protocol::OutboundMessage> decoded(const Reply& reply) {
    if (reply.kind != ReplyKind::Message) {
        return std::nullopt;
    }
    return protocol::decode_outbound(reply.bytes()).message;
}

// The message of type Message inside a reply. The test fails (by an exception) if the
// reply holds something else.
template <class Message>
Message message_of(const Reply& reply) {
    return std::get<Message>(litbook::testing::unwrap(decoded(reply)));
}

template <class Message>
bool holds(const Reply& reply) {
    const auto m = decoded(reply);
    return m && std::holds_alternative<Message>(*m);
}

// Pops everything a single-threaded test's queue holds.
template <class Queue>
std::vector<typename Queue::value_type> drain_all(Queue& queue) {
    std::vector<typename Queue::value_type> out;
    typename Queue::value_type value;
    while (queue.try_pop(value)) {
        out.push_back(value);
    }
    return out;
}

}  // namespace litbook::pipeline::testing
