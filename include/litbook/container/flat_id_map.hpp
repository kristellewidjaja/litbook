#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace litbook {

// Hash map from a 64-bit key (an order ID) to a 32-bit value (a pool index), with a
// capacity fixed at construction. It uses open addressing with linear probing in one flat
// array, so a lookup reads consecutive slots, usually within one cache line. Nothing is
// allocated after the constructor, unlike std::unordered_map, which allocates a node per
// insert.
//
// The table size is a power of two at least twice max_entries, so the load factor never
// exceeds 50% and probe sequences stay short.
// The hash is Fibonacci hashing. We multiply the key by 2^64 / golden ratio and keep the
// top bits. This spreads sequential IDs, the common case, evenly. It is not hardened
// against adversarial keys, which is fine for exchange-assigned IDs.
// Erase uses backward-shift deletion instead of tombstones (markers left in deleted
// slots), so the table never fills up with dead slots under heavy churn.
//
// These invariants always hold. size() <= max_entries(). Every stored key is reachable
// from its home slot without crossing an empty slot. No key is stored twice.
class FlatIdMap {
public:
    using Key = std::uint64_t;
    using Value = std::uint32_t;
    static constexpr Value kNotFound = std::numeric_limits<Value>::max();

    // Throws std::invalid_argument if max_entries is 0 or too large for a table.
    explicit FlatIdMap(std::size_t max_entries) : max_entries_(max_entries) {
        if (max_entries == 0 || max_entries > (std::size_t{1} << 40)) {
            throw std::invalid_argument("FlatIdMap max_entries must be in [1, 2^40]");
        }
        const std::size_t table = std::bit_ceil(std::max<std::size_t>(2 * max_entries, 16));
        slots_.resize(table);
        mask_ = table - 1;
        shift_ = 64 - std::countr_zero(table);
    }

    // Stores `value` under `key`. Returns false and changes nothing if the key is already
    // present or the map already holds max_entries() keys. `value` must not be kNotFound.
    bool insert(Key key, Value value) noexcept {
        if (size_ == max_entries_) {
            return false;
        }
        std::size_t i = home(key);
        while (slots_[i].value != kNotFound) {
            if (slots_[i].key == key) {
                return false;
            }
            i = (i + 1) & mask_;
        }
        slots_[i] = {.key = key, .value = value};
        ++size_;
        return true;
    }

    // Returns the value for `key`, or kNotFound.
    [[nodiscard]] Value find(Key key) const noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            if (slots_[i].value == kNotFound) {
                return kNotFound;  // An empty slot ends every probe sequence.
            }
            if (slots_[i].key == key) {
                return slots_[i].value;
            }
        }
    }

    [[nodiscard]] bool contains(Key key) const noexcept { return find(key) != kNotFound; }

    // Removes `key`. Returns false if it was not present.
    bool erase(Key key) noexcept {
        std::size_t hole = home(key);
        while (slots_[hole].key != key || slots_[hole].value == kNotFound) {
            if (slots_[hole].value == kNotFound) {
                return false;
            }
            hole = (hole + 1) & mask_;
        }
        // Backward shift. Walk the run of entries after the hole and pull back any entry whose
        // probe path passes through the hole, so no later lookup stops early there.
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value != kNotFound;
             j = (j + 1) & mask_) {
            const std::size_t k = home(slots_[j].key);
            // The entry at j may move into the hole if and only if the hole lies on its path from
            // k to j, which means the hole is no further back from j than its home slot k is.
            if (((j - hole) & mask_) <= ((j - k) & mask_)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].value = kNotFound;
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }
    [[nodiscard]] std::size_t table_size() const noexcept { return slots_.size(); }

    // Checks the probe invariant for every stored key in O(table_size()) time. For tests.
    [[nodiscard]] bool check_invariants() const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].value == kNotFound) {
                continue;
            }
            ++count;
            for (std::size_t p = home(slots_[i].key); p != i; p = (p + 1) & mask_) {
                if (slots_[p].value == kNotFound || slots_[p].key == slots_[i].key) {
                    return false;  // A gap before reaching the key, or a duplicate.
                }
            }
        }
        return count == size_ && size_ <= max_entries_;
    }

private:
    struct Slot {
        Key key = 0;
        Value value = kNotFound;  // kNotFound marks an empty slot, because any key value is legal.
    };

    [[nodiscard]] std::size_t home(Key key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    int shift_ = 0;
    std::size_t size_ = 0;
    std::size_t max_entries_;
};

}  // namespace litbook
