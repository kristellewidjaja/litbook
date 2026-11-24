#include "litbook/client/order_tracker.hpp"

#include <variant>

namespace litbook::client {

void OrderTracker::on_reply(const protocol::OutboundMessage& reply) {
    if (const auto* a = std::get_if<protocol::AcceptedMessage>(&reply)) {
        if (a->qty > 0) {
            open_.insert_or_assign(a->user_ref, OpenOrder{.symbol = a->symbol,
                                                          .side = a->side,
                                                          .price = a->price,
                                                          .open = a->qty,
                                                          .executed = 0,
                                                          .order_ref = a->order_ref});
        }
    } else if (const auto* e = std::get_if<protocol::ExecutedMessage>(&reply)) {
        take_off(e->user_ref, e->qty, true);
    } else if (const auto* c = std::get_if<protocol::CanceledMessage>(&reply)) {
        take_off(c->user_ref, c->qty, false);
    } else if (const auto* r = std::get_if<protocol::ReplacedMessage>(&reply)) {
        const auto it = open_.find(r->orig_user_ref);
        if (it == open_.end()) {
            return;
        }
        OpenOrder order = it->second;
        open_.erase(it);
        order.price = r->price;
        order.open = r->qty;
        order.order_ref = r->order_ref;
        if (order.open > 0) {
            open_.insert_or_assign(r->user_ref, order);
        }
    }
    // Rejected changes nothing. A failed replace is followed by its own Order Canceled.
}

void OrderTracker::take_off(protocol::UserRefNum ref, Qty qty, bool executed) {
    const auto it = open_.find(ref);
    if (it == open_.end()) {
        return;
    }
    OpenOrder& order = it->second;
    if (executed) {
        order.executed += qty;
    }
    // The exchange never takes off more than is open, but a clamp keeps the count sane
    // if a reply ever says otherwise.
    order.open = qty >= order.open ? 0 : order.open - qty;
    if (order.open == 0) {
        open_.erase(it);
    }
}

const OpenOrder* OrderTracker::find(protocol::UserRefNum ref) const noexcept {
    const auto it = open_.find(ref);
    return it == open_.end() ? nullptr : &it->second;
}

bool answers(const Awaited& request, const protocol::OutboundMessage& reply) noexcept {
    if (const auto* r = std::get_if<protocol::RejectedMessage>(&reply)) {
        return request.kind != RequestKind::Cancel && r->user_ref == request.ref;
    }
    switch (request.kind) {
        case RequestKind::Enter: {
            const auto* a = std::get_if<protocol::AcceptedMessage>(&reply);
            return a != nullptr && a->user_ref == request.ref;
        }
        case RequestKind::Replace: {
            const auto* r = std::get_if<protocol::ReplacedMessage>(&reply);
            return r != nullptr && r->user_ref == request.ref && r->orig_user_ref == request.orig;
        }
        case RequestKind::Cancel: {
            const auto* c = std::get_if<protocol::CanceledMessage>(&reply);
            return c != nullptr && c->user_ref == request.ref &&
                   c->reason == protocol::WireCancelReason::UserRequested;
        }
    }
    return false;
}

bool gone(const Awaited& request, const OrderTracker& tracker) noexcept {
    switch (request.kind) {
        case RequestKind::Enter:
            return false;
        case RequestKind::Replace:
            return tracker.find(request.orig) == nullptr;
        case RequestKind::Cancel:
            return tracker.find(request.ref) == nullptr;
    }
    return false;
}

}  // namespace litbook::client
