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
// constrained by this concept, so calls are resolved at compile time and can be
// inlined: no virtual dispatch on the hot path.
//
// Behavioral contract (not expressible in a concept, enforced by the shared tests):
// for the same BookConfig and the same sequence of calls, every implementation emits
// exactly the same event stream (see events.hpp) and answers every query identically.
//
// Construction: Book(OrderBookListener&, BookConfig); throws std::invalid_argument for
// a config the implementation cannot support.
template <class Book>
concept OrderBook = requires(Book& book, const Book& cbook, const OrderRequest& request, OrderId id,
                             Price price, Qty qty, Side side, std::size_t n) {
    { book.submit(request) } -> std::same_as<void>;
    { book.cancel(id) } -> std::same_as<void>;
    { book.modify(id, price, qty) } -> std::same_as<void>;
    { cbook.best_bid() } -> std::same_as<std::optional<Price>>;
    { cbook.best_ask() } -> std::same_as<std::optional<Price>>;
    { cbook.volume_at(side, price) } -> std::same_as<AggregateQty>;
    // Allocates its result: a query for tools and tests, not for the matching path.
    { cbook.depth(side, n) } -> std::same_as<std::vector<LevelSummary>>;
    { cbook.contains(id) } -> std::same_as<bool>;
    { cbook.order_count() } -> std::same_as<std::size_t>;
    { cbook.level_count(side) } -> std::same_as<std::size_t>;
    { cbook.config() } -> std::same_as<const BookConfig&>;
    { cbook.check_invariants() } -> std::same_as<bool>;
};

}  // namespace litbook
