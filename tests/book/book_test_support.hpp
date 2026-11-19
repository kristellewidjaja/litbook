#pragma once

// Shared helpers for tests that run against every order book implementation.

#include "litbook/book/book_config.hpp"
#include "litbook/book/events.hpp"
#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

#include "book/test_limits.hpp"

namespace litbook::testing {

// Every implementation the shared (typed) tests run against.
using BookTypes = ::testing::Types<MapOrderBook, FastOrderBook>;

// Names typed-test instances "Map" and "Fast" instead of 0 and 1.
struct BookTypeNames {
    template <class Book>
    static std::string GetName(int /*index*/) {  // NOLINT(readability-identifier-naming)
        return std::is_same_v<Book, MapOrderBook> ? "Map" : "Fast";
    }
};

// ---- Expected-event builders --------------------------------------------------------

inline BookEvent accepted(const OrderRequest& r) {
    return OrderAccepted{.order_id = r.id,
                         .owner = r.owner,
                         .side = r.side,
                         .type = r.type,
                         .tif = r.tif,
                         .post_only = r.post_only,
                         .price = r.type == OrderType::Market ? 0 : r.price,
                         .qty = r.qty};
}
// A plain limit GoodTillCancel order with no owner.
inline BookEvent accepted(OrderId id, Side side, Price price, Qty qty) {
    return accepted(OrderRequest{.id = id, .side = side, .price = price, .qty = qty});
}
inline BookEvent rejected(OrderId id, RejectReason reason) {
    return OrderRejected{.order_id = id, .reason = reason};
}
inline BookEvent canceled(OrderId id, Qty qty, Qty left, CancelReason reason) {
    return OrderCanceled{
        .order_id = id, .canceled_qty = qty, .remaining_qty = left, .reason = reason};
}
inline BookEvent level(Side side, Price price, AggregateQty total, std::uint32_t count) {
    return LevelUpdate{.side = side, .price = price, .total_qty = total, .order_count = count};
}
inline BookEvent trade(MatchId match, OrderId aggr, OrderId rest, Side aggr_side, Price price,
                       Qty qty, Qty aggr_left, Qty rest_left) {
    return Trade{.match_id = match,
                 .aggressor_id = aggr,
                 .resting_id = rest,
                 .aggressor_side = aggr_side,
                 .price = price,
                 .qty = qty,
                 .aggressor_remaining = aggr_left,
                 .resting_remaining = rest_left};
}
inline BookEvent modified(OrderId id, Side side, Price price, Qty qty, bool kept) {
    return OrderModified{
        .order_id = id, .side = side, .price = price, .qty = qty, .kept_priority = kept};
}

}  // namespace litbook::testing
