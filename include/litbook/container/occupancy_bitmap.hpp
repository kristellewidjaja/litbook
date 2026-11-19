#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace litbook {

// One bit per position, for example one per price level, where 1 means the level has
// orders. It finds the next or previous set bit 64 positions at a time using count-zero
// instructions (countr_zero and countl_zero). This is how the fast book moves its best
// price past empty levels without visiting each one.
//
// The size is fixed at construction, and nothing is allocated afterwards. Bits at
// positions >= size() are always 0, so searches never return them.
class OccupancyBitmap {
public:
    static constexpr std::size_t kNotFound = std::numeric_limits<std::size_t>::max();

    explicit OccupancyBitmap(std::size_t size) : words_((size + 63) / 64), size_(size) {}

    // These three require i < size() and do not check it.
    void set(std::size_t i) noexcept { words_[i / 64] |= bit(i); }
    void reset(std::size_t i) noexcept { words_[i / 64] &= ~bit(i); }
    [[nodiscard]] bool test(std::size_t i) const noexcept { return (words_[i / 64] & bit(i)) != 0; }

    // Returns the smallest set position >= from, or kNotFound. Any `from` is allowed, and a
    // `from` >= size() gives kNotFound.
    [[nodiscard]] std::size_t find_next(std::size_t from) const noexcept {
        if (from >= size_) {
            return kNotFound;
        }
        std::size_t w = from / 64;
        std::uint64_t word = words_[w] & (~std::uint64_t{0} << (from % 64));  // Clear bits < from.
        while (word == 0) {
            if (++w == words_.size()) {
                return kNotFound;
            }
            word = words_[w];
        }
        return w * 64 + static_cast<std::size_t>(std::countr_zero(word));
    }

    // Returns the largest set position <= from, or kNotFound. If `from` >= size(), the
    // search starts from the end.
    [[nodiscard]] std::size_t find_prev(std::size_t from) const noexcept {
        if (size_ == 0) {
            return kNotFound;
        }
        if (from >= size_) {
            from = size_ - 1;
        }
        std::size_t w = from / 64;
        // Keep the bits <= from by shifting a full mask right until only 0..from%64 remain.
        std::uint64_t word = words_[w] & (~std::uint64_t{0} >> (63 - from % 64));
        while (word == 0) {
            if (w == 0) {
                return kNotFound;
            }
            word = words_[--w];
        }
        return w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(word));
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    static constexpr std::uint64_t bit(std::size_t i) noexcept {
        return std::uint64_t{1} << (i % 64);
    }

    std::vector<std::uint64_t> words_;
    std::size_t size_;
};

}  // namespace litbook
