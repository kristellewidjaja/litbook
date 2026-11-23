#pragma once

// The requests one load generator session sends, and what it knows about its own orders.
//
// The flow trades one stock around a fixed middle price. Passive orders are limit orders
// that rest between 1 and band_ticks ticks away from the middle, buys below it and sells
// above it, so passive orders never cross each other. Aggressive orders are IOC limit
// orders priced band_ticks through the middle, so they trade with whatever rests on the
// other side within the band and the rest is canceled. Cancels and replaces always name
// one of the session's own live orders.
//
// The flow only learns what happened from the exchange's replies (on_reply), the same way
// a real client does. An order is live from its Order Accepted until executions and cancels
// have taken all of its open shares. While a Cancel or Replace for an order is on its way
// the order is busy, and the flow never sends a second request for it. That keeps the
// replies unambiguous (see InFlightQueue). A Cancel or Replace can still arrive after the
// order filled, and the exchange then ignores it, which is normal.
//
// The live orders sit in a dense array, so picking one at random is one index, with a
// FlatIdMap from UserRefNum to position. Both are sized in the constructor and nothing
// allocates after it.
//
// These invariants always hold. live() + pending_enters() <= max_live(). Every live order
// has open shares. Each live order is in the map under its UserRefNum, and a busy order
// with a Replace on its way also under the Replace's new UserRefNum. busy() counts the
// live orders that are busy. Every UserRefNum handed out is higher than the one before.

#include "litbook/container/flat_id_map.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/loadgen/in_flight_queue.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace litbook::loadgen {

// make_ticker for a name that is known to be valid. Throws std::invalid_argument if not.
[[nodiscard]] constexpr Ticker known_ticker(std::string_view name) {
    const std::optional<Ticker> ticker = make_ticker(name);
    if (!ticker) {
        throw std::invalid_argument("not a valid ticker");
    }
    return *ticker;
}

struct FlowConfig {
    Ticker symbol = known_ticker("AAPL");
    // A stock the exchange does not list, used by fence requests.
    Ticker fence_symbol = known_ticker("ZZZZZZZZ");
    Price middle = 1'000'000;  // $100.0000 with 4 implied decimals.
    Price tick = 100;          // $0.01, the tick litbook_gateway uses.
    std::uint32_t band_ticks = 10;
    Qty min_qty = 1;
    Qty max_qty = 100;
    // Once this many orders are live, every request is a cancel or a replace.
    std::uint32_t live_target = 100;
    // The chance of each request type, in percent, while fewer than live_target orders are
    // live. Whatever the three leave is passive orders.
    std::uint32_t cancel_percent = 30;
    std::uint32_t replace_percent = 10;
    std::uint32_t aggressive_percent = 10;
    std::uint64_t seed = 1;
    // The first UserRefNum. The exchange ignores numbers that are not higher than the last
    // one the account used, so a second run against the same exchange needs a higher one.
    protocol::UserRefNum first_ref = 1;
};

// One request, ready to encode, and the facts the in-flight queue needs about it.
struct FlowRequest {
    protocol::InboundMessage message;
    RequestKind kind;
    protocol::UserRefNum ref;  // The UserRefNum the first reply carries.
};

class OrderFlow {
public:
    // max_live bounds live orders plus enters on their way. Throws std::invalid_argument
    // for a config that cannot work (a percentage sum over 100, a quantity range that is
    // empty, a zero tick or band, a middle price too close to 0, or max_live of 0).
    OrderFlow(const FlowConfig& config, std::size_t max_live);

    // The next request. Returns nothing if no request can be sent right now, which only
    // happens when every slot is taken by enters on their way and busy orders.
    [[nodiscard]] std::optional<FlowRequest> next() noexcept;

    // A request that is always rejected, for the fence (see RequestKind::Fence).
    [[nodiscard]] FlowRequest fence() noexcept;

    // Updates the live orders from one reply of the exchange.
    void on_reply(const protocol::OutboundMessage& reply) noexcept;

    [[nodiscard]] std::size_t live() const noexcept { return orders_.size(); }
    [[nodiscard]] std::size_t busy() const noexcept { return busy_; }
    [[nodiscard]] std::size_t pending_enters() const noexcept { return pending_enters_; }
    [[nodiscard]] std::size_t max_live() const noexcept { return max_live_; }
    // The UserRefNum the next request will use.
    [[nodiscard]] protocol::UserRefNum next_ref() const noexcept { return next_ref_; }
    // Replies about an order the flow does not know, which should never happen.
    [[nodiscard]] std::uint64_t unknown_replies() const noexcept { return unknown_replies_; }

    // The shares the session holds after its executions, buys minus sells.
    [[nodiscard]] std::int64_t position() const noexcept { return position_; }

    // Calls visit(ref, side, price, open) for every live order, in no particular order.
    template <class Visit>
    void for_each_live(Visit&& visit) const {
        for (const LiveOrder& o : orders_) {
            visit(o.ref, o.side, o.price, o.open);
        }
    }

    // Checks every invariant above. For tests.
    [[nodiscard]] bool check_invariants() const;

private:
    struct LiveOrder {
        protocol::UserRefNum ref = 0;
        protocol::UserRefNum replace_ref = 0;  // The new UserRefNum of a Replace on its way.
        Side side = Side::Buy;
        Price price = 0;
        Qty open = 0;
        Qty executed = 0;
        bool busy = false;
    };

    [[nodiscard]] std::uint64_t random() noexcept;
    [[nodiscard]] std::uint64_t random_below(std::uint64_t n) noexcept;
    [[nodiscard]] Qty random_qty() noexcept;
    [[nodiscard]] Price passive_price(Side side) noexcept;
    [[nodiscard]] Side random_side() noexcept;

    [[nodiscard]] FlowRequest enter(bool aggressive) noexcept;
    // An idle live order picked at random, or nothing if every live order is busy.
    [[nodiscard]] std::optional<std::size_t> pick_idle() noexcept;
    [[nodiscard]] FlowRequest cancel(std::size_t index) noexcept;
    [[nodiscard]] FlowRequest replace(std::size_t index) noexcept;

    void accepted(const protocol::AcceptedMessage& m) noexcept;
    void replaced(const protocol::ReplacedMessage& m) noexcept;
    void canceled(const protocol::CanceledMessage& m) noexcept;
    void executed(const protocol::ExecutedMessage& m) noexcept;
    void rejected(const protocol::RejectedMessage& m) noexcept;

    // Takes `shares` off the order at `index` and removes it once nothing is open.
    void take_off(std::size_t index, Qty shares) noexcept;
    void remove(std::size_t index) noexcept;
    [[nodiscard]] std::optional<std::size_t> find(protocol::UserRefNum ref) const noexcept;

    FlowConfig config_;
    std::size_t max_live_;
    std::uint64_t state_;
    protocol::UserRefNum next_ref_;
    std::vector<LiveOrder> orders_;
    FlatIdMap index_;
    std::size_t busy_ = 0;
    std::size_t pending_enters_ = 0;
    std::uint64_t unknown_replies_ = 0;
    std::int64_t position_ = 0;
    // The UserRefNum of the last fence, or 0. Its Rejected is not an order's reply.
    protocol::UserRefNum fence_ref_ = 0;
};

}  // namespace litbook::loadgen
