#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace litbook::journal {

// CRC32C, the 32-bit cyclic redundancy check with the Castagnoli polynomial 0x1EDC6F41
// (0x82F63B78 when the bits are reflected, as the code uses it). It is the checksum of
// iSCSI (RFC 3720), and it is the one CRC that both arm64 (the CRC extension) and x86
// (SSE4.2) can compute with a single instruction. Every journal record ends with the
// CRC32C of its other bytes, so a reader can tell a record that was only half written, or
// changed on disk, from a good one.
//
// The register starts as all ones and the result is inverted at the end, as RFC 3720
// asks. So crc32c of 32 zero bytes is 0x8A9136AA, and crc32c of the 9 ASCII bytes
// "123456789" is 0xE3069283.
//
// crc32c() uses the hardware instruction when the CPU has it and the table otherwise. The
// choice is made once, the first time it is called. Both give the same result for every
// input, which the tests check.

// The CRC32C of `bytes`.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

// Continues a CRC32C. `crc` is the finished CRC32C of some earlier bytes, and the result
// is the CRC32C of those bytes followed by `bytes`. crc32c_extend(0, b) equals crc32c(b),
// because 0 is the CRC32C of no bytes at all.
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t crc,
                                          std::span<const std::byte> bytes) noexcept;

// The table version, one byte per step. It is the reference the hardware version is
// checked against, and the fallback for a CPU without the instruction.
[[nodiscard]] std::uint32_t crc32c_software(std::uint32_t crc,
                                            std::span<const std::byte> bytes) noexcept;

// True if this CPU has a CRC32C instruction that crc32c_hardware can use.
[[nodiscard]] bool crc32c_hardware_available() noexcept;

// The hardware version, 8 bytes per instruction. Call it only when
// crc32c_hardware_available() is true. On a CPU without the instruction it would crash.
[[nodiscard]] std::uint32_t crc32c_hardware(std::uint32_t crc,
                                            std::span<const std::byte> bytes) noexcept;

// "hardware" or "software", whichever crc32c() uses on this machine. Tools print it.
[[nodiscard]] const char* crc32c_implementation() noexcept;

}  // namespace litbook::journal
