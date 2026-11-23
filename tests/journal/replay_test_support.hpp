#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/protocol/order_entry.hpp"
#include "litbook/types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "book/test_limits.hpp"
#include "support/unwrap.hpp"

// A random request stream, recording sinks and a state comparison, shared by the replay
// tests and the no-allocation test.
namespace litbook::journal::testing {

inline constexpr Price kStreamTick = 100;  // $0.01 in 4-decimal prices.
inline constexpr std::array<const char*, 3> kReplayStocks{"AAPL", "MSFT", "ZVZZT"};
inline constexpr OwnerId kReplayOwners = 4;

inline BookConfig replay_book_config() {
    return litbook::testing::with_test_limits(BookConfig{.tick_size = kStreamTick, .lot_size = 1});
}

inline protocol::OrderEntryConfig replay_order_entry_config() {
    protocol::OrderEntryConfig config;
    config.max_owners = kReplayOwners;
    config.max_live_orders = 4096;
    config.max_symbols = kReplayStocks.size();
    return config;
}

// Account 3 has tight risk limits, so some of its orders are refused before the book.
inline constexpr OwnerId kLimitedOwner = 3;
inline constexpr protocol::RiskLimits kLimitedRisk{.max_order_qty = 50, .max_open_orders = 6};

// Builds a reproducible stream of requests, as the gateway thread would hand them to the
// matching thread. It covers every path the order entry layer has, so a replay that
// differs anywhere shows up.
//   new orders (GTC limits, IOC, FOK, post-only, market), replaces, full and partial
//   cancels, cancel on disconnect (CancelAll), malformed frames, frames from owners that
//   do not exist, and timestamps that grow by a random step.
// Buys sit below $50.00 and sells above, one order in eight crosses, so trades happen.
inline std::vector<pipeline::Request> replay_stream(std::uint64_t seed, int count,
                                                    pipeline::Timestamp start = 1'000) {
    std::mt19937_64 rng(seed);
    const auto pick = [&](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    std::array<protocol::UserRefNum, kReplayOwners + 1> last_ref{};
    pipeline::Timestamp now = start;

    std::vector<pipeline::Request> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        now += 1 + pick(5'000);
        const auto owner = static_cast<OwnerId>(1 + pick(kReplayOwners));
        const std::uint32_t kind = pick(100);
        if (kind < 2) {
            out.push_back(pipeline::make_cancel_all_request(owner, now));
            continue;
        }
        protocol::UserRefNum& last = last_ref.at(owner);
        const protocol::UserRefNum recent =
            last - pick(std::min<protocol::UserRefNum>(last, 10) + 1);
        const Side side = pick(2) == 0 ? Side::Buy : Side::Sell;
        const bool cross = pick(8) == 0;
        const auto distance = static_cast<Price>(pick(25)) + 1;
        const bool below = (side == Side::Buy) != cross;
        const Price price = (below ? 5'000 - distance : 5'000 + distance) * kStreamTick;
        const Qty qty = 10 * (1 + pick(10));
        const Ticker stock = litbook::testing::unwrap(make_ticker(kReplayStocks.at(pick(3))));

        protocol::InboundMessage message = protocol::CancelOrder{.user_ref = recent, .qty = 0};
        if (kind < 45) {
            protocol::EnterOrder enter{.user_ref = ++last,
                                       .side = side,
                                       .qty = qty,
                                       .symbol = stock,
                                       .price = price,
                                       .tif = TimeInForce::GoodTillCancel,
                                       .type = OrderType::Limit,
                                       .post_only = false};
            if (kind < 6) {
                enter.tif = TimeInForce::ImmediateOrCancel;
            } else if (kind < 8) {
                enter.tif = TimeInForce::FillOrKill;
            } else if (kind < 10) {
                enter.post_only = true;
            } else if (kind < 12) {
                enter.type = OrderType::Market;
                enter.tif = TimeInForce::ImmediateOrCancel;
                enter.price = 0;
            }
            message = enter;
        } else if (kind < 70) {
            message = protocol::ReplaceOrder{
                .orig_user_ref = recent, .user_ref = ++last, .qty = qty, .price = price};
        } else if (kind < 80) {
            // Cancel down to a smaller size instead of all of it.
            message = protocol::CancelOrder{.user_ref = recent, .qty = 10 * (1 + pick(3))};
        }
        std::array<std::byte, protocol::kMaxMessageLength> bytes{};
        std::size_t n = protocol::encode(message, bytes);
        if (kind >= 98) {
            --n;  // One byte short, so the frame does not decode.
        }
        OwnerId sender = owner;
        if (kind == 97) {
            sender = pick(2) == 0 ? kNoOwner : kReplayOwners + 1;  // No such account.
        }
        out.push_back(
            pipeline::make_frame_request(sender, std::span<const std::byte>(bytes.data(), n), now));
    }
    return out;
}

// One reply as the order entry layer sent it.
struct RecordedReply {
    OwnerId owner;
    std::vector<std::byte> bytes;
    bool operator==(const RecordedReply&) const = default;
};

class RecordingReplies final : public protocol::OutboundSink {
public:
    void send(OwnerId owner, std::span<const std::byte> frame) override {
        replies.push_back(RecordedReply{.owner = owner, .bytes = {frame.begin(), frame.end()}});
    }
    std::vector<RecordedReply> replies;
};

// One trade or level update, with the time of the request that caused it, which is what
// the matching thread puts in its MarketEvent.
struct RecordedEvent {
    bool trade;
    pipeline::Timestamp now;
    SymbolId symbol;
    Side side;
    Price price;
    AggregateQty qty;
    std::uint32_t order_count;
    MatchId match_id;
    bool operator==(const RecordedEvent&) const = default;
};

class RecordingEvents final : public protocol::MarketDataSink {
public:
    // `now` points at whatever the test or the replayer says the current time is.
    explicit RecordingEvents(const pipeline::Timestamp* now) : now_(now) {}
    void on_trade(SymbolId symbol, const Trade& t) override {
        events.push_back(RecordedEvent{.trade = true,
                                       .now = *now_,
                                       .symbol = symbol,
                                       .side = t.aggressor_side,
                                       .price = t.price,
                                       .qty = t.qty,
                                       .order_count = 0,
                                       .match_id = t.match_id});
    }
    void on_level_update(SymbolId symbol, const LevelUpdate& u) override {
        events.push_back(RecordedEvent{.trade = false,
                                       .now = *now_,
                                       .symbol = symbol,
                                       .side = u.side,
                                       .price = u.price,
                                       .qty = u.total_qty,
                                       .order_count = u.order_count,
                                       .match_id = 0});
    }
    std::vector<RecordedEvent> events;

private:
    const pipeline::Timestamp* now_;
};

// Feeds one request to an order entry layer exactly as MatchingWorker::handle does.
template <OrderBook Book>
void apply_request(protocol::OrderEntry<Book>& orders, const pipeline::Request& r) {
    switch (r.kind) {
        case pipeline::RequestKind::Frame:
            static_cast<void>(orders.on_frame(r.owner, r.bytes(), r.now));
            return;
        case pipeline::RequestKind::CancelAll:
            orders.cancel_all(r.owner, r.now);
            return;
        case pipeline::RequestKind::Stop:
            return;
    }
}

// Lists the stocks and the risk limits the way the replay tests expect.
template <OrderBook Book>
void set_up_exchange(protocol::OrderEntry<Book>& orders) {
    for (const char* name : kReplayStocks) {
        orders.add_symbol(litbook::testing::unwrap(make_ticker(name)), replay_book_config());
    }
    orders.set_risk_limits(kLimitedOwner, kLimitedRisk);
}

// The setup records of the exchange that set_up_exchange builds, as a journal holds them.
inline std::vector<JournalRecord> replay_setup_records() {
    std::vector<JournalRecord> setup;
    setup.push_back(session_start_record(replay_order_entry_config()));
    SymbolId id = 0;
    for (const char* name : kReplayStocks) {
        setup.push_back(
            add_symbol_record(SymbolListing{.id = id,
                                            .ticker = litbook::testing::unwrap(make_ticker(name)),
                                            .config = replay_book_config()}));
        ++id;
    }
    setup.push_back(risk_limits_record(kLimitedOwner, kLimitedRisk));
    return setup;
}

// Everything that can be seen about the state of two order entry layers must agree, each
// book level by level.
template <OrderBook A, OrderBook B>
void expect_same_state(const protocol::OrderEntry<A>& a, const protocol::OrderEntry<B>& b) {
    EXPECT_EQ(a.live_orders(), b.live_orders());
    EXPECT_EQ(a.stats(), b.stats());
    for (OwnerId owner = 1; owner <= a.config().max_owners; ++owner) {
        EXPECT_EQ(a.last_user_ref(owner), b.last_user_ref(owner)) << "owner " << owner;
        EXPECT_EQ(a.open_orders(owner), b.open_orders(owner)) << "owner " << owner;
        EXPECT_EQ(a.risk_limits(owner), b.risk_limits(owner)) << "owner " << owner;
    }
    const auto& ea = a.engine();
    const auto& eb = b.engine();
    ASSERT_EQ(ea.symbol_count(), eb.symbol_count());
    EXPECT_EQ(ea.last_match_id(), eb.last_match_id());
    EXPECT_TRUE(ea.check_invariants());
    EXPECT_TRUE(eb.check_invariants());
    for (std::size_t s = 0; s < ea.symbol_count(); ++s) {
        const auto id = static_cast<SymbolId>(s);
        const A& book_a = *ea.book(id);
        const B& book_b = *eb.book(id);
        EXPECT_EQ(ea.ticker(id), eb.ticker(id));
        EXPECT_EQ(book_a.order_count(), book_b.order_count()) << "stock " << s;
        EXPECT_EQ(book_a.best_bid(), book_b.best_bid()) << "stock " << s;
        EXPECT_EQ(book_a.best_ask(), book_b.best_ask()) << "stock " << s;
        for (const Side side : {Side::Buy, Side::Sell}) {
            ASSERT_EQ(book_a.level_count(side), book_b.level_count(side)) << "stock " << s;
            EXPECT_EQ(book_a.depth(side, book_a.level_count(side)),
                      book_b.depth(side, book_b.level_count(side)))
                << "stock " << s;
        }
    }
}

}  // namespace litbook::journal::testing
