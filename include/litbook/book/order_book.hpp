#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/order_request.hpp"
#include "litbook/types.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
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
template <class Book>
concept OrderBook = requires(Book& book, const Book& cbook, const OrderRequest& request, OrderId id,
                             Price price, Qty qty, Side side, std::size_t n) {
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
};

}  // namespace litbook
