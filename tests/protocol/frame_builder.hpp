#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace litbook::protocol::testing {

// Builds a byte frame field by field for tests. It writes big-endian on its own, without
// using byte_order.hpp, so the golden frames in the tests do not depend on the code they
// check.
class FrameBuilder {
public:
    FrameBuilder& ch(char c) {
        bytes_.push_back(static_cast<std::byte>(c));
        return *this;
    }
    FrameBuilder& text(std::string_view s) {
        for (const char c : s) {
            ch(c);
        }
        return *this;
    }
    FrameBuilder& u16(std::uint16_t v) { return big_endian(v, 2); }
    FrameBuilder& u32(std::uint32_t v) { return big_endian(v, 4); }
    FrameBuilder& u64(std::uint64_t v) { return big_endian(v, 8); }

    [[nodiscard]] std::vector<std::byte> build() const { return bytes_; }

private:
    FrameBuilder& big_endian(std::uint64_t v, int width) {
        for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) {
            bytes_.push_back(static_cast<std::byte>((v >> static_cast<unsigned>(shift)) & 0xFFU));
        }
        return *this;
    }

    std::vector<std::byte> bytes_;
};

}  // namespace litbook::protocol::testing
