#include "litbook/itch/binary_file_reader.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <span>
#include <vector>

namespace litbook::itch {
namespace {

using Status = BinaryFileReader::Status;

// Builds a BinaryFILE byte stream: each payload gets a 2-byte big-endian length in front.
class Stream {
public:
    Stream& message(std::size_t length, std::uint8_t fill) {
        bytes_.push_back(static_cast<unsigned char>(length >> 8U));
        bytes_.push_back(static_cast<unsigned char>(length & 0xFFU));
        for (std::size_t i = 0; i < length; ++i) {
            bytes_.push_back(static_cast<unsigned char>(fill + i));
        }
        return *this;
    }
    Stream& end_of_session() { return raw({0, 0}); }
    // Appends bytes one at a time. A range insert here trips a false -Warray-bounds
    // error in GCC 14 at -O3 (the compiler CI uses), so this is deliberately a loop.
    Stream& raw(std::initializer_list<unsigned char> b) {
        for (const unsigned char byte : b) {
            bytes_.push_back(byte);
        }
        return *this;
    }

    // A temporary file holding the stream, rewound to the start and closed automatically.
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> open() const {
        std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(std::tmpfile(), &std::fclose);
        // An empty vector's data() may be null, and fwrite must not get a null pointer
        // even for zero bytes (UBSan reports it), so an empty stream writes nothing.
        if (f && !bytes_.empty()) {
            std::fwrite(bytes_.data(), 1, bytes_.size(), f.get());
        }
        if (f) {
            std::fseek(f.get(), 0, SEEK_SET);
        }
        return f;
    }

private:
    std::vector<unsigned char> bytes_;
};

void expect_message(BinaryFileReader& reader, std::size_t length, std::uint8_t fill) {
    std::span<const std::byte> payload;
    ASSERT_EQ(reader.next(payload), Status::Message);
    ASSERT_EQ(payload.size(), length);
    for (std::size_t i = 0; i < length; ++i) {
        ASSERT_EQ(std::to_integer<std::uint8_t>(payload[i]), static_cast<std::uint8_t>(fill + i))
            << "byte " << i;
    }
}

TEST(BinaryFileReaderTest, ReadsMessagesThenTheEndOfSession) {
    Stream s;
    s.message(12, 1).message(36, 50).message(1, 200).end_of_session();
    auto file = s.open();
    BinaryFileReader reader(file.get());
    expect_message(reader, 12, 1);
    expect_message(reader, 36, 50);
    expect_message(reader, 1, 200);
    std::span<const std::byte> payload;
    EXPECT_EQ(reader.next(payload), Status::EndOfSession);
    EXPECT_EQ(reader.bytes_consumed(), (2U + 12) + (2 + 36) + (2 + 1) + 2);
}

// The spec says a file without the zero-length end message is incomplete. The reader
// reports that as EndOfFile instead of EndOfSession, so the caller can tell.
TEST(BinaryFileReaderTest, InputWithoutEndMarkerEndsWithEndOfFile) {
    Stream s;
    s.message(5, 0);
    auto file = s.open();
    BinaryFileReader reader(file.get());
    expect_message(reader, 5, 0);
    std::span<const std::byte> payload;
    EXPECT_EQ(reader.next(payload), Status::EndOfFile);
    EXPECT_EQ(reader.next(payload), Status::EndOfFile);  // and stays there
}

TEST(BinaryFileReaderTest, EmptyInputIsEndOfFile) {
    const Stream s;
    auto file = s.open();
    BinaryFileReader reader(file.get());
    std::span<const std::byte> payload;
    EXPECT_EQ(reader.next(payload), Status::EndOfFile);
}

TEST(BinaryFileReaderTest, InputEndingInsideAPayloadIsTruncated) {
    Stream s;
    s.message(4, 0).raw({0, 10, 1, 2, 3});  // promises 10 bytes, delivers 3
    auto file = s.open();
    BinaryFileReader reader(file.get());
    expect_message(reader, 4, 0);
    std::span<const std::byte> payload;
    EXPECT_EQ(reader.next(payload), Status::Truncated);
}

TEST(BinaryFileReaderTest, InputEndingInsideALengthPrefixIsTruncated) {
    Stream s;
    s.message(4, 0).raw({0});  // half of a length prefix
    auto file = s.open();
    BinaryFileReader reader(file.get());
    expect_message(reader, 4, 0);
    std::span<const std::byte> payload;
    EXPECT_EQ(reader.next(payload), Status::Truncated);
}

// With the smallest allowed buffer, thousands of messages of varying size cross the
// buffer's end at every possible position, including inside a length prefix.
TEST(BinaryFileReaderTest, MessagesSplitAcrossBufferRefillsAreReassembled) {
    Stream s;
    std::vector<std::size_t> lengths;
    for (std::size_t i = 0; i < 5'000; ++i) {
        const std::size_t length = 1 + (i * 37) % 200;
        lengths.push_back(length);
        s.message(length, static_cast<std::uint8_t>(i));
    }
    s.end_of_session();
    auto file = s.open();
    BinaryFileReader reader(file.get(), 1);  // clamped up to kMinBuffer
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        expect_message(reader, lengths[i], static_cast<std::uint8_t>(i));
    }
    std::span<const std::byte> payload;
    EXPECT_EQ(reader.next(payload), Status::EndOfSession);
}

// The largest message the format allows (65,535 bytes) fits even in the smallest buffer.
TEST(BinaryFileReaderTest, LargestPossibleMessage) {
    Stream s;
    s.message(10, 0).message(65'535, 3).message(10, 9);
    auto file = s.open();
    BinaryFileReader reader(file.get(), 1);
    expect_message(reader, 10, 0);
    expect_message(reader, 65'535, 3);
    expect_message(reader, 10, 9);
}

}  // namespace
}  // namespace litbook::itch
