// This is a differential fuzz target. The fuzzer's bytes become a book configuration and a
// sequence of operations, which run through the naive oracle, MapOrderBook and
// FastOrderBook. Any disagreement in events or queries, or a broken invariant, aborts
// (libFuzzer then saves the input as a crash file).
//
// The input uses fixed-size records, so a one-byte mutation changes one field of one
// operation instead of shifting everything after it. The layout is below.
//
//   A 6-byte header holds, in order, the tick, lot, band low, band width, max orders and
//   self-trade policy. Band low is the byte minus 128, in ticks, so bands can be negative.
//   Then each operation takes 5 bytes.
//     [0] kind and flags  bits 0-1 give the kind (0 or 1 submit, 2 cancel, 3 modify)
//                         bit 2 sell, bit 3 market, bits 4-5 time in force
//                         bit 6 post-only
//     [1] order ID        1..32 (a small ID space, so duplicates and cancels of live
//                         orders are common)
//     [2] price           a tick inside the band or up to 4 ticks outside it
//     [3] quantity        0..15 lots
//     [4] owner and odd   bits 0-1 owner (0..3), bit 6 off-lot quantity,
//                         bit 7 off-tick price
//
// Value ranges are deliberately narrow, so even short inputs reach band edges, a full
// book, self-trades and duplicate IDs.

#include "book_fuzzer.hpp"

#include "litbook/book/book_config.hpp"
#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/book/recording_listener.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "book/naive_book.hpp"

namespace litbook::fuzz {
namespace {

constexpr std::size_t kHeaderSize = 6;
constexpr std::size_t kOpSize = 5;
constexpr Price kOutsideTicks = 4;  // how far outside the band generated prices can reach

struct Decoded {
    BookConfig config;
    Price low_tick = 0;  // the band start, in ticks
    Price width = 1;     // the band width in ticks, at least 1
};

Decoded decode_header(std::span<const std::uint8_t, kHeaderSize> h) {
    const Price tick = 1 + h[0] % 4;
    const Qty lot = 1 + h[1] % 3;
    const Price low_tick = static_cast<Price>(h[2]) - 128;  // the band may be negative
    const Price width = 1 + h[3] % 40;
    const BookConfig c{.tick_size = tick,
                       .lot_size = lot,
                       .self_trade_policy = static_cast<SelfTradePolicy>(h[5] % 4),
                       .min_price = low_tick * tick,
                       .max_price = (low_tick + width - 1) * tick,
                       .max_orders = 1U + h[4] % 32U};
    return {.config = c, .low_tick = low_tick, .width = width};
}

struct Op {
    enum class Kind : std::uint8_t { Submit, Cancel, Modify };
    Kind kind;
    OrderRequest request;
};

Op decode_op(const Decoded& d, std::span<const std::uint8_t, kOpSize> b) {
    const BookConfig& c = d.config;
    const unsigned kind = b[0] & 3U;
    const Price span = d.width + 2 * kOutsideTicks;
    Price price = (d.low_tick - kOutsideTicks + b[2] % span) * c.tick_size;
    if ((b[4] & 0x80U) != 0 && c.tick_size > 1) {
        price += 1;  // move the price off the tick grid
    }
    Qty qty = c.lot_size * (b[3] % 16U);
    if ((b[4] & 0x40U) != 0 && c.lot_size > 1) {
        qty += 1;  // make the quantity not a whole number of lots
    }
    const unsigned tif = (b[0] >> 4U) & 3U;
    const OrderRequest r{.id = 1U + b[1] % 32U,
                         .owner = static_cast<OwnerId>(b[4] & 3U),
                         .side = (b[0] & 4U) != 0 ? Side::Sell : Side::Buy,
                         .type = (b[0] & 8U) != 0 ? OrderType::Market : OrderType::Limit,
                         .tif = tif == 1   ? TimeInForce::ImmediateOrCancel
                                : tif == 2 ? TimeInForce::FillOrKill
                                           : TimeInForce::GoodTillCancel,
                         .post_only = (b[0] & 0x40U) != 0,
                         .price = price,
                         .qty = qty};
    const Op::Kind k = kind < 2    ? Op::Kind::Submit
                       : kind == 2 ? Op::Kind::Cancel
                                   : Op::Kind::Modify;
    return {.kind = k, .request = r};
}

template <class Book>
void apply(Book& book, const Op& op) {
    switch (op.kind) {
        case Op::Kind::Submit:
            book.submit(op.request);
            return;
        case Op::Kind::Cancel:
            book.cancel(op.request.id);
            return;
        case Op::Kind::Modify:
            book.modify(op.request.id, op.request.price, op.request.qty);
            return;
    }
}

[[noreturn]] void fail(const char* what, std::size_t op) {
    std::fprintf(stderr, "book_fuzzer: %s at operation %zu\n", what, op);  // NOLINT
    std::abort();
}

}  // namespace

int run_one(const std::uint8_t* data, std::size_t size) {
    if (size < kHeaderSize) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data, size);
    const Decoded d = decode_header(input.first<kHeaderSize>());

    RecordingListener naive_rec;
    RecordingListener map_rec;
    RecordingListener fast_rec;
    RecordingListener sorted_rec;
    testing::NaiveBook naive(naive_rec, d.config);
    MapOrderBook map_book(map_rec, d.config);
    FastOrderBook fast_book(fast_rec, d.config);
    SortedOrderBook sorted_book(sorted_rec, d.config);

    std::size_t index = 0;
    for (std::size_t at = kHeaderSize; at + kOpSize <= size; at += kOpSize, ++index) {
        const Op op = decode_op(d, input.subspan(at).first<kOpSize>());
        apply(naive, op);
        apply(map_book, op);
        apply(fast_book, op);
        apply(sorted_book, op);

        if (map_rec.events() != naive_rec.events()) {
            fail("map events differ from the oracle", index);
        }
        if (fast_rec.events() != naive_rec.events()) {
            fail("fast events differ from the oracle", index);
        }
        if (sorted_rec.events() != naive_rec.events()) {
            fail("sorted events differ from the oracle", index);
        }
        if (!map_book.check_invariants() || !fast_book.check_invariants() ||
            !sorted_book.check_invariants()) {
            fail("invariant broken", index);
        }
        for (const Side side : {Side::Buy, Side::Sell}) {
            if (map_book.depth(side, 64) != fast_book.depth(side, 64) ||
                map_book.level_count(side) != fast_book.level_count(side) ||
                map_book.depth(side, 64) != sorted_book.depth(side, 64) ||
                map_book.level_count(side) != sorted_book.level_count(side)) {
                fail("depth differs", index);
            }
        }
        if (map_book.best_bid() != naive.best(Side::Buy) ||
            map_book.best_ask() != naive.best(Side::Sell) ||
            fast_book.best_bid() != map_book.best_bid() ||
            fast_book.best_ask() != map_book.best_ask() ||
            sorted_book.best_bid() != map_book.best_bid() ||
            sorted_book.best_ask() != map_book.best_ask()) {
            fail("best price differs", index);
        }
        if (map_book.order_count() != naive.order_count() ||
            fast_book.order_count() != naive.order_count() ||
            sorted_book.order_count() != naive.order_count()) {
            fail("order count differs", index);
        }
        naive_rec.clear();
        map_rec.clear();
        fast_rec.clear();
        sorted_rec.clear();
    }
    return 0;
}

}  // namespace litbook::fuzz
