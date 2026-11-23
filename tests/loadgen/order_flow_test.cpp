#include "litbook/loadgen/order_flow.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <random>
#include <stdexcept>
#include <variant>
#include <vector>

#include "support/unwrap.hpp"

namespace litbook::loadgen {
namespace {

FlowConfig config_with(std::uint32_t cancel, std::uint32_t replace, std::uint32_t aggressive) {
    FlowConfig c;
    c.cancel_percent = cancel;
    c.replace_percent = replace;
    c.aggressive_percent = aggressive;
    return c;
}

const protocol::EnterOrder& as_enter(const FlowRequest& r) {
    return std::get<protocol::EnterOrder>(r.message);
}

// The Order Accepted the exchange would send for an Enter Order.
protocol::OutboundMessage accept(const FlowRequest& r) {
    const protocol::EnterOrder& e = as_enter(r);
    return protocol::AcceptedMessage{.timestamp = 0,
                                     .user_ref = e.user_ref,
                                     .side = e.side,
                                     .qty = e.qty,
                                     .symbol = e.symbol,
                                     .price = e.price,
                                     .tif = e.tif,
                                     .type = e.type,
                                     .post_only = e.post_only,
                                     .order_ref = e.user_ref};
}

protocol::OutboundMessage canceled(protocol::UserRefNum ref, Qty qty,
                                   protocol::WireCancelReason reason) {
    return protocol::CanceledMessage{.timestamp = 0, .user_ref = ref, .qty = qty, .reason = reason};
}

protocol::OutboundMessage executed(protocol::UserRefNum ref, Qty qty, protocol::Liquidity l) {
    return protocol::ExecutedMessage{.timestamp = 0,
                                     .user_ref = ref,
                                     .qty = qty,
                                     .price = 1'000'000,
                                     .liquidity = l,
                                     .match = 1};
}

protocol::OutboundMessage rejected(protocol::UserRefNum ref, protocol::WireRejectReason reason) {
    return protocol::RejectedMessage{.timestamp = 0, .user_ref = ref, .reason = reason};
}

// Enters and accepts orders until `n` are live. Only works while nothing else is sent.
std::vector<FlowRequest> make_live(OrderFlow& flow, std::size_t n) {
    std::vector<FlowRequest> entered;
    while (flow.live() < n) {
        const auto r = flow.next();
        EXPECT_TRUE(r.has_value());
        if (!r || r->kind != RequestKind::Enter) {
            break;
        }
        flow.on_reply(accept(*r));
        entered.push_back(*r);
    }
    return entered;
}

TEST(OrderFlowTest, RefusesConfigsThatCannotWork) {
    EXPECT_THROW(OrderFlow(FlowConfig{}, 0), std::invalid_argument);
    EXPECT_THROW(OrderFlow(config_with(50, 40, 11), 10), std::invalid_argument);
    FlowConfig c;
    c.min_qty = 0;
    EXPECT_THROW(OrderFlow(c, 10), std::invalid_argument);
    c = FlowConfig{};
    c.min_qty = 5;
    c.max_qty = 4;
    EXPECT_THROW(OrderFlow(c, 10), std::invalid_argument);
    c = FlowConfig{};
    c.band_ticks = 0;
    EXPECT_THROW(OrderFlow(c, 10), std::invalid_argument);
    c = FlowConfig{};
    c.middle = 1'000;  // Exactly band * tick, so the lowest buy would be at 0.
    EXPECT_THROW(OrderFlow(c, 10), std::invalid_argument);
    c = FlowConfig{};
    c.middle = 1'000'050;  // Not a tick multiple.
    EXPECT_THROW(OrderFlow(c, 10), std::invalid_argument);
    EXPECT_NO_THROW(OrderFlow(config_with(50, 40, 10), 10));
}

TEST(OrderFlowTest, SameSeedSameRequests) {
    FlowConfig c;
    c.seed = 42;
    OrderFlow a(c, 1'000);
    OrderFlow b(c, 1'000);
    c.seed = 43;
    OrderFlow other(c, 1'000);
    bool differs = false;
    for (int i = 0; i < 300; ++i) {
        const FlowRequest ra = testing::unwrap(a.next());
        const FlowRequest rb = testing::unwrap(b.next());
        const FlowRequest ro = testing::unwrap(other.next());
        ASSERT_EQ(ra.message, rb.message) << i;
        differs = differs || ra.message != ro.message;
        // Accept each enter so later requests include cancels and replaces.
        if (ra.kind == RequestKind::Enter) {
            a.on_reply(accept(ra));
            b.on_reply(accept(rb));
        }
        if (ro.kind == RequestKind::Enter) {
            other.on_reply(accept(ro));
        }
    }
    EXPECT_TRUE(differs);
}

TEST(OrderFlowTest, UserRefNumsStartAtFirstRefAndIncrease) {
    FlowConfig c;
    c.first_ref = 500;
    OrderFlow flow(c, 1'000);
    protocol::UserRefNum last = 0;
    bool first = true;
    for (int i = 0; i < 500; ++i) {
        const FlowRequest r = testing::unwrap(flow.next());
        if (r.kind == RequestKind::Cancel) {
            continue;  // A cancel names an existing order and uses no new number.
        }
        if (first) {
            EXPECT_EQ(r.ref, 500U);
            first = false;
        } else {
            EXPECT_GT(r.ref, last);
        }
        last = r.ref;
        if (r.kind == RequestKind::Enter) {
            flow.on_reply(accept(r));
        }
    }
    EXPECT_EQ(flow.next_ref(), last + 1);
}

TEST(OrderFlowTest, PassiveOrdersRestInsideTheBand) {
    FlowConfig c = config_with(0, 0, 0);
    c.min_qty = 3;
    c.max_qty = 7;
    c.live_target = 100'000;
    OrderFlow flow(c, 100'000);
    bool saw_buy = false;
    bool saw_sell = false;
    for (int i = 0; i < 2'000; ++i) {
        const FlowRequest r = testing::unwrap(flow.next());
        const protocol::EnterOrder& e = as_enter(r);
        EXPECT_EQ(e.tif, TimeInForce::GoodTillCancel);
        EXPECT_EQ(e.type, OrderType::Limit);
        EXPECT_EQ(e.symbol, c.symbol);
        EXPECT_GE(e.qty, 3U);
        EXPECT_LE(e.qty, 7U);
        EXPECT_EQ(e.price % c.tick, 0);
        const Price distance = e.side == Side::Buy ? c.middle - e.price : e.price - c.middle;
        EXPECT_GE(distance, c.tick);
        EXPECT_LE(distance, c.tick * c.band_ticks);
        saw_buy = saw_buy || e.side == Side::Buy;
        saw_sell = saw_sell || e.side == Side::Sell;
    }
    EXPECT_TRUE(saw_buy && saw_sell);
    EXPECT_EQ(flow.pending_enters(), 2'000U);
}

TEST(OrderFlowTest, AggressiveOrdersAreIocThroughTheWholeBand) {
    const FlowConfig c = config_with(0, 0, 100);
    OrderFlow flow(c, 1'000);
    for (int i = 0; i < 200; ++i) {
        const FlowRequest r = testing::unwrap(flow.next());
        const protocol::EnterOrder& e = as_enter(r);
        EXPECT_EQ(e.tif, TimeInForce::ImmediateOrCancel);
        const Price reach = c.tick * c.band_ticks;
        EXPECT_EQ(e.price, e.side == Side::Buy ? c.middle + reach : c.middle - reach);
        // An IOC that finds nothing is accepted and then canceled with reason I.
        flow.on_reply(accept(r));
        flow.on_reply(canceled(e.user_ref, e.qty, protocol::WireCancelReason::ImmediateOrCancel));
    }
    EXPECT_EQ(flow.live(), 0U);
    EXPECT_EQ(flow.pending_enters(), 0U);
    EXPECT_TRUE(flow.check_invariants());
}

TEST(OrderFlowTest, AtTheLiveTargetOnlyCancelsAndReplacesAreSent) {
    FlowConfig c = config_with(0, 0, 0);  // Below the target every request is an enter.
    c.live_target = 20;
    OrderFlow flow(c, 1'000);
    (void)make_live(flow, 20);
    ASSERT_EQ(flow.live(), 20U);
    // With both percentages 0 the flow cancels. Every order gets one request, then all are
    // busy and it enters again.
    for (int i = 0; i < 20; ++i) {
        const FlowRequest r = testing::unwrap(flow.next());
        EXPECT_EQ(r.kind, RequestKind::Cancel) << i;
    }
    EXPECT_EQ(flow.busy(), 20U);
    const FlowRequest r = testing::unwrap(flow.next());
    EXPECT_EQ(r.kind, RequestKind::Enter);
    EXPECT_TRUE(flow.check_invariants());
}

TEST(OrderFlowTest, ABusyOrderIsNeverNamedTwice) {
    const FlowConfig c = config_with(100, 0, 0);
    OrderFlow flow(c, 1'000);
    const auto entered = make_live(flow, 1);
    ASSERT_EQ(entered.size(), 1U);
    const FlowRequest first = testing::unwrap(flow.next());
    EXPECT_EQ(first.kind, RequestKind::Cancel);
    EXPECT_EQ(first.ref, entered[0].ref);
    const FlowRequest second = testing::unwrap(flow.next());
    EXPECT_EQ(second.kind, RequestKind::Enter);  // The only order is busy.
    // The cancel comes back, and the order is gone.
    flow.on_reply(canceled(entered[0].ref, as_enter(entered[0]).qty,
                           protocol::WireCancelReason::UserRequested));
    EXPECT_EQ(flow.live(), 0U);
    EXPECT_EQ(flow.busy(), 0U);
    EXPECT_TRUE(flow.check_invariants());
}

TEST(OrderFlowTest, FullOfBusyOrdersSendsNothing) {
    FlowConfig c = config_with(100, 0, 0);
    c.live_target = 1;
    OrderFlow flow(c, 1);
    (void)make_live(flow, 1);
    const FlowRequest cancel = testing::unwrap(flow.next());
    EXPECT_EQ(cancel.kind, RequestKind::Cancel);
    EXPECT_FALSE(flow.next().has_value());
    // An enter on its way also takes the only slot.
    OrderFlow other(c, 1);
    ASSERT_TRUE(other.next());
    EXPECT_FALSE(other.next().has_value());
}

TEST(OrderFlowTest, ReplaceMovesTheOrderToItsNewNumber) {
    FlowConfig c = config_with(0, 100, 0);
    c.live_target = 1;
    c.min_qty = 5;  // A fixed size, so the Quantity of the replace is known exactly.
    c.max_qty = 5;
    OrderFlow flow(c, 10);
    const auto entered = make_live(flow, 1);
    const protocol::UserRefNum old_ref = entered[0].ref;
    // Part of it trades first, so the replace's Quantity counts those shares too.
    flow.on_reply(executed(old_ref, 1, protocol::Liquidity::Added));
    const FlowRequest r = testing::unwrap(flow.next());
    ASSERT_EQ(r.kind, RequestKind::Replace);
    const auto& m = std::get<protocol::ReplaceOrder>(r.message);
    EXPECT_EQ(m.orig_user_ref, old_ref);
    EXPECT_EQ(m.user_ref, r.ref);
    EXPECT_EQ(m.qty, 1U + 5U);  // The share that executed plus the new open size.
    EXPECT_TRUE(flow.check_invariants());
    flow.on_reply(protocol::ReplacedMessage{.timestamp = 0,
                                            .orig_user_ref = old_ref,
                                            .user_ref = m.user_ref,
                                            .side = as_enter(entered[0]).side,
                                            .qty = m.qty - 1,
                                            .symbol = c.symbol,
                                            .price = m.price,
                                            .order_ref = 1,
                                            .kept_priority = false});
    EXPECT_EQ(flow.busy(), 0U);
    EXPECT_TRUE(flow.check_invariants());
    // From now on the order answers to its new number only.
    flow.on_reply(executed(old_ref, 1, protocol::Liquidity::Added));
    EXPECT_EQ(flow.unknown_replies(), 1U);
    flow.on_reply(executed(m.user_ref, m.qty - 1, protocol::Liquidity::Added));
    EXPECT_EQ(flow.live(), 0U);
    EXPECT_TRUE(flow.check_invariants());
}

TEST(OrderFlowTest, FailedReplaceEndsWithTheCancelOfTheOriginal) {
    FlowConfig c = config_with(0, 100, 0);
    c.live_target = 1;
    OrderFlow flow(c, 10);
    const auto entered = make_live(flow, 1);
    const FlowRequest r = testing::unwrap(flow.next());
    ASSERT_EQ(r.kind, RequestKind::Replace);
    flow.on_reply(rejected(r.ref, protocol::WireRejectReason::InvalidPrice));
    EXPECT_EQ(flow.live(), 1U);
    EXPECT_EQ(flow.busy(), 1U);  // Still busy until the cancel arrives.
    EXPECT_TRUE(flow.check_invariants());
    flow.on_reply(canceled(entered[0].ref, as_enter(entered[0]).qty,
                           protocol::WireCancelReason::UserRequested));
    EXPECT_EQ(flow.live(), 0U);
    EXPECT_EQ(flow.unknown_replies(), 0U);
    EXPECT_TRUE(flow.check_invariants());
}

TEST(OrderFlowTest, ExecutionsTrackThePosition) {
    const FlowConfig c = config_with(0, 0, 0);
    OrderFlow flow(c, 100);
    const auto entered = make_live(flow, 10);
    std::int64_t expected = 0;
    for (const FlowRequest& r : entered) {
        const protocol::EnterOrder& e = as_enter(r);
        const std::int64_t sign = e.side == Side::Buy ? 1 : -1;
        flow.on_reply(executed(e.user_ref, 1, protocol::Liquidity::Added));
        expected += sign;
        if (e.qty > 1) {
            flow.on_reply(executed(e.user_ref, e.qty - 1, protocol::Liquidity::Added));
            expected += sign * static_cast<std::int64_t>(e.qty - 1);
        }
    }
    EXPECT_EQ(flow.position(), expected);
    EXPECT_EQ(flow.live(), 0U);
    EXPECT_TRUE(flow.check_invariants());
}

TEST(OrderFlowTest, RejectedEnterFreesItsSlot) {
    // With room for one order, the enter on its way takes the only slot. Its Rejected
    // must give the slot back, or the flow could never send again.
    OrderFlow flow(config_with(0, 0, 0), 1);
    const FlowRequest r = testing::unwrap(flow.next());
    ASSERT_EQ(r.kind, RequestKind::Enter);
    EXPECT_FALSE(flow.next().has_value());
    flow.on_reply(rejected(r.ref, protocol::WireRejectReason::MaxOpenOrdersExceeded));
    EXPECT_EQ(flow.pending_enters(), 0U);
    EXPECT_EQ(flow.live(), 0U);
    EXPECT_EQ(flow.unknown_replies(), 0U);
    EXPECT_TRUE(flow.next().has_value());
    EXPECT_TRUE(flow.check_invariants());
}

TEST(OrderFlowTest, FenceIsRejectedWithoutTouchingTheOrders) {
    OrderFlow flow(FlowConfig{}, 10);
    const FlowRequest fence = flow.fence();
    EXPECT_EQ(fence.kind, RequestKind::Fence);
    EXPECT_EQ(as_enter(fence).symbol, FlowConfig{}.fence_symbol);
    EXPECT_EQ(flow.pending_enters(), 0U);
    flow.on_reply(rejected(fence.ref, protocol::WireRejectReason::InvalidSymbol));
    EXPECT_EQ(flow.unknown_replies(), 0U);
    EXPECT_EQ(flow.next_ref(), fence.ref + 1);
}

TEST(OrderFlowTest, ForEachLiveVisitsEveryLiveOrder) {
    OrderFlow flow(config_with(0, 0, 0), 100);
    const auto entered = make_live(flow, 5);
    flow.on_reply(executed(entered[1].ref, 1, protocol::Liquidity::Added));
    std::vector<protocol::UserRefNum> seen;
    flow.for_each_live([&](protocol::UserRefNum ref, Side side, Price price, Qty open) {
        seen.push_back(ref);
        for (const FlowRequest& r : entered) {
            if (r.ref == ref) {
                const protocol::EnterOrder& e = as_enter(r);
                EXPECT_EQ(side, e.side);
                EXPECT_EQ(price, e.price);
                EXPECT_EQ(open, ref == entered[1].ref ? e.qty - 1 : e.qty);
            }
        }
    });
    std::ranges::sort(seen);
    std::vector<protocol::UserRefNum> expected;
    for (const FlowRequest& r : entered) {
        if (r.ref != entered[1].ref || as_enter(r).qty > 1) {
            expected.push_back(r.ref);
        }
    }
    EXPECT_EQ(seen, expected);
}

TEST(OrderFlowTest, RepliesAboutUnknownOrdersAreCounted) {
    OrderFlow flow(FlowConfig{}, 10);
    flow.on_reply(executed(99, 1, protocol::Liquidity::Added));
    flow.on_reply(canceled(99, 1, protocol::WireCancelReason::System));
    flow.on_reply(rejected(99, protocol::WireRejectReason::Other));  // No enter is pending.
    EXPECT_EQ(flow.unknown_replies(), 3U);
    EXPECT_TRUE(flow.check_invariants());
}

// Two flows trade against the real order entry layer and its book. Requests reach the
// exchange and replies reach the clients after random delays, so cancels and replaces
// race with fills the way they do over a network. Every request must be answered or
// ignored, no enter may be ignored, and in the end each flow must agree with the exchange
// about its live orders.
class Exchange final : public protocol::OutboundSink {
public:
    explicit Exchange(std::array<std::deque<protocol::OutboundMessage>, 3>& inboxes)
        : inboxes_(&inboxes),
          entry_(*this, protocol::OrderEntryConfig{
                            .max_owners = 2, .max_live_orders = 1U << 14U, .max_symbols = 1}) {
        (void)entry_.add_symbol(FlowConfig{}.symbol,
                                BookConfig{.tick_size = 100, .max_orders = 1U << 14U});
    }

    void send(OwnerId owner, std::span<const std::byte> frame) override {
        (*inboxes_)[owner].push_back(testing::unwrap(protocol::decode_outbound(frame).message));
    }

    protocol::OrderEntry<SortedOrderBook>& entry() { return entry_; }

private:
    std::array<std::deque<protocol::OutboundMessage>, 3>* inboxes_;
    protocol::OrderEntry<SortedOrderBook> entry_;
};

struct SimClient {
    SimClient(const FlowConfig& c, std::size_t max_live) : flow(c, max_live), in_flight(65) {}
    OrderFlow flow;
    InFlightQueue in_flight;
    std::deque<protocol::InboundMessage> outbox;
    std::array<std::uint64_t, kRequestKinds> ignored{};
    std::uint64_t answered = 0;
    bool fence_answered = false;
};

TEST(OrderFlowTest, AgreesWithTheRealExchangeUnderDelays) {
    std::array<std::deque<protocol::OutboundMessage>, 3> inboxes;
    Exchange exchange(inboxes);
    std::vector<SimClient> clients;
    clients.reserve(2);
    for (const std::uint64_t seed : {7ULL, 8ULL}) {
        FlowConfig c;
        c.seed = seed;
        c.live_target = 40;
        c.aggressive_percent = 15;
        c.band_ticks = 4;  // A narrow band, so aggressive orders often reach resting ones.
        clients.emplace_back(c, 200);
    }
    std::mt19937_64 rng(1);
    protocol::Timestamp now = 1;

    const auto deliver = [&](std::size_t i, std::size_t n) {
        SimClient& client = clients[i];
        auto& inbox = inboxes[i + 1];
        for (std::size_t k = 0; k < n && !inbox.empty(); ++k) {
            const protocol::OutboundMessage m = inbox.front();
            inbox.pop_front();
            if (const auto key = reply_key(m)) {
                const Answer a = client.in_flight.answer(key->kind, key->ref);
                for (std::size_t j = 0; j < kRequestKinds; ++j) {
                    client.ignored[j] += a.ignored[j];
                }
                if (a.found) {
                    ++client.answered;
                    client.fence_answered =
                        client.fence_answered || a.request.kind == RequestKind::Fence;
                }
            }
            client.flow.on_reply(m);
            ASSERT_TRUE(client.flow.check_invariants());
        }
    };
    const auto handle = [&](std::size_t i, std::size_t n) {
        SimClient& client = clients[i];
        for (std::size_t k = 0; k < n && !client.outbox.empty(); ++k) {
            exchange.entry().handle(static_cast<OwnerId>(i + 1), client.outbox.front(), now++);
            client.outbox.pop_front();
        }
    };

    for (int step = 0; step < 20'000; ++step) {
        for (std::size_t i = 0; i < clients.size(); ++i) {
            SimClient& client = clients[i];
            if (client.in_flight.size() < 64) {
                if (const auto r = client.flow.next()) {
                    ASSERT_TRUE(client.in_flight.push(InFlight{.kind = r->kind, .ref = r->ref}));
                    client.outbox.push_back(r->message);
                }
            }
            handle(i, rng() % 3);
            deliver(i, rng() % 4);
        }
    }
    // Every client sends a fence, then everything is handled. Only then is everything
    // delivered, because one client's last orders can still trade with the other's.
    for (std::size_t i = 0; i < clients.size(); ++i) {
        const FlowRequest fence = clients[i].flow.fence();
        ASSERT_TRUE(clients[i].in_flight.push(InFlight{.kind = fence.kind, .ref = fence.ref}));
        clients[i].outbox.push_back(fence.message);
        handle(i, ~std::size_t{0});
    }
    for (std::size_t i = 0; i < clients.size(); ++i) {
        deliver(i, ~std::size_t{0});
    }

    std::int64_t net = 0;
    std::uint64_t ignored_cancels = 0;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        const SimClient& client = clients[i];
        const auto owner = static_cast<OwnerId>(i + 1);
        EXPECT_TRUE(client.fence_answered);
        EXPECT_TRUE(client.in_flight.empty());
        EXPECT_EQ(client.ignored[static_cast<std::size_t>(RequestKind::Enter)], 0U);
        EXPECT_EQ(client.ignored[static_cast<std::size_t>(RequestKind::Fence)], 0U);
        EXPECT_EQ(client.flow.unknown_replies(), 0U);
        EXPECT_EQ(client.flow.pending_enters(), 0U);
        EXPECT_EQ(client.flow.busy(), 0U);
        EXPECT_EQ(client.flow.live(), exchange.entry().open_orders(owner));
        EXPECT_GT(client.answered, 10'000U);
        net += client.flow.position();
        ignored_cancels += client.ignored[static_cast<std::size_t>(RequestKind::Cancel)] +
                           client.ignored[static_cast<std::size_t>(RequestKind::Replace)];
    }
    EXPECT_EQ(net, 0);  // Every share one flow bought, the other sold.
    // The delays must really have produced cancels of orders that had filled, or this
    // test would not cover the race it is about.
    EXPECT_GT(ignored_cancels, 0U);
}

}  // namespace
}  // namespace litbook::loadgen
