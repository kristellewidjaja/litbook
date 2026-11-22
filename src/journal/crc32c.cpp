#include "litbook/journal/crc32c.hpp"

#include "litbook/protocol/byte_order.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#if defined(__aarch64__)
#include <arm_acle.h>
#include <sys/auxv.h>
#elif defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace litbook::journal {

namespace {

// The polynomial with its bits reflected, so the lowest bit is the x^31 term. Reflected
// CRCs shift right, which lets a byte enter the register at the bottom.
constexpr std::uint32_t kReflectedPolynomial = 0x82F63B78U;

// Entry b is the effect of the 8 bits of b on the register. It is built at compile time
// by running the bit-by-bit algorithm on every possible byte.
constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t b = 0; b < 256; ++b) {
        std::uint32_t crc = b;
        for (int bit = 0; bit < 8; ++bit) {
            // If the lowest bit is set, the polynomial is subtracted (xor) after the shift.
            const bool low_bit_set = (crc & 1U) != 0;
            crc >>= 1U;
            if (low_bit_set) {
                crc ^= kReflectedPolynomial;
            }
        }
        table[b] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

// The hardware path needs the CPU to say it has the instruction. This is asked once.
bool detect_hardware() noexcept {
#if defined(__aarch64__)
#ifndef HWCAP_CRC32
    constexpr unsigned long kHwcapCrc32 = 1UL << 7U;  // The Linux value for arm64.
#else
    constexpr unsigned long kHwcapCrc32 = HWCAP_CRC32;
#endif
    return (getauxval(AT_HWCAP) & kHwcapCrc32) != 0;
#elif defined(__x86_64__)
    return __builtin_cpu_supports("sse4.2") != 0;
#else
    return false;
#endif
}

#if defined(__aarch64__)
// The target attribute turns on the CRC instructions for this one function only, so the
// rest of the program never uses them by accident and still runs on a CPU without them.
__attribute__((target("+crc"))) std::uint32_t hardware_register(
    std::uint32_t reg, std::span<const std::byte> bytes) noexcept {
    std::size_t i = 0;
    while (i + 8 <= bytes.size()) {
        reg = __crc32cd(reg, protocol::load_le<std::uint64_t>(bytes, i));
        i += 8;
    }
    while (i < bytes.size()) {
        reg = __crc32cb(reg, std::to_integer<std::uint8_t>(bytes[i]));
        ++i;
    }
    return reg;
}
#elif defined(__x86_64__)
__attribute__((target("sse4.2"))) std::uint32_t hardware_register(
    std::uint32_t reg, std::span<const std::byte> bytes) noexcept {
    std::size_t i = 0;
    std::uint64_t wide = reg;
    while (i + 8 <= bytes.size()) {
        wide = _mm_crc32_u64(wide, protocol::load_le<std::uint64_t>(bytes, i));
        i += 8;
    }
    reg = static_cast<std::uint32_t>(wide);
    while (i < bytes.size()) {
        reg = _mm_crc32_u8(reg, std::to_integer<std::uint8_t>(bytes[i]));
        ++i;
    }
    return reg;
}
#endif

}  // namespace

std::uint32_t crc32c_software(std::uint32_t crc, std::span<const std::byte> bytes) noexcept {
    // The register holds the inverted CRC while bytes go in, as RFC 3720 asks.
    std::uint32_t reg = ~crc;
    for (const std::byte b : bytes) {
        const std::uint32_t index = (reg ^ std::to_integer<std::uint32_t>(b)) & 0xFFU;
        reg = (reg >> 8U) ^ kTable[index];
    }
    return ~reg;
}

bool crc32c_hardware_available() noexcept {
    static const bool available = detect_hardware();
    return available;
}

std::uint32_t crc32c_hardware(std::uint32_t crc, std::span<const std::byte> bytes) noexcept {
#if defined(__aarch64__) || defined(__x86_64__)
    return ~hardware_register(~crc, bytes);
#else
    // There is no hardware path on this CPU, and crc32c_hardware_available() says so.
    return crc32c_software(crc, bytes);
#endif
}

std::uint32_t crc32c_extend(std::uint32_t crc, std::span<const std::byte> bytes) noexcept {
    if (crc32c_hardware_available()) {
        return crc32c_hardware(crc, bytes);
    }
    return crc32c_software(crc, bytes);
}

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    return crc32c_extend(0, bytes);
}

const char* crc32c_implementation() noexcept {
    return crc32c_hardware_available() ? "hardware" : "software";
}

}  // namespace litbook::journal
