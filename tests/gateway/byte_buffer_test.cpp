#include "litbook/gateway/byte_buffer.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace litbook::gateway {
namespace {

using Bytes = std::vector<std::byte>;

// n bytes counting up from `first`, so tests can see which bytes went where.
Bytes counting(std::size_t n, unsigned first = 0) {
    Bytes out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::byte>(first + i);
    }
    return out;
}

Bytes contents(const ByteBuffer& b) {
    const auto r = b.readable();
    return {r.begin(), r.end()};
}

TEST(ByteBuffer, ZeroCapacityThrows) {
    EXPECT_THROW(ByteBuffer(0), std::invalid_argument);
}

TEST(ByteBuffer, StartsEmpty) {
    ByteBuffer b(16);
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.size(), 0U);
    EXPECT_EQ(b.capacity(), 16U);
    EXPECT_EQ(b.free_space(), 16U);
    EXPECT_EQ(b.writable().size(), 16U);
    EXPECT_TRUE(b.readable().empty());
}

TEST(ByteBuffer, AppendThenConsumeInOrder) {
    ByteBuffer b(16);
    ASSERT_TRUE(b.append(counting(5)));
    ASSERT_TRUE(b.append(counting(3, 5)));
    EXPECT_EQ(contents(b), counting(8));
    b.consume(2);
    EXPECT_EQ(contents(b), counting(6, 2));
    EXPECT_EQ(b.free_space(), 10U);
}

TEST(ByteBuffer, ConsumingEverythingFreesTheWholeCapacity) {
    ByteBuffer b(16);
    ASSERT_TRUE(b.append(counting(10)));
    b.consume(4);
    EXPECT_EQ(b.writable().size(), 6U);  // Not compacted yet.
    b.consume(6);
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.writable().size(), 16U);  // Both positions went back to the start.
}

TEST(ByteBuffer, ConsumeIsCappedAtSize) {
    ByteBuffer b(8);
    ASSERT_TRUE(b.append(counting(3)));
    b.consume(100);
    EXPECT_TRUE(b.empty());
    b.consume(1);  // Consuming from an empty buffer does nothing.
    EXPECT_TRUE(b.empty());
}

TEST(ByteBuffer, AppendThatDoesNotFitChangesNothing) {
    ByteBuffer b(8);
    ASSERT_TRUE(b.append(counting(6)));
    EXPECT_FALSE(b.append(counting(3, 6)));
    EXPECT_EQ(contents(b), counting(6));
    EXPECT_TRUE(b.append(counting(2, 6)));  // Exactly full is fine.
    EXPECT_EQ(b.free_space(), 0U);
    EXPECT_FALSE(b.append(counting(1)));
}

TEST(ByteBuffer, AppendCompactsWhenTheBackIsFull) {
    ByteBuffer b(8);
    ASSERT_TRUE(b.append(counting(8)));
    b.consume(5);
    // Only 0 bytes are free at the back, but 5 are free in total.
    EXPECT_EQ(b.writable().size(), 0U);
    ASSERT_TRUE(b.append(counting(4, 8)));
    EXPECT_EQ(contents(b), counting(7, 5));
}

TEST(ByteBuffer, AppendSomeTakesWhatFits) {
    ByteBuffer b(8);
    ASSERT_TRUE(b.append(counting(3)));
    b.consume(1);
    EXPECT_EQ(b.append_some(counting(10, 3)), 6U);
    EXPECT_EQ(contents(b), counting(8, 1));
    EXPECT_EQ(b.append_some(counting(1)), 0U);
    EXPECT_EQ(b.append_some({}), 0U);
}

TEST(ByteBuffer, MakeRoomCompactsOnlyWhenNeeded) {
    ByteBuffer b(8);
    ASSERT_TRUE(b.append(counting(6)));
    b.consume(4);
    ASSERT_TRUE(b.make_room(2));
    EXPECT_EQ(b.writable().size(), 2U);  // There was room at the back already.
    ASSERT_TRUE(b.make_room(5));
    EXPECT_EQ(b.writable().size(), 6U);  // This needed the move.
    EXPECT_EQ(contents(b), counting(2, 4));
    EXPECT_FALSE(b.make_room(7));
}

TEST(ByteBuffer, WriteIntoWritableThenCommit) {
    ByteBuffer b(8);
    const Bytes data = counting(3, 40);
    std::memcpy(b.writable().data(), data.data(), data.size());
    b.commit(data.size());
    EXPECT_EQ(contents(b), data);
}

TEST(ByteBuffer, CompactKeepsTheBytes) {
    ByteBuffer b(8);
    ASSERT_TRUE(b.append(counting(7)));
    b.consume(3);
    b.compact();
    EXPECT_EQ(contents(b), counting(4, 3));
    EXPECT_EQ(b.writable().size(), 4U);
    b.compact();  // Already at the front, so nothing changes.
    EXPECT_EQ(contents(b), counting(4, 3));
}

TEST(ByteBuffer, Clear) {
    ByteBuffer b(8);
    ASSERT_TRUE(b.append(counting(5)));
    b.clear();
    EXPECT_TRUE(b.empty());
    EXPECT_EQ(b.writable().size(), 8U);
}

}  // namespace
}  // namespace litbook::gateway
