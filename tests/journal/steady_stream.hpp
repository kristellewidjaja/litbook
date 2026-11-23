#pragma once

// A request stream that keeps the exchange at a chosen size, for the journal benchmarks.
// No GoogleTest dependency, so the benchmarks and tools can use it too.

#include "litbook/book/book_config.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "protocol/order_entry_stream.hpp"
#include "support/unwrap.hpp"

namespace litbook::testing {

// The exchange a steady stream is written for. Four accounts (owners 1 to 4) trade the two
// stocks of kStreamStocks with a $0.01 tick around $50.00, and each book holds up to
// max_orders orders.
inline constexpr OwnerId kSteadyOwners = 4;
inline constexpr Price kSteadyTick = 100;
inline constexpr Price kSteadyMiddle = 5'000 * kSteadyTick;
inline constexpr Price kSteadyMaxDistanceTicks = 100;

inline BookConfig steady_book_config(std::uint32_t max_orders) {
    BookConfig config{.tick_size = kSteadyTick};
    config.min_price = kSteadyMiddle - (2 * kSteadyMaxDistanceTicks * kSteadyTick);
    config.max_price = kSteadyMiddle + (2 * kSteadyMaxDistanceTicks * kSteadyTick);
    config.max_orders = max_orders;
    return config;
}

// A reproducible stream of encoded frames that grows the exchange to about `target_live`
// live orders and then holds it there. The generator keeps a list of each account's
// orders that may still be live. While the lists hold fewer than target_live orders, a
// frame is a new order (55%), a replace of a random listed order (25%) or a cancel of one
// (20%). Once they hold target_live, every frame cancels a random listed order, so the
// size swings back. One new order in 25 crosses the spread and trades, and prices sit 1
// to 100 ticks away from $50.00, so books are about 100 levels deep.
//
// A filled order stays on its list until a cancel or replace picks it, and that request
// is then rejected as for an unknown order, as a real client's late cancel would be. A
// replace of a filled order is rejected the same way, but its new reference stays listed,
// so dead entries carry on. Measured over 60,000 frames with a target of 2,000, about one
// request in five named an unknown order and about two thirds of the target was live.
// The exchange never holds more live orders than the lists say. A replace whose new size
// is not above what the order already executed is rejected too. Every frame decodes.
//
// Unlike order_entry_stream, whose cancels only reach each account's last few orders
// (so its books fill up and later orders are refused), this stream has a bounded and
// chosen state size, which is what snapshot and recovery measurements need. It makes one
// frame at a time, so a tool can write millions of them without holding them all.
//
// Invariants. listed_total_ is the sum of the list sizes, every listed reference is at
// most its account's last_ref_, and last_ref_ only grows, so every UserRefNum it sends is
// new for its account.
class SteadyStream {
public:
    SteadyStream(std::uint64_t seed, std::size_t target_live)
        : rng_(seed), target_live_(target_live) {}

    // The next frame of the stream.
    InboundFrame next() {
        auto owner = static_cast<OwnerId>(1 + pick(kSteadyOwners));
        // At the target the frame must cancel, so it moves on to an account that has
        // something listed.
        while (listed_total_ >= target_live_ && listed_total_ > 0 && listed_.at(owner).empty()) {
            owner = static_cast<OwnerId>(1 + (owner % kSteadyOwners));
        }
        std::vector<protocol::UserRefNum>& mine = listed_.at(owner);

        const Side side = pick(2) == 0 ? Side::Buy : Side::Sell;
        const bool cross = pick(25) == 0;
        const auto distance = static_cast<Price>(1 + pick(kSteadyMaxDistanceTicks));
        const bool below = (side == Side::Buy) != cross;
        const Price price = below ? kSteadyMiddle - (distance * kSteadyTick)
                                  : kSteadyMiddle + (distance * kSteadyTick);
        const Qty qty = 10 * (1 + static_cast<Qty>(pick(8)));

        // Decide what this frame does. An account with nothing listed can only add.
        const std::size_t roll = pick(100);
        enum class Kind : std::uint8_t { Enter, Replace, Cancel };
        Kind kind = Kind::Enter;
        if (!mine.empty()) {
            if (listed_total_ >= target_live_ || roll >= 80) {
                kind = Kind::Cancel;
            } else if (roll >= 55) {
                kind = Kind::Replace;
            }
        }

        protocol::InboundMessage message = protocol::CancelOrder{};
        protocol::UserRefNum& last = last_ref_.at(owner);
        if (kind == Kind::Enter) {
            message = protocol::EnterOrder{.user_ref = ++last,
                                           .side = side,
                                           .qty = qty,
                                           .symbol = stocks_.at(pick(stocks_.size())),
                                           .price = price,
                                           .tif = TimeInForce::GoodTillCancel,
                                           .type = OrderType::Limit,
                                           .post_only = false};
            mine.push_back(last);
            ++listed_total_;
        } else {
            const std::size_t slot = pick(mine.size());
            const protocol::UserRefNum target = mine[slot];
            if (kind == Kind::Replace) {
                // The replacement takes the old order's place on the list.
                mine[slot] = ++last;
                message = protocol::ReplaceOrder{
                    .orig_user_ref = target, .user_ref = mine[slot], .qty = qty, .price = price};
            } else {
                mine[slot] = mine.back();
                mine.pop_back();
                --listed_total_;
                message = protocol::CancelOrder{.user_ref = target, .qty = 0};
            }
        }

        std::vector<std::byte> bytes(protocol::kMaxMessageLength);
        bytes.resize(protocol::encode(message, bytes));
        return InboundFrame{.owner = owner, .bytes = std::move(bytes)};
    }

private:
    std::size_t pick(std::size_t n) { return static_cast<std::size_t>(rng_() % n); }

    std::mt19937_64 rng_;
    std::size_t target_live_;
    std::array<protocol::UserRefNum, kSteadyOwners + 1> last_ref_{};  // Slot 0 unused.
    std::array<std::vector<protocol::UserRefNum>, kSteadyOwners + 1> listed_{};
    std::size_t listed_total_ = 0;
    std::array<Ticker, 2> stocks_{unwrap(make_ticker(kStreamStocks[0])),
                                  unwrap(make_ticker(kStreamStocks[1]))};
};

// The first `frames` frames of SteadyStream(seed, target_live), all at once.
inline std::vector<InboundFrame> steady_stream(std::uint64_t seed, std::size_t frames,
                                               std::size_t target_live) {
    SteadyStream stream(seed, target_live);
    std::vector<InboundFrame> out;
    out.reserve(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out.push_back(stream.next());
    }
    return out;
}

}  // namespace litbook::testing
