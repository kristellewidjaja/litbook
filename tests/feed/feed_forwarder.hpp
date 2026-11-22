#pragma once

#include "litbook/feed/feed_publisher.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <cstddef>
#include <span>

namespace litbook::feed::testing {

// Turns the order entry layer's market data callbacks into the publisher's events, the
// same way the matching thread does in the pipeline.
class FeedForwarder final : public protocol::MarketDataSink {
public:
    explicit FeedForwarder(FeedPublisher& publisher) : publisher_(&publisher) {}

    void set_now(Timestamp now) noexcept { now_ = now; }

    void on_trade(SymbolId symbol, const Trade& trade) override {
        pipeline::MarketEvent e;
        e.now = now_;
        e.kind = pipeline::MarketEventKind::Trade;
        e.side = trade.aggressor_side;
        e.symbol = symbol;
        e.price = trade.price;
        e.qty = trade.qty;
        e.match_id = trade.match_id;
        publisher_->on_event(e);
    }

    void on_level_update(SymbolId symbol, const LevelUpdate& update) override {
        pipeline::MarketEvent e;
        e.now = now_;
        e.kind = pipeline::MarketEventKind::LevelUpdate;
        e.side = update.side;
        e.symbol = symbol;
        e.order_count = update.order_count;
        e.price = update.price;
        e.qty = update.total_qty;
        publisher_->on_event(e);
    }

private:
    FeedPublisher* publisher_;
    Timestamp now_ = 0;
};

// Throws away the order entry replies, which these tests do not look at.
class DiscardReplies final : public protocol::OutboundSink {
public:
    void send(OwnerId /*owner*/, std::span<const std::byte> /*frame*/) override {}
};

}  // namespace litbook::feed::testing
