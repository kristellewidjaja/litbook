#pragma once

#include "litbook/engine/ticker.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include "support/unwrap.hpp"

namespace litbook::testing {

// One inbound frame and the account that sends it.
struct InboundFrame {
    OwnerId owner;
    std::vector<std::byte> bytes;
};

// The two stocks every stream trades. Both use a $0.01 tick (100 in 4-decimal prices) and
// lot size 1.
inline constexpr std::array<const char*, 2> kStreamStocks{"AAPL", "MSFT"};

// Builds a random but reproducible stream of encoded inbound frames from four accounts
// (owners 1 to 4) over the two stocks above. Half the frames are new orders, three in
// ten are replaces and two in ten are full cancels. Buys sit below $50.00 and sells
// above it, with one order in ten crossing, so the books stay a realistic depth and
// trades happen. One frame in a hundred has its last byte cut off, so it is malformed.
// The no-allocation test and the order entry benchmark both use it.
inline std::vector<InboundFrame> order_entry_stream(std::uint64_t seed, int frames) {
    std::mt19937_64 rng(seed);
    const auto pick = [&](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    std::array<protocol::UserRefNum, 5> last_ref{};  // Indexed by owner. Slot 0 is unused.

    std::vector<InboundFrame> out;
    out.reserve(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const auto owner = static_cast<OwnerId>(1 + pick(4));
        protocol::UserRefNum& last = last_ref.at(owner);
        // Cancels and replaces name one of the account's last few orders.
        const protocol::UserRefNum recent = last - pick(std::min<protocol::UserRefNum>(last, 8) + 1);

        const Side side = pick(2) == 0 ? Side::Buy : Side::Sell;
        const bool cross = pick(10) == 0;
        const auto distance = static_cast<Price>(pick(20)) + 1;
        const bool below = (side == Side::Buy) != cross;
        const Price price = (below ? 5'000 - distance : 5'000 + distance) * 100;
        const Qty qty = 10 * (1 + pick(8));

        protocol::InboundMessage message = protocol::CancelOrder{.user_ref = recent, .qty = 0};
        const std::uint32_t kind = pick(10);
        if (kind < 5) {
            message = protocol::EnterOrder{.user_ref = ++last,
                                           .side = side,
                                           .qty = qty,
                                           .symbol = unwrap(make_ticker(kStreamStocks.at(pick(2)))),
                                           .price = price,
                                           .tif = TimeInForce::GoodTillCancel,
                                           .type = OrderType::Limit,
                                           .post_only = false};
        } else if (kind < 8) {
            message = protocol::ReplaceOrder{
                .orig_user_ref = recent, .user_ref = ++last, .qty = qty, .price = price};
        }

        std::vector<std::byte> bytes(protocol::kMaxMessageLength);
        bytes.resize(protocol::encode(message, bytes));
        if (pick(100) == 0) {
            bytes.pop_back();
        }
        out.push_back(InboundFrame{.owner = owner, .bytes = std::move(bytes)});
    }
    return out;
}

}  // namespace litbook::testing
