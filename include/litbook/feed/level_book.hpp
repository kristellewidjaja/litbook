#pragma once

#include "litbook/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace litbook::feed {

struct PriceLevel {
    Price price = 0;
    AggregateQty total_qty = 0;
    std::uint32_t order_count = 0;
    bool operator==(const PriceLevel&) const = default;
};

// The price levels of one stock as a market by price feed describes them, with no single
// orders. The publisher keeps one per stock to build snapshots from, and the feed handler
// keeps one per stock as its copy of the exchange's book.
//
// Each side is a sorted vector with the best price at the back. Most updates touch levels
// near the best price, so an insert or erase there moves only a few elements. A lookup is
// a binary search. Both vectors reserve max_levels_per_side in the constructor and never
// grow past it, so set() never allocates.
//
// Invariants. On each side, prices are strictly ordered from worst to best (bids
// ascending, asks descending), every level has a nonzero quantity and order count, and
// there are at most max_levels_per_side levels.
class LevelBook {
public:
    explicit LevelBook(std::size_t max_levels_per_side) : max_levels_(max_levels_per_side) {
        bids_.reserve(max_levels_);
        asks_.reserve(max_levels_);
    }

    // Sets the level at `price` to the given total. A quantity and count of 0 removes the
    // level (and does nothing if there is none). Returns false and changes nothing if a
    // new level would not fit, or if only one of quantity and count is 0.
    bool set(Side side, Price price, AggregateQty total_qty, std::uint32_t order_count) {
        if ((total_qty == 0) != (order_count == 0)) {
            return false;
        }
        std::vector<PriceLevel>& levels = side_of(side);
        const auto it = position(side, price);
        const bool found = it != levels.end() && it->price == price;
        if (total_qty == 0) {
            if (found) {
                levels.erase(it);
            }
            return true;
        }
        if (found) {
            it->total_qty = total_qty;
            it->order_count = order_count;
            return true;
        }
        if (levels.size() >= max_levels_) {
            return false;
        }
        levels.insert(
            it, PriceLevel{.price = price, .total_qty = total_qty, .order_count = order_count});
        return true;
    }

    [[nodiscard]] std::size_t level_count(Side side) const noexcept { return side_of(side).size(); }
    [[nodiscard]] std::size_t max_levels_per_side() const noexcept { return max_levels_; }
    [[nodiscard]] bool empty() const noexcept { return bids_.empty() && asks_.empty(); }

    // The level `rank` places from the best one (rank 0 is the best). The caller makes
    // sure rank < level_count(side).
    [[nodiscard]] const PriceLevel& level(Side side, std::size_t rank) const noexcept {
        const std::vector<PriceLevel>& levels = side_of(side);
        return levels[levels.size() - 1 - rank];
    }

    [[nodiscard]] std::optional<PriceLevel> best(Side side) const noexcept {
        const std::vector<PriceLevel>& levels = side_of(side);
        if (levels.empty()) {
            return std::nullopt;
        }
        return levels.back();
    }

    // The level at exactly `price`, or nothing if that price has no orders.
    [[nodiscard]] std::optional<PriceLevel> find(Side side, Price price) const noexcept {
        const std::vector<PriceLevel>& levels = side_of(side);
        const auto it =
            std::ranges::lower_bound(levels, price, worse_than(side), &PriceLevel::price);
        if (it == levels.end() || it->price != price) {
            return std::nullopt;
        }
        return *it;
    }

    void clear() noexcept {
        bids_.clear();
        asks_.clear();
    }

    // Makes this book hold the same levels as `other` without allocating. Returns false
    // and changes nothing if `other` has more levels on a side than this book can hold.
    bool copy_from(const LevelBook& other) {
        if (other.bids_.size() > max_levels_ || other.asks_.size() > max_levels_) {
            return false;
        }
        bids_.assign(other.bids_.begin(), other.bids_.end());
        asks_.assign(other.asks_.begin(), other.asks_.end());
        return true;
    }

    // Two books are equal when they hold the same levels. Their capacities may differ.
    bool operator==(const LevelBook& other) const noexcept {
        return bids_ == other.bids_ && asks_ == other.asks_;
    }

    // Checks every invariant listed above. Meant for tests and the fuzzer.
    [[nodiscard]] bool check_invariants() const noexcept {
        return side_is_valid(Side::Buy) && side_is_valid(Side::Sell);
    }

private:
    // A comparison that puts the worse of two prices first, which is the order the
    // vectors are kept in. For bids a lower price is worse, for asks a higher one.
    struct WorseThan {
        Side side;
        bool operator()(Price a, Price b) const noexcept {
            return side == Side::Buy ? a < b : a > b;
        }
    };
    static WorseThan worse_than(Side side) noexcept { return WorseThan{side}; }

    std::vector<PriceLevel>& side_of(Side side) noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }
    [[nodiscard]] const std::vector<PriceLevel>& side_of(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    // The first level that is not worse than `price`, which is where `price` is or would go.
    std::vector<PriceLevel>::iterator position(Side side, Price price) {
        return std::ranges::lower_bound(side_of(side), price, worse_than(side), &PriceLevel::price);
    }

    [[nodiscard]] bool side_is_valid(Side side) const noexcept {
        const std::vector<PriceLevel>& levels = side_of(side);
        if (levels.size() > max_levels_) {
            return false;
        }
        for (std::size_t i = 0; i < levels.size(); ++i) {
            if (levels[i].total_qty == 0 || levels[i].order_count == 0) {
                return false;
            }
            if (i > 0 && !worse_than(side)(levels[i - 1].price, levels[i].price)) {
                return false;
            }
        }
        return true;
    }

    std::size_t max_levels_;
    std::vector<PriceLevel> bids_;  // Ascending price, so the highest bid is at the back.
    std::vector<PriceLevel> asks_;  // Descending price, so the lowest ask is at the back.
};

}  // namespace litbook::feed
