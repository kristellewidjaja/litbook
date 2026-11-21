#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/types.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace litbook::gateway {

// The session layer as an order backend sees it. A backend that handles requests on the
// calling thread hands every reply to on_reply() before forward() returns. The frame is an
// encoded OUCH message and is only valid during the call.
class ReplyTarget {
public:
    virtual ~ReplyTarget() = default;
    virtual void on_reply(OwnerId owner, std::span<const std::byte> frame) = 0;

protected:
    ReplyTarget() = default;
    ReplyTarget(const ReplyTarget&) = default;
    ReplyTarget(ReplyTarget&&) = default;
    ReplyTarget& operator=(const ReplyTarget&) = default;
    ReplyTarget& operator=(ReplyTarget&&) = default;
};

// What happened to one inbound OUCH frame that the session layer passed on.
enum class ForwardResult : std::uint8_t {
    Taken,      // The backend has the request. Its replies come now or later.
    Malformed,  // The frame did not decode. The session layer closes the connection.
    Busy,       // The backend has no room right now. Offer the same frame again later.
};

// Where the session layer sends the orders it receives. There are two kinds.
//
// InlineBackend (inline_backend.hpp) owns the order entry layer and handles each request
// on the calling thread, so every reply has arrived by the time forward() returns. That
// is the single-threaded gateway of Phase 6, and kAsynchronous is false.
//
// QueueBackend (pipeline/queue_backend.hpp) checks the frame and pushes it onto a queue
// for the matching thread. The replies come back later through the session layer's
// deliver(), and each request ends with request_done(). kAsynchronous is true, which
// tells the session layer to count requests in flight for backpressure.
//
// add_symbol and set_risk_limits are startup calls. cancel_all returns false when the
// backend is busy, and the session layer then tries again later.
template <class B>
concept OrderBackend =
    requires(B& backend, const B& const_backend, ReplyTarget& target, OwnerId owner,
             std::span<const std::byte> frame, protocol::Timestamp now, Ticker ticker,
             const BookConfig& book, const protocol::RiskLimits& limits) {
        { B::kAsynchronous } -> std::convertible_to<bool>;
        backend.attach(target);
        { backend.forward(owner, frame, now) } -> std::same_as<ForwardResult>;
        { backend.cancel_all(owner, now) } -> std::same_as<bool>;
        backend.set_risk_limits(owner, limits);
        { backend.add_symbol(ticker, book) } -> std::same_as<SymbolId>;
        { const_backend.max_owners() } -> std::same_as<OwnerId>;
    };

}  // namespace litbook::gateway
