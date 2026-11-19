#include "litbook/itch/binary_file_reader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>

namespace litbook::itch {

BinaryFileReader::BinaryFileReader(std::FILE* in, std::size_t buffer_size)
    : in_(in), buffer_(std::max(buffer_size, kMinBuffer)) {}

bool BinaryFileReader::ensure(std::size_t want) {
    while (end_ - begin_ < want) {
        if (eof_) {
            return false;
        }
        // Move the unread tail to the front, then fill the rest of the buffer.
        if (begin_ > 0) {
            std::memmove(buffer_.data(), buffer_.data() + begin_, end_ - begin_);
            end_ -= begin_;
            begin_ = 0;
        }
        const std::size_t got = std::fread(buffer_.data() + end_, 1, buffer_.size() - end_, in_);
        end_ += got;
        if (got == 0) {
            eof_ = true;
        }
    }
    return true;
}

BinaryFileReader::Status BinaryFileReader::next(std::span<const std::byte>& payload) {
    if (!ensure(2)) {
        return begin_ == end_ ? Status::EndOfFile : Status::Truncated;
    }
    const std::size_t length = (std::to_integer<std::size_t>(buffer_[begin_]) << 8U) |
                               std::to_integer<std::size_t>(buffer_[begin_ + 1]);
    if (length == 0) {
        begin_ += 2;
        consumed_ += 2;
        return Status::EndOfSession;
    }
    if (!ensure(2 + length)) {
        return Status::Truncated;
    }
    payload = std::span<const std::byte>(buffer_).subspan(begin_ + 2, length);
    begin_ += 2 + length;
    consumed_ += 2 + length;
    return Status::Message;
}

}  // namespace litbook::itch
