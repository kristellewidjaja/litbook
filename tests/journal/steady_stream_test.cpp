#include "journal/steady_stream.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "support/unwrap.hpp"

namespace litbook::testing {
namespace {

// Counts the replies by their message type, which is the first byte of each frame.
class TypeCountingSink final : public protocol::OutboundSink {
public:
    void send(OwnerId /*owner*/, std::span<const std::byte> frame) override {
        ++replies;
        if (frame.empty()) {
            return;
        }
        const auto type = static_cast<char>(frame.front());
        if (type == protocol::kRejectedType) {
            ++rejected;
        } else if (type == protocol::kExecutedType) {
            ++executed;
        }
    }
    std::uint64_t replies = 0;
    std::uint64_t rejected = 0;
    std::uint64_t executed = 0;
};

// The exchange the stream is written for, with books much bigger than any target here,
// so a book is never full.
struct SteadyExchange {
    TypeCountingSink sink;
    protocol::OrderEntry<SortedOrderBook> orders{
        sink, protocol::OrderEntryConfig{.max_owners = kSteadyOwners, .max_symbols = 2}};

    SteadyExchange() {
        for (const char* name : kStreamStocks) {
            orders.add_symbol(unwrap(make_ticker(name)), steady_book_config(1U << 16U));
        }
    }
};

TEST(SteadyStreamTest, TheSameSeedGivesTheSameStream) {
    const auto a = steady_stream(3, 2000, 100);
    const auto b = steady_stream(3, 2000, 100);
    ASSERT_EQ(a.size(), 2000U);
    ASSERT_EQ(b.size(), 2000U);
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].owner, b[i].owner);
        EXPECT_EQ(a[i].bytes, b[i].bytes);
    }
}

TEST(SteadyStreamTest, AnotherSeedGivesAnotherStream) {
    const auto a = steady_stream(3, 200, 100);
    const auto b = steady_stream(4, 200, 100);
    std::size_t same = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].owner == b[i].owner && a[i].bytes == b[i].bytes) {
            ++same;
        }
    }
    EXPECT_LT(same, 20U);
}

TEST(SteadyStreamTest, ZeroFramesGivesAnEmptyStream) {
    EXPECT_TRUE(steady_stream(1, 0, 100).empty());
}

TEST(SteadyStreamTest, EveryFrameDecodesAndComesFromARealAccount) {
    for (const InboundFrame& frame : steady_stream(5, 5000, 300)) {
        EXPECT_GE(frame.owner, 1U);
        EXPECT_LE(frame.owner, kSteadyOwners);
        EXPECT_EQ(protocol::decode_inbound(frame.bytes).status, protocol::DecodeStatus::Ok);
    }
}

// After the stream has grown the exchange, the live orders stay between half the target
// and the target, the stream keeps trading, and only a small share of requests name an
// order that already filled or are rejected.
TEST(SteadyStreamTest, HoldsTheLiveOrdersNearTheTarget) {
    constexpr std::size_t kTarget = 2000;
    constexpr std::size_t kFrames = 60'000;
    const auto stream = steady_stream(7, kFrames, kTarget);
    auto exchange = std::make_unique<SteadyExchange>();
    protocol::Timestamp now = 0;
    std::uint32_t lowest_after_growth = kTarget;
    std::uint32_t highest = 0;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        EXPECT_EQ(exchange->orders.on_frame(stream[i].owner, stream[i].bytes, ++now),
                  protocol::DecodeStatus::Ok);
        const std::uint32_t live = exchange->orders.live_orders();
        highest = std::max(highest, live);
        if (i >= 20'000) {
            lowest_after_growth = std::min(lowest_after_growth, live);
        }
    }
    EXPECT_LE(highest, kTarget);
    EXPECT_GE(lowest_after_growth, kTarget / 2);

    const protocol::OrderEntryStats& stats = exchange->orders.stats();
    EXPECT_EQ(stats.malformed, 0U);
    EXPECT_EQ(stats.bad_owner, 0U);
    EXPECT_EQ(stats.stale_user_ref, 0U);
    EXPECT_LT(stats.unknown_order, kFrames / 4);
    EXPECT_LT(exchange->sink.rejected, kFrames / 1000);
    EXPECT_GT(exchange->sink.executed, kFrames / 100);
}

TEST(SteadyStreamTest, NeverHoldsMoreThanASmallTarget) {
    for (const std::size_t target : {std::size_t{1}, std::size_t{10}}) {
        const auto stream = steady_stream(9, 3000, target);
        auto exchange = std::make_unique<SteadyExchange>();
        protocol::Timestamp now = 0;
        for (const InboundFrame& frame : stream) {
            static_cast<void>(exchange->orders.on_frame(frame.owner, frame.bytes, ++now));
            EXPECT_LE(exchange->orders.live_orders(), target) << "target " << target;
        }
    }
}

}  // namespace
}  // namespace litbook::testing
