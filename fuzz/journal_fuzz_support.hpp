#pragma once

// Pieces shared by the journal fuzzer and the snapshot fuzzer. Both build a small test
// exchange from the fuzz input and compare a live order entry layer with a rebuilt one.

#include "litbook/book/order_book.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "support/unwrap.hpp"

namespace litbook::fuzz {

inline void check(bool ok) {
    if (!ok) {
        std::abort();
    }
}

// Reads the fuzz input front to back. Past the end every read gives 0.
class Input {
public:
    explicit Input(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    [[nodiscard]] bool empty() const noexcept { return at_ >= bytes_.size(); }
    std::uint8_t byte() noexcept { return empty() ? 0 : bytes_[at_++]; }
    std::uint16_t u16() noexcept {
        const auto high = static_cast<std::uint16_t>(byte());
        return static_cast<std::uint16_t>((high << 8U) | byte());
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
};

class Replies final : public protocol::OutboundSink {
public:
    void send(OwnerId owner, std::span<const std::byte> frame) override {
        sent.push_back(owner);
        for (const std::byte b : frame) {
            sent.push_back(std::to_integer<std::uint32_t>(b));
        }
    }
    // Every reply flattened into one list of numbers, which is enough to compare two runs.
    std::vector<std::uint32_t> sent;
};

// The exchange that every structured journal sets up.
constexpr std::array<const char*, 3> kStocks{"AAPL", "MSFT", "ZVZZT"};
constexpr Price kTick = 100;

inline BookConfig stock_config() {
    BookConfig c;
    c.tick_size = kTick;
    c.min_price = 4'900 * kTick;
    c.max_price = 5'100 * kTick;
    c.max_orders = 64;
    return c;
}

inline protocol::OrderEntryConfig session_config() {
    return {.max_owners = 4, .max_live_orders = 256, .max_symbols = kStocks.size()};
}

// One request built from the input. Most are real orders, so replay gets deep into the
// order entry layer, and some are raw bytes.
inline pipeline::Request next_request(Input& in, pipeline::Timestamp& now) {
    now += in.byte();
    const std::uint8_t op = in.byte();
    const std::uint8_t who = in.byte();
    OwnerId owner = 1 + who % 4U;
    if (who >= 250) {
        owner = who == 250 ? kNoOwner : 5;  // No such account.
    }
    switch (op % 10U) {
        case 7:
            return pipeline::make_cancel_all_request(owner, now);
        case 8:
            return pipeline::make_stop_request(now);
        case 9: {
            std::array<std::byte, journal::kMaxPayload> raw{};
            const std::size_t n = 1 + in.byte() % journal::kMaxPayload;
            for (std::size_t i = 0; i < n; ++i) {
                raw[i] = static_cast<std::byte>(in.byte());
            }
            return pipeline::make_frame_request(owner, std::span<const std::byte>(raw.data(), n),
                                                now);
        }
        default:
            break;
    }
    const std::uint8_t a = in.byte();
    const std::uint8_t b = in.byte();
    const auto user_ref = static_cast<protocol::UserRefNum>(1 + in.byte());
    const Side side = (a & 1U) != 0 ? Side::Buy : Side::Sell;
    const Price price = (5'000 - 20 + static_cast<Price>(b % 41U)) * kTick;
    const Qty qty = 10 * (1 + static_cast<Qty>(a >> 4U));
    protocol::InboundMessage message =
        protocol::CancelOrder{.user_ref = user_ref, .qty = 10U * (b % 3U)};
    if (op % 10U < 4) {
        protocol::EnterOrder enter{.user_ref = user_ref,
                                   .side = side,
                                   .qty = qty,
                                   .symbol = testing::unwrap(make_ticker(kStocks.at(a % 3U))),
                                   .price = price,
                                   .tif = static_cast<TimeInForce>((a >> 1U) % 3U),
                                   .type = OrderType::Limit,
                                   .post_only = (b & 0x80U) != 0};
        if ((a & 0x06U) == 0x06U) {
            enter.type = OrderType::Market;
            enter.tif = TimeInForce::ImmediateOrCancel;
            enter.price = 0;
        }
        message = enter;
    } else if (op % 10U < 6) {
        message = protocol::ReplaceOrder{
            .orig_user_ref = user_ref,
            .user_ref = static_cast<protocol::UserRefNum>(user_ref + 1 + b % 4U),
            .qty = qty,
            .price = price};
    }
    std::array<std::byte, protocol::kMaxMessageLength> frame{};
    const std::size_t n = protocol::encode(message, frame);
    return pipeline::make_frame_request(owner, std::span<const std::byte>(frame.data(), n), now);
}

template <class Book>
inline void apply(protocol::OrderEntry<Book>& orders, const pipeline::Request& r) {
    if (r.kind == pipeline::RequestKind::Frame) {
        static_cast<void>(orders.on_frame(r.owner, r.bytes(), r.now));
    } else if (r.kind == pipeline::RequestKind::CancelAll) {
        orders.cancel_all(r.owner, r.now);
    }
}

// Everything visible about two order entry layers must agree.
template <class A, class B>
inline void check_same_state(const protocol::OrderEntry<A>& a, const protocol::OrderEntry<B>& b) {
    check(a.live_orders() == b.live_orders());
    check(a.stats() == b.stats());
    check(a.engine().last_match_id() == b.engine().last_match_id());
    check(a.engine().symbol_count() == b.engine().symbol_count());
    check(a.engine().check_invariants() && b.engine().check_invariants());
    for (OwnerId owner = 1; owner <= a.config().max_owners; ++owner) {
        check(a.open_orders(owner) == b.open_orders(owner));
        check(a.last_user_ref(owner) == b.last_user_ref(owner));
    }
    for (std::size_t s = 0; s < a.engine().symbol_count(); ++s) {
        const auto id = static_cast<SymbolId>(s);
        for (const Side side : {Side::Buy, Side::Sell}) {
            const auto& ba = *a.engine().book(id);
            const auto& bb = *b.engine().book(id);
            check(ba.depth(side, ba.level_count(side)) == bb.depth(side, bb.level_count(side)));
        }
    }
}

}  // namespace litbook::fuzz
