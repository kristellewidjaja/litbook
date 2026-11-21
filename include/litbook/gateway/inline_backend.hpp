#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/order_backend.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <span>

namespace litbook::gateway {

// The order backend of the single-threaded gateway. It owns the order entry layer (and so
// the matching engine) and handles each request right away on the calling thread. Every
// reply goes straight to the session layer through ReplyTarget::on_reply, before
// forward() or cancel_all() returns. This is exactly how Phase 6 worked.
//
// Invariant: after attach(), every reply the order entry layer produces reaches the
// target. attach() must be called before the first request.
template <OrderBook Book>
class InlineBackend final : private protocol::OutboundSink {
public:
    static constexpr bool kAsynchronous = false;

    // Throws whatever OrderEntry throws for a bad configuration.
    explicit InlineBackend(const protocol::OrderEntryConfig& config)
        : order_entry_(*this, config) {}

    // The order entry layer holds a reference to this object, so it must never move.
    InlineBackend(const InlineBackend&) = delete;
    InlineBackend& operator=(const InlineBackend&) = delete;
    InlineBackend(InlineBackend&&) = delete;
    InlineBackend& operator=(InlineBackend&&) = delete;
    ~InlineBackend() override = default;

    void attach(ReplyTarget& target) noexcept { target_ = &target; }

    ForwardResult forward(OwnerId owner, std::span<const std::byte> frame,
                          protocol::Timestamp now) {
        const protocol::DecodeStatus status = order_entry_.on_frame(owner, frame, now);
        if (status != protocol::DecodeStatus::Ok) {
            return ForwardResult::Malformed;
        }
        return ForwardResult::Taken;
    }

    // The orders are canceled before this returns, so it never reports busy.
    bool cancel_all(OwnerId owner, protocol::Timestamp now) {
        order_entry_.cancel_all(owner, now);
        return true;
    }

    void set_risk_limits(OwnerId owner, const protocol::RiskLimits& limits) {
        order_entry_.set_risk_limits(owner, limits);
    }

    SymbolId add_symbol(Ticker ticker, const BookConfig& config) {
        return order_entry_.add_symbol(ticker, config);
    }

    [[nodiscard]] OwnerId max_owners() const noexcept { return order_entry_.config().max_owners; }

    [[nodiscard]] const protocol::OrderEntry<Book>& order_entry() const noexcept {
        return order_entry_;
    }

private:
    void send(OwnerId owner, std::span<const std::byte> frame) override {
        target_->on_reply(owner, frame);
    }

    protocol::OrderEntry<Book> order_entry_;
    ReplyTarget* target_ = nullptr;
};

}  // namespace litbook::gateway
