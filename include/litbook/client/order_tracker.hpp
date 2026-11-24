#pragma once

// The orders one client account has open, kept up to date from the exchange's replies
// alone. The blocking order client (order_client.hpp) feeds it every reply it reads, in
// order, and asks it whether an order it wants to cancel or replace is still open.
//
// An order becomes open with Order Accepted, which carries its full size. Order Executed
// and Order Canceled take shares off, and an order whose open shares reach 0 is gone.
// Order Replaced moves the order to its new UserRefNum with the open shares the reply
// states, keeping the shares it already executed. Rejected changes nothing, because a
// failed replace is followed by an Order Canceled for the original order (PROTOCOL.md
// session rule 6). A reply about an order the tracker never saw accepted (for example one
// entered on an earlier connection of the account) is ignored.
//
// The second half of this file says which reply answers a request. The exchange drops a
// Cancel or Replace that names an order which is no longer open without any reply (session
// rules 5 and 6, D-069). One account's replies come in the order the exchange handled its
// requests, so the replies that closed such an order always arrive before the exchange
// even looks at the request. That is why a client can stop waiting as soon as the order
// it named is gone from the tracker.
//
// Invariants. Every order in the tracker has open > 0. A UserRefNum names at most one open
// order. Nothing here touches a socket or a clock.

#include "litbook/engine/ticker.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace litbook::client {

struct OpenOrder {
    Ticker symbol;
    Side side = Side::Buy;
    Price price = 0;
    Qty open = 0;      // Shares that can still trade.
    Qty executed = 0;  // Shares that traded so far, across replaces.
    protocol::OrderRef order_ref = 0;
};

class OrderTracker {
public:
    // Updates the open orders from one reply. Call it for every reply, in order.
    void on_reply(const protocol::OutboundMessage& reply);

    // The open order with this UserRefNum, or nullptr if it is not open.
    [[nodiscard]] const OpenOrder* find(protocol::UserRefNum ref) const noexcept;
    [[nodiscard]] std::size_t open_count() const noexcept { return open_.size(); }
    [[nodiscard]] const std::unordered_map<protocol::UserRefNum, OpenOrder>& orders()
        const noexcept {
        return open_;
    }

private:
    // Takes shares off an order and forgets it when nothing is left open.
    void take_off(protocol::UserRefNum ref, Qty qty, bool executed);

    std::unordered_map<protocol::UserRefNum, OpenOrder> open_;
};

enum class RequestKind : std::uint8_t { Enter, Replace, Cancel };

// A request the client sent and now waits on. For a Replace, `orig` is the order it
// replaces and `ref` the new UserRefNum. For an Enter or a Cancel, `orig` is unused.
struct Awaited {
    RequestKind kind = RequestKind::Enter;
    protocol::UserRefNum ref = 0;
    protocol::UserRefNum orig = 0;
};

// True if this reply is the answer to the request. An Enter is answered by Order Accepted
// or Rejected for its UserRefNum. A Replace is answered by Order Replaced that names both
// numbers, or by Rejected for the new number. A Cancel is answered by Order Canceled for
// its order with reason U.
[[nodiscard]] bool answers(const Awaited& request, const protocol::OutboundMessage& reply) noexcept;

// True if the request can no longer get an answer, because the order it names is not open
// any more. An Enter always gets an answer, so this is false for it.
[[nodiscard]] bool gone(const Awaited& request, const OrderTracker& tracker) noexcept;

}  // namespace litbook::client
