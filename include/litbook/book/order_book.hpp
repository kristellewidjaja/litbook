#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/types.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <vector>

namespace litbook {

// Aggregated view of one price level.
struct LevelSummary {
    Price price;
    AggregateQty total_qty;
    std::uint32_t order_count;
    bool operator==(const LevelSummary&) const = default;
};

// One resting order, as for_each_resting() reports it and restore_resting() takes it.
// A snapshot of a book is the list of these in priority order plus the match counter.
struct RestingOrder {
    OrderId id = 0;
    OwnerId owner = kNoOwner;
    Side side = Side::Buy;
    Price price = 0;
    Qty remaining = 0;
    bool post_only = false;  // Kept so that a later modify still cannot make it cross.
    bool operator==(const RestingOrder&) const = default;
};

// Why restore_resting() refused an order. Each one names a rule the order would break
// if it were put in the book, so a damaged or foreign snapshot cannot build a broken book.
enum class RestoreStatus : std::uint8_t {
    Ok,
    BadPrice,     // Not a multiple of the tick size, or outside the price band.
    BadQuantity,  // Zero, or not a multiple of the lot size.
    DuplicateId,  // An order with this ID is already resting.
    WouldCross,   // The order would trade against the other side, which a resting order never can.
    BookFull,     // The book already holds max_orders orders.
};

std::ostream& operator<<(std::ostream& os, RestoreStatus status);

// The interface every order book implementation provides (MapOrderBook, FastOrderBook).
// Code that works with "any book" (tests, the engine, benchmarks) is a template
// constrained by this concept. Calls are then resolved at compile time and can be
// inlined, so there is no virtual dispatch on the hot path.
//
// The behavioral contract cannot be written as a concept, so the shared tests enforce it.
// For the same BookConfig and the same sequence of calls, every implementation emits
// exactly the same event stream (see events.hpp) and answers every query identically.
//
// Every implementation is constructed as Book(OrderBookListener&, BookConfig) and throws
// std::invalid_argument for a config it cannot support.
//
// Snapshots use the last four calls. for_each_resting(side, visit) calls visit once per
// resting order of that side, best price first and in priority order within a price.
// restore_resting(order) puts one order at the back of its price's queue without any
// event and without matching, so restoring a book's orders in the order for_each_resting
// listed them rebuilds the same queues. next_match_id() and restore_next_match_id() save
// and restore the number the next trade will get. These are startup calls, and
// restore_resting may allocate in MapOrderBook.
template <class Book>
concept OrderBook =
    requires(Book& book, const Book& cbook, const OrderRequest& request, OrderId id, Price price,
             Qty qty, Side side, std::size_t n, const RestingOrder& resting, MatchId match,
             void (*visit)(const RestingOrder&)) {
        { book.submit(request) } -> std::same_as<void>;
        { book.cancel(id) } -> std::same_as<void>;
        { book.modify(id, price, qty) } -> std::same_as<void>;
        { cbook.best_bid() } -> std::same_as<std::optional<Price>>;
        { cbook.best_ask() } -> std::same_as<std::optional<Price>>;
        { cbook.volume_at(side, price) } -> std::same_as<AggregateQty>;
        // depth() allocates its result, so it is a query for tools and tests, not for matching.
        { cbook.depth(side, n) } -> std::same_as<std::vector<LevelSummary>>;
        { cbook.contains(id) } -> std::same_as<bool>;
        { cbook.order_count() } -> std::same_as<std::size_t>;
        { cbook.level_count(side) } -> std::same_as<std::size_t>;
        { cbook.config() } -> std::same_as<const BookConfig&>;
        { cbook.check_invariants() } -> std::same_as<bool>;
        { cbook.for_each_resting(side, visit) } -> std::same_as<void>;
        { book.restore_resting(resting) } -> std::same_as<RestoreStatus>;
        { cbook.next_match_id() } -> std::same_as<MatchId>;
        { book.restore_next_match_id(match) } -> std::same_as<void>;
    };

}  // namespace litbook
