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
// The hash folds the key. It cuts the 64-bit key into chunks as wide as the table index
// and XORs them together. Exchange IDs are handed out in increasing order, so IDs that
// arrive close together land in nearby slots, and the slots touched by recent orders
// stay in cache. Fibonacci hashing, used before, scatters nearby IDs across the whole
// table. On a full Nasdaq day the fold won 4 of 5 before/after pairs, with the median
// replay CPU time about 6% lower (see DECISIONS.md D-039). Every bit of the key still
// reaches the index, so keys that differ only in high bits do not all collide. It is not
// hardened against adversarial keys, which is fine for exchange-assigned IDs.
// Entries are kept in Robin Hood order (D-070). Each slot remembers how far its key sits
// from the key's home slot. An insert that reaches an entry closer to its home than the
// new key already is puts the new key there and carries the old entry on, so along any
// run the distances never jump up by more than one from one slot to the next. That lets
// a lookup stop as soon as it passes an entry closer to home than the key it wants, and
// it lets an erase stop as soon as the next entry already sits in its home slot.
// Erase uses backward-shift deletion instead of tombstones (markers left in deleted
// slots), so the table never fills up with dead slots under heavy churn. The fold hash
// gives a range of recent IDs a long run of slots that each hold their own key. Without
// the Robin Hood order, an erase near the front of such a run had to read every slot to
// its end, because some later key might have needed to move back.
//
// These invariants always hold. size() <= max_entries(). Every stored key is reachable
// from its home slot without crossing an empty slot. No key is stored twice. Each slot's
// distance is its key's real distance from home, and in a run the next slot's distance
// is at most one more than this slot's.
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
        bits_ = std::countr_zero(table);
    }

    // Stores `value` under `key`. Returns false and changes nothing if the key is already
    // present or the map already holds max_entries() keys. `value` must not be kNotFound.
    bool insert(Key key, Value value) noexcept {
        if (size_ == max_entries_ || find_slot(key) != kNoSlot) {
            return false;
        }
        // The entry being placed. It starts as the new key and becomes whichever entry was
        // moved out of the way.
        Slot carried{.key = key, .value = value, .distance = 0};
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& slot = slots_[i];
            if (slot.value == kNotFound) {
                slot = carried;
                ++size_;
                return true;
            }
            if (slot.distance < carried.distance) {
                // This entry is closer to its home than the carried one, so the carried
                // one takes the slot and the old entry looks further on.
                std::swap(slot, carried);
            }
            ++carried.distance;
        }
    }

    // Returns the value for `key`, or kNotFound.
    [[nodiscard]] Value find(Key key) const noexcept {
        const std::size_t i = find_slot(key);
        return i == kNoSlot ? kNotFound : slots_[i].value;
    }

    [[nodiscard]] bool contains(Key key) const noexcept { return find(key) != kNotFound; }

    // Removes `key`. Returns false if it was not present.
    bool erase(Key key) noexcept {
        std::size_t hole = find_slot(key);
        if (hole == kNoSlot) {
            return false;
        }
        // Backward shift. Every entry after the hole that is not in its home slot moves back
        // by one, so no later lookup stops early at the hole. In Robin Hood order those
        // entries come right after the hole, and the first empty slot or the first entry
        // already in its home slot ends them.
        for (std::size_t next = (hole + 1) & mask_;
             slots_[next].value != kNotFound && slots_[next].distance > 0;
             next = (next + 1) & mask_) {
            ++erase_walk_;
            slots_[hole] = slots_[next];
            --slots_[hole].distance;
            hole = next;
        }
        slots_[hole] = Slot{};
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }
    [[nodiscard]] std::size_t table_size() const noexcept { return slots_.size(); }
    // How many entries every erase so far has moved back by one slot. Tests use it to check
    // that an erase does not walk a whole run of keys.
    [[nodiscard]] std::uint64_t erase_walk() const noexcept { return erase_walk_; }

    // The longest distance any stored key sits from its home slot, found by scanning the
    // whole table in O(table size). A lookup for a key never reads more slots than this
    // plus one, so tests use it to check that keys do not pile up. Meant for tests.
    [[nodiscard]] std::size_t longest_probe() const noexcept {
        std::size_t longest = 0;
        for (const Slot& slot : slots_) {
            if (slot.value != kNotFound) {
                longest = std::max<std::size_t>(longest, slot.distance);
            }
        }
        return longest;
    }

    // Checks the probe invariant for every stored key in O(table_size()) time. For tests.
    [[nodiscard]] bool check_invariants() const {
        std::size_t count = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].value == kNotFound) {
                continue;
            }
            ++count;
            const std::size_t home_slot = home(slots_[i].key);
            if (slots_[i].distance != ((i - home_slot) & mask_)) {
                return false;  // The stored distance is wrong.
            }
            for (std::size_t p = home_slot; p != i; p = (p + 1) & mask_) {
                if (slots_[p].value == kNotFound || slots_[p].key == slots_[i].key) {
                    return false;  // A gap before reaching the key, or a duplicate.
                }
            }
            // In Robin Hood order an entry is at most one step further from home than the
            // entry before it.
            const Slot& before = slots_[(i - 1) & mask_];
            if (slots_[i].distance > 0 && slots_[i].distance > before.distance + 1) {
                return false;
            }
        }
        return count == size_ && size_ <= max_entries_;
    }

private:
    struct Slot {
        Key key = 0;
        Value value = kNotFound;  // kNotFound marks an empty slot, because any key value is legal.
        // How many slots past its home slot the key sits. It fills what would otherwise be
        // padding, so a slot is still 16 bytes.
        std::uint32_t distance = 0;
    };
    static constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

    // The slot that holds `key`, or kNoSlot. The search stops at an empty slot, or at an
    // entry closer to its home than `key` would be there, because in Robin Hood order
    // `key` would have taken that entry's place.
    [[nodiscard]] std::size_t find_slot(Key key) const noexcept {
        std::uint32_t distance = 0;
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& slot = slots_[i];
            if (slot.value == kNotFound || slot.distance < distance) {
                return kNoSlot;
            }
            if (slot.key == key) {
                return i;
            }
            ++distance;
        }
    }

    // The slot where a key's probe sequence starts.
    [[nodiscard]] std::size_t home(Key key) const noexcept {
        Key h = key;
        for (int s = bits_; s < 64; s += bits_) {
            h ^= key >> s;
        }
        return static_cast<std::size_t>(h) & mask_;
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    int bits_ = 0;  // width of a table index, log2 of the table size
    std::uint64_t erase_walk_ = 0;
    std::size_t size_ = 0;
    std::size_t max_entries_;
};

}  // namespace litbook
