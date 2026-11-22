#include "litbook/protocol/byte_order.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace litbook::protocol {
namespace {

template <std::size_t N>
std::array<std::byte, N> bytes(const std::array<unsigned, N>& values) {
    std::array<std::byte, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = static_cast<std::byte>(values[i]);
    }
    return out;
}

TEST(ByteOrderTest, LoadsMostSignificantByteFirst) {
    const auto b = bytes<8>({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});
    EXPECT_EQ(load_be<std::uint8_t>(b, 0), 0x01U);
    EXPECT_EQ(load_be<std::uint16_t>(b, 0), 0x0123U);
    EXPECT_EQ(load_be<std::uint32_t>(b, 0), 0x01234567U);
    EXPECT_EQ(load_be<std::uint64_t>(b, 0), 0x0123456789ABCDEFULL);
}

TEST(ByteOrderTest, LoadsAtAnOffset) {
    // Offset 1 is misaligned for every type wider than a byte, which must not matter.
    const auto b = bytes<9>({0xFF, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});
    EXPECT_EQ(load_be<std::uint32_t>(b, 1), 0x01234567U);
    EXPECT_EQ(load_be<std::uint64_t>(b, 1), 0x0123456789ABCDEFULL);
    EXPECT_EQ(load_be<std::uint16_t>(b, 7), 0xCDEFU);
}

TEST(ByteOrderTest, StoresMostSignificantByteFirst) {
    std::array<std::byte, 10> out{};
    store_be<std::uint64_t>(out, 1, 0x0123456789ABCDEFULL);
    EXPECT_EQ(out, (bytes<10>({0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x00})));
    store_be<std::uint16_t>(out, 8, 0xBEEF);
    EXPECT_EQ(out[8], std::byte{0xBE});
    EXPECT_EQ(out[9], std::byte{0xEF});
}

TEST(ByteOrderTest, ExtremesRoundTrip) {
    std::array<std::byte, 8> out{};
    for (const std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1}, ~std::uint64_t{0},
                                  std::uint64_t{0x8000000000000000ULL}}) {
        store_be(out, 0, v);
        EXPECT_EQ(load_be<std::uint64_t>(out, 0), v);
    }
    store_be(out, 0, std::uint32_t{0xFFFFFFFF});
    EXPECT_EQ(load_be<std::uint32_t>(out, 0), 0xFFFFFFFFU);
}

TEST(ByteOrderTest, WorksAtCompileTime) {
    constexpr std::array<std::byte, 2> kBytes{std::byte{0x12}, std::byte{0x34}};
    static_assert(load_be<std::uint16_t>(kBytes, 0) == 0x1234);
}

TEST(ByteOrderTest, LoadsLeastSignificantByteFirst) {
    const auto b = bytes<9>({0xFF, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01});
    EXPECT_EQ(load_le<std::uint8_t>(b, 1), 0xEFU);
    EXPECT_EQ(load_le<std::uint16_t>(b, 1), 0xCDEFU);
    EXPECT_EQ(load_le<std::uint32_t>(b, 1), 0x89ABCDEFU);
    EXPECT_EQ(load_le<std::uint64_t>(b, 1), 0x0123456789ABCDEFULL);
}

TEST(ByteOrderTest, StoresLeastSignificantByteFirst) {
    std::array<std::byte, 10> out{};
    store_le<std::uint64_t>(out, 1, 0x0123456789ABCDEFULL);
    EXPECT_EQ(out, (bytes<10>({0x00, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, 0x00})));
    store_le<std::uint16_t>(out, 8, 0xBEEF);
    EXPECT_EQ(out[8], std::byte{0xEF});
    EXPECT_EQ(out[9], std::byte{0xBE});
}

TEST(ByteOrderTest, LittleEndianIsTheReverseOfBigEndian) {
    std::array<std::byte, 4> le{};
    std::array<std::byte, 4> be{};
    store_le(le, 0, std::uint32_t{0x11223344});
    store_be(be, 0, std::uint32_t{0x11223344});
    EXPECT_EQ(load_be<std::uint32_t>(le, 0), 0x44332211U);
    EXPECT_EQ(load_le<std::uint32_t>(be, 0), 0x44332211U);
    for (const std::uint64_t v :
         {std::uint64_t{0}, ~std::uint64_t{0}, std::uint64_t{0x8000000000000001ULL}}) {
        std::array<std::byte, 8> out{};
        store_le(out, 0, v);
        EXPECT_EQ(load_le<std::uint64_t>(out, 0), v);
    }
    constexpr std::array<std::byte, 2> kBytes{std::byte{0x12}, std::byte{0x34}};
    static_assert(load_le<std::uint16_t>(kBytes, 0) == 0x3412);
}

}  // namespace
}  // namespace litbook::protocol
