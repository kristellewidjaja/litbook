#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace litbook::itch {

// Splits a Nasdaq BinaryFILE stream into message payloads. In that format every message
// is a 2-byte big-endian length followed by that many bytes of payload, and a message
// of length 0 marks the end of the session ("BinaryFILE", version 1.00, section 1).
//
// The reader works on any std::FILE*, so it can read a decompressed stream from a pipe
// (gunzip -c day.gz | tool) and the uncompressed day never has to be stored on disk.
// It reads in large chunks into one buffer that is allocated once in the constructor,
// and next() returns a view into that buffer, so reading never allocates. A message
// that is split across two chunks is handled by moving the unread bytes to the front
// of the buffer before reading more.
//
// The buffer always holds at least one message of the largest possible size
// (2 + 65,535 bytes), so any valid message fits. The span returned by next() stays valid
// only until the next call to next().
class BinaryFileReader {
public:
    enum class Status : std::uint8_t {
        Message,       // payload holds the next message
        EndOfSession,  // the zero-length message that ends the session was read
        EndOfFile,     // the input ended cleanly between messages, with no end marker
        Truncated,     // the input ended in the middle of a message
    };

    static constexpr std::size_t kMinBuffer = 2 + 65'535;

    explicit BinaryFileReader(std::FILE* in, std::size_t buffer_size = std::size_t{1} << 20);

    Status next(std::span<const std::byte>& payload);

    // Total bytes consumed so far, length prefixes included.
    [[nodiscard]] std::uint64_t bytes_consumed() const noexcept { return consumed_; }

private:
    // Makes at least `want` unread bytes available, reading more input if needed.
    // Returns false if the input ends first.
    bool ensure(std::size_t want);

    std::FILE* in_;
    std::vector<std::byte> buffer_;
    std::size_t begin_ = 0;  // first unread byte
    std::size_t end_ = 0;    // one past the last byte read from the input
    std::uint64_t consumed_ = 0;
    bool eof_ = false;
};

}  // namespace litbook::itch
