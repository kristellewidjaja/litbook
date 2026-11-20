#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace litbook::gateway {

// A fixed-capacity byte queue used for one connection's input or output. Bytes are added
// at the back and taken from the front. The memory is allocated once in the constructor
// and the capacity never changes, so a slow or hostile peer cannot make it grow.
//
// The bytes waiting in the queue are always one contiguous span (readable()), which is
// what a packet parser or a write() call wants. When the free space at the back runs
// out, the waiting bytes are moved to the front (compact()). A ring buffer would never
// move bytes, but it would split a packet across the end of the memory. Packets here are
// small and the queue usually drains completely, so the move is rare and short.
//
// These invariants always hold. 0 <= begin_ <= end_ <= capacity(). The waiting bytes are
// exactly storage_[begin_, end_). When the queue becomes empty both positions go back to
// 0, so the whole capacity is free again without a move.
class ByteBuffer {
public:
    // Throws std::invalid_argument for a capacity of 0.
    explicit ByteBuffer(std::size_t capacity) : storage_(checked(capacity)) {}

    // The bytes waiting to be read, oldest first.
    [[nodiscard]] std::span<const std::byte> readable() const noexcept {
        return std::span<const std::byte>(storage_).subspan(begin_, end_ - begin_);
    }

    // The free space after the waiting bytes. Write into it and then call commit().
    [[nodiscard]] std::span<std::byte> writable() noexcept {
        return std::span<std::byte>(storage_).subspan(end_);
    }

    // Marks the first n bytes of writable() as filled. n must not be more than
    // writable().size(), and this is not checked, for speed.
    void commit(std::size_t n) noexcept { end_ += n; }

    // Drops the first n waiting bytes. n is capped at size().
    void consume(std::size_t n) noexcept {
        begin_ += std::min(n, size());
        if (begin_ == end_) {
            begin_ = 0;
            end_ = 0;
        }
    }

    // Moves the waiting bytes to the front, so all free space is in writable().
    void compact() noexcept {
        if (begin_ == 0) {
            return;
        }
        const std::size_t n = size();
        // The two ranges can overlap, so this has to be memmove and not memcpy.
        std::memmove(storage_.data(), storage_.data() + begin_, n);
        begin_ = 0;
        end_ = n;
    }

    // Makes sure writable() has at least n bytes, compacting if needed. Returns false if
    // the buffer does not have n free bytes even after compacting.
    [[nodiscard]] bool make_room(std::size_t n) noexcept {
        if (n > free_space()) {
            return false;
        }
        if (n > capacity() - end_) {
            compact();
        }
        return true;
    }

    // Adds all of `bytes` at the back. Returns false and adds nothing if they do not fit.
    [[nodiscard]] bool append(std::span<const std::byte> bytes) noexcept {
        if (!make_room(bytes.size())) {
            return false;
        }
        copy_in(bytes);
        return true;
    }

    // Adds as many of `bytes` as fit and returns how many that was.
    std::size_t append_some(std::span<const std::byte> bytes) noexcept {
        const std::size_t n = std::min(bytes.size(), free_space());
        compact();
        copy_in(bytes.first(n));
        return n;
    }

    void clear() noexcept {
        begin_ = 0;
        end_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return end_ - begin_; }
    [[nodiscard]] bool empty() const noexcept { return begin_ == end_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] std::size_t free_space() const noexcept { return capacity() - size(); }

private:
    static std::size_t checked(std::size_t capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("ByteBuffer capacity must be at least 1");
        }
        return capacity;
    }

    // Copies bytes into writable(). The caller has made room for them.
    void copy_in(std::span<const std::byte> bytes) noexcept {
        // An empty span may have a null data pointer, which memcpy must not get.
        if (bytes.empty()) {
            return;
        }
        std::memcpy(storage_.data() + end_, bytes.data(), bytes.size());
        end_ += bytes.size();
    }

    std::vector<std::byte> storage_;
    std::size_t begin_ = 0;  // The first waiting byte.
    std::size_t end_ = 0;    // One past the last waiting byte.
};

}  // namespace litbook::gateway
