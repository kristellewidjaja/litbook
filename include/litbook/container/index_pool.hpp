#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace litbook {

// Fixed-capacity pool of T slots addressed by 32-bit index. All memory is allocated in
// the constructor, and acquire() and release() never allocate and run in O(1).
//
// We use indexes instead of pointers because they are half the size of a pointer (so
// linked lists are denser), stay valid if the pool is moved, and are easy to check
// against capacity().
//
// Freed slots are reused last-in first-out, so the next order usually lands in a slot
// that is still in cache.
//
// These invariants always hold. 0 <= in_use() <= capacity(), and the free stack holds
// exactly the indexes not currently acquired, each once. Slot contents are not reset, so
// the caller initializes a slot after acquire().
// release(i) must only be called for an i that is currently acquired. For speed, this
// precondition is not checked.
template <class T>
class IndexPool {
public:
    using Index = std::uint32_t;
    static constexpr Index kNone = std::numeric_limits<Index>::max();

    // Throws std::invalid_argument if capacity is 0 or kNone (kNone is the "no slot"
    // marker).
    explicit IndexPool(Index capacity) : slots_(checked(capacity)) {
        free_.reserve(capacity);
        for (Index i = capacity; i > 0; --i) {
            free_.push_back(i - 1);  // So acquire() hands out 0, 1, 2, ... first.
        }
    }

    // Returns a free slot's index, or kNone if every slot is in use.
    [[nodiscard]] Index acquire() noexcept {
        if (free_.empty()) {
            return kNone;
        }
        const Index i = free_.back();
        free_.pop_back();
        return i;
    }

    // Returns slot i to the pool. This never reallocates, because the free stack was
    // reserved to capacity() and can never hold more than that.
    void release(Index i) noexcept { free_.push_back(i); }

    [[nodiscard]] T& operator[](Index i) noexcept { return slots_[i]; }
    [[nodiscard]] const T& operator[](Index i) const noexcept { return slots_[i]; }

    [[nodiscard]] Index capacity() const noexcept { return static_cast<Index>(slots_.size()); }
    [[nodiscard]] Index in_use() const noexcept {
        return static_cast<Index>(slots_.size() - free_.size());
    }
    [[nodiscard]] bool full() const noexcept { return free_.empty(); }

private:
    // Validates the capacity before slots_ allocates, because a member initializer runs
    // before the constructor body.
    static Index checked(Index capacity) {
        if (capacity == 0 || capacity == kNone) {
            throw std::invalid_argument("IndexPool capacity must be in [1, 2^32 - 2]");
        }
        return capacity;
    }

    std::vector<T> slots_;
    std::vector<Index> free_;  // Used as a stack; back() is handed out next.
};

}  // namespace litbook
