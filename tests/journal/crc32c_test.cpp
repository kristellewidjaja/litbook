#include "litbook/journal/crc32c.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace litbook::journal {
namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::vector<std::byte> filled(std::size_t n, unsigned value) {
    return std::vector<std::byte>(n, static_cast<std::byte>(value));
}

// The examples of RFC 3720 appendix B.4. The RFC prints each CRC as the four bytes in the
// order they are sent, lowest byte first, so "aa 36 91 8a" is the number 0x8A9136AA.
struct Example {
    const char* name;
    std::vector<std::byte> data;
    std::uint32_t crc;
};

std::vector<Example> rfc_examples() {
    std::vector<std::byte> increasing(32);
    std::vector<std::byte> decreasing(32);
    for (std::size_t i = 0; i < 32; ++i) {
        increasing[i] = static_cast<std::byte>(i);
        decreasing[i] = static_cast<std::byte>(31 - i);
    }
    // An iSCSI SCSI Read (10) Command PDU, the last example in the RFC.
    const std::array<unsigned, 48> pdu{0x01, 0xc0, 0, 0,    0,    0, 0, 0,    0,    0, 0,    0,
                                       0,    0,    0, 0,    0x14, 0, 0, 0,    0,    0, 0x04, 0,
                                       0,    0,    0, 0x14, 0,    0, 0, 0x18, 0x28, 0, 0,    0,
                                       0,    0,    0, 0,    0x02, 0, 0, 0,    0,    0, 0,    0};
    std::vector<std::byte> pdu_bytes;
    pdu_bytes.reserve(pdu.size());
    for (const unsigned b : pdu) {
        pdu_bytes.push_back(static_cast<std::byte>(b));
    }
    return {
        {"32 zero bytes", filled(32, 0x00), 0x8A9136AAU},
        {"32 bytes of ones", filled(32, 0xFF), 0x62A8AB43U},
        {"32 increasing bytes", increasing, 0x46DD794EU},
        {"32 decreasing bytes", decreasing, 0x113FDB5CU},
        {"SCSI Read PDU", pdu_bytes, 0xD9963A56U},
        // Not from the RFC, but the usual check value of CRC catalogues.
        {"123456789", bytes_of("123456789"), 0xE3069283U},
    };
}

TEST(Crc32cTest, MatchesTheRfcExamples) {
    for (const Example& e : rfc_examples()) {
        EXPECT_EQ(crc32c(e.data), e.crc) << e.name;
        EXPECT_EQ(crc32c_software(0, e.data), e.crc) << e.name;
        if (crc32c_hardware_available()) {
            EXPECT_EQ(crc32c_hardware(0, e.data), e.crc) << e.name;
        }
    }
}

TEST(Crc32cTest, NoBytesGiveZero) {
    EXPECT_EQ(crc32c({}), 0U);
    EXPECT_EQ(crc32c_software(0, {}), 0U);
    // Extending by nothing leaves any CRC as it is.
    EXPECT_EQ(crc32c_extend(0x12345678U, {}), 0x12345678U);
}

TEST(Crc32cTest, OneChangedBitChangesTheResult) {
    std::vector<std::byte> data = bytes_of("a journal record of some length");
    const std::uint32_t original = crc32c(data);
    for (std::size_t i = 0; i < data.size(); ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            data[i] ^= static_cast<std::byte>(1U << bit);
            EXPECT_NE(crc32c(data), original) << "byte " << i << " bit " << bit;
            data[i] ^= static_cast<std::byte>(1U << bit);
        }
    }
}

TEST(Crc32cTest, ExtendingInPiecesEqualsOneCall) {
    const std::vector<std::byte> data = bytes_of("The quick brown fox jumps over the lazy dog");
    const std::uint32_t whole = crc32c(data);
    for (std::size_t split = 0; split <= data.size(); ++split) {
        const std::span<const std::byte> all(data);
        const std::uint32_t first = crc32c(all.first(split));
        EXPECT_EQ(crc32c_extend(first, all.subspan(split)), whole) << "split at " << split;
        EXPECT_EQ(crc32c_software(crc32c_software(0, all.first(split)), all.subspan(split)), whole);
    }
}

TEST(Crc32cTest, HardwareEqualsSoftwareForEveryLengthAndAlignment) {
    if (!crc32c_hardware_available()) {
        GTEST_SKIP() << "this CPU has no CRC32C instruction";
    }
    std::mt19937_64 rng(9);
    std::vector<std::byte> data(512);
    for (std::byte& b : data) {
        b = static_cast<std::byte>(rng() & 0xFFU);
    }
    // Every start offset inside one 8-byte word and every length up to 300 covers the
    // 8-byte loop, the 1-byte tail, and every way they can meet.
    for (std::size_t start = 0; start < 8; ++start) {
        for (std::size_t length = 0; length <= 300; ++length) {
            const auto piece = std::span<const std::byte>(data).subspan(start, length);
            ASSERT_EQ(crc32c_hardware(0, piece), crc32c_software(0, piece))
                << "start " << start << " length " << length;
            ASSERT_EQ(crc32c_hardware(0xDEADBEEFU, piece), crc32c_software(0xDEADBEEFU, piece));
        }
    }
}

TEST(Crc32cTest, NamesTheImplementationInUse) {
    const std::string_view name = crc32c_implementation();
    EXPECT_EQ(name, crc32c_hardware_available() ? "hardware" : "software");
    // Both arm64 with the CRC extension and x86 with SSE4.2 have the instruction. The
    // dev container (arm64) and the CI runners (x86) both do, so print which ran.
    std::cout << "crc32c implementation: " << name << '\n';
}

}  // namespace
}  // namespace litbook::journal
