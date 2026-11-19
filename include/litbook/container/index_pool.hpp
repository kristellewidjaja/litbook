#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace litbook {

// Fixed-capacity pool of T slots addressed by 32-bit index. All memory is allocated in
// the constructor; acquire() and release() never allocate and are O(1).
//
// Indexes instead of pointers: they are half the size of a pointer (denser linked
// lists), stay valid if the pool is moved, and are easy to check against capacity().
//
// Freed slots are reused last-in first-out, so the next order usually lands in a slot
// that is still in cache.
//
// Invariants: 0 <= in_use() <= capacity(); the free stack holds exactly the indexes not
// currently acquired, each once. Slot contents are not reset: the caller initializes a
// slot after acquire().
// Precondition (unchecked, for speed): release(i) only for an i that is currently acquired.
template <class T>
class IndexPool {
public:
    using Index = std::uint32_t;
    static constexpr Index kNone = std::numeric_limits<Index>::max();

    // Throws std::invalid_argument if capacity is 0 or kNone (kNone is the "no slot" marker).
    explicit IndexPool(Index capacity) : slots_(checked(capacity)) {
        free_.reserve(capacity);
        for (Index i = capacity; i > 0; --i) {
            free_.push_back(i - 1);  // so acquire() hands out 0, 1, 2, ... first
        }
    }

    // A free slot's index, or kNone if every slot is in use.
    [[nodiscard]] Index acquire() noexcept {
        if (free_.empty()) {
            return kNone;
        }
        const Index i = free_.back();
        free_.pop_back();
        return i;
    }

    // Returns slot i to the pool. Never reallocates: the free stack was reserved to
    // capacity() and can never hold more than that.
    void release(Index i) noexcept { free_.push_back(i); }

    [[nodiscard]] T& operator[](Index i) noexcept { return slots_[i]; }
    [[nodiscard]] const T& operator[](Index i) const noexcept { return slots_[i]; }

    [[nodiscard]] Index capacity() const noexcept { return static_cast<Index>(slots_.size()); }
    [[nodiscard]] Index in_use() const noexcept {
        return static_cast<Index>(slots_.size() - free_.size());
    }
    [[nodiscard]] bool full() const noexcept { return free_.empty(); }

private:
    // Validates before slots_ allocates (a member initializer runs before the body).
    static Index checked(Index capacity) {
        if (capacity == 0 || capacity == kNone) {
            throw std::invalid_argument("IndexPool capacity must be in [1, 2^32 - 2]");
        }
        return capacity;
    }

    std::vector<T> slots_;
    std::vector<Index> free_;  // stack; back() is handed out next
};

}  // namespace litbook
