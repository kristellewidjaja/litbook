#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace litbook {

// A stock symbol in the fixed 8-byte form that ITCH and OUCH both use. The name is
// left-justified and padded on the right with spaces, so "AAPL" is stored as "AAPL    ".
//
// The invariant is that a Ticker always holds 1 to 8 printable, non-space ASCII
// characters followed only by spaces. The only ways to build one are make_ticker and
// parse_ticker, which both check this, so code that receives a Ticker never has to
// check it again.
class Ticker {
public:
    static constexpr std::size_t kLength = 8;

    // Returns the ticker for `name`, or nothing if `name` is empty, longer than 8
    // characters, or contains a space or a non-printable character.
    friend constexpr std::optional<Ticker> make_ticker(std::string_view name) noexcept;

    // Returns the ticker stored in 8 raw wire bytes, or nothing if the bytes are not
    // left-justified and space-padded as described above.
    friend constexpr std::optional<Ticker> parse_ticker(
        std::span<const char, kLength> bytes) noexcept;

    // The 8 bytes packed into one integer, first character in the highest byte. Two
    // tickers are equal exactly when their keys are equal, so the key can be stored in a
    // hash map, and comparing keys as numbers sorts tickers alphabetically.
    [[nodiscard]] constexpr std::uint64_t key() const noexcept {
        std::uint64_t k = 0;
        for (const char c : chars_) {
            k = (k << 8U) | static_cast<unsigned char>(c);
        }
        return k;
    }

    // The 8 bytes exactly as they go on the wire, padding included.
    [[nodiscard]] constexpr const std::array<char, kLength>& bytes() const noexcept {
        return chars_;
    }

    // The name without its padding, for printing and error messages.
    [[nodiscard]] std::string name() const {
        std::string out(chars_.begin(), chars_.end());
        out.erase(out.find_last_not_of(' ') + 1);
        return out;
    }

    constexpr bool operator==(const Ticker&) const = default;

private:
    constexpr Ticker() = default;

    static constexpr bool is_name_char(char c) noexcept { return c > ' ' && c <= '~'; }

    std::array<char, kLength> chars_{};
};

constexpr std::optional<Ticker> make_ticker(std::string_view name) noexcept {
    if (name.empty() || name.size() > Ticker::kLength) {
        return std::nullopt;
    }
    Ticker t;
    t.chars_.fill(' ');
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (!Ticker::is_name_char(name[i])) {
            return std::nullopt;
        }
        t.chars_[i] = name[i];
    }
    return t;
}

constexpr std::optional<Ticker> parse_ticker(
    std::span<const char, Ticker::kLength> bytes) noexcept {
    // The name ends at the first space. Everything after it must be padding.
    std::size_t length = 0;
    while (length < Ticker::kLength && bytes[length] != ' ') {
        ++length;
    }
    for (std::size_t i = length; i < Ticker::kLength; ++i) {
        if (bytes[i] != ' ') {
            return std::nullopt;
        }
    }
    return make_ticker(std::string_view(bytes.data(), length));
}

}  // namespace litbook
