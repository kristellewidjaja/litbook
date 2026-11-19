#pragma once

#include "litbook/types.hpp"

namespace litbook {

// A request to enter a new order. It is plain data. The book does the validation and
// rejects an invalid request with a reason (see RejectReason).
//
// A request is valid only if all of these rules hold.
//   1. qty is at least 1 and a multiple of the book's lot size.
//   2. For a limit order, price is a multiple of the book's tick size.
//   3. For a market order, price is ignored, and GoodTillCancel behaves like
//      ImmediateOrCancel.
//   4. post_only requires type == Limit and tif == GoodTillCancel.
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
