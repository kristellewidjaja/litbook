#pragma once

#include <concepts>
#include <cstddef>
#include <span>

namespace litbook::protocol {

// Reads and writes unsigned integers in big-endian order (most significant byte first),
// which is the byte order of OUCH, ITCH and network protocols in general.
//
// The functions build the value one byte at a time with shifts, so the result is the same
// on any CPU, whatever its own byte order is, and they never read through a misaligned
// pointer. GCC and Clang recognize this loop and turn it into a single load plus a byte
// reverse instruction (rev on ARM, bswap on x86), so it costs no more than a cast.
//
// The caller must make sure that offset + sizeof(T) <= bytes.size(). The codec checks the
// whole message length once before reading any field, so the fields themselves are not
// checked again.

template <std::unsigned_integral T>
[[nodiscard]] constexpr T load_be(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value = static_cast<T>((value << 8U) | std::to_integer<T>(bytes[offset + i]));
    }
    return value;
}

template <std::unsigned_integral T>
constexpr void store_be(std::span<std::byte> bytes, std::size_t offset, T value) noexcept {
    for (std::size_t i = sizeof(T); i > 0; --i) {
        bytes[offset + i - 1] = static_cast<std::byte>(value & 0xFFU);
        value = static_cast<T>(value >> 8U);
    }
}

// Reads and writes unsigned integers in little-endian order (least significant byte
// first). The wire protocols are big-endian, but the journal (docs/JOURNAL.md) is a local
// file format, and every machine litbook runs on is little-endian, so its fields use this
// order. The same rules as above apply. The value is built one byte at a time, so the
// result does not depend on the CPU, and the caller checks the length first.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T load_le(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    T value = 0;
    for (std::size_t i = sizeof(T); i > 0; --i) {
        value = static_cast<T>((value << 8U) | std::to_integer<T>(bytes[offset + i - 1]));
    }
    return value;
}

template <std::unsigned_integral T>
constexpr void store_le(std::span<std::byte> bytes, std::size_t offset, T value) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & 0xFFU);
        value = static_cast<T>(value >> 8U);
    }
}

}  // namespace litbook::protocol
