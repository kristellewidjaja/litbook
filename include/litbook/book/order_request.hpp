#pragma once

#include "litbook/types.hpp"

namespace litbook {

// A request to enter a new order. Plain data: validation happens in the book, which
// rejects invalid requests with a reason (see RejectReason).
//
// Valid combinations:
//   - qty >= 1 and a multiple of the book's lot size
//   - Limit orders: price is a multiple of the book's tick size
//   - Market orders: price is ignored; GoodTillCancel behaves like ImmediateOrCancel
//   - post_only requires type == Limit and tif == GoodTillCancel
struct OrderRequest {
    OrderId id = 0;
    OwnerId owner = kNoOwner;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GoodTillCancel;
    bool post_only = false;
    Price price = 0;
    Qty qty = 0;
};

}  // namespace litbook
