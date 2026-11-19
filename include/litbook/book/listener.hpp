#pragma once

#include "litbook/book/events.hpp"

namespace litbook {

// Receives every event an order book emits, synchronously, in the order described
// in events.hpp. Default implementations ignore the event, so a listener overrides
// only what it needs.
//
// Invariant: a listener must not call back into the book that is notifying it
// (the book is mid-operation and its state is not yet consistent).
class OrderBookListener {
public:
    virtual ~OrderBookListener() = default;

    virtual void on_accepted(const OrderAccepted& /*event*/) {}
    virtual void on_rejected(const OrderRejected& /*event*/) {}
    virtual void on_trade(const Trade& /*event*/) {}
    virtual void on_canceled(const OrderCanceled& /*event*/) {}
    virtual void on_cancel_rejected(const CancelRejected& /*event*/) {}
    virtual void on_level_update(const LevelUpdate& /*event*/) {}

protected:
    OrderBookListener() = default;
    OrderBookListener(const OrderBookListener&) = default;
    OrderBookListener(OrderBookListener&&) = default;
    OrderBookListener& operator=(const OrderBookListener&) = default;
    OrderBookListener& operator=(OrderBookListener&&) = default;
};

}  // namespace litbook
