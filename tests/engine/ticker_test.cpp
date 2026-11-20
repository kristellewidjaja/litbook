#include "litbook/engine/ticker.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>

#include "support/unwrap.hpp"

namespace litbook {
namespace {

using testing::unwrap;

// Parses exactly 8 wire bytes given as text. A literal such as "MSFT\0\0\0\0" has embedded
// nulls, so callers pass the length explicitly through string_view's constructor.
std::optional<Ticker> parse(std::string_view text) {
    EXPECT_EQ(text.size(), Ticker::kLength);
    std::array<char, Ticker::kLength> bytes{};
    std::copy_n(text.begin(), std::min(text.size(), bytes.size()), bytes.begin());
    return parse_ticker(bytes);
}

TEST(TickerTest, MakeTickerPadsWithSpaces) {
    const Ticker t = unwrap(make_ticker("AAPL"));
    const std::array<char, 8> expected{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    EXPECT_EQ(t.bytes(), expected);
    EXPECT_EQ(t.name(), "AAPL");
}

TEST(TickerTest, EightCharacterNameHasNoPadding) {
    EXPECT_EQ(unwrap(make_ticker("ABCDEFGH")).name(), "ABCDEFGH");
}

TEST(TickerTest, PunctuationAndLowercaseAreAllowed) {
    // Real symbols include names such as "BRK.A" and test symbols such as "ZVZZT".
    EXPECT_TRUE(make_ticker("BRK.A").has_value());
    EXPECT_TRUE(make_ticker("abc").has_value());
}

TEST(TickerTest, MakeTickerRejectsBadNames) {
    EXPECT_FALSE(make_ticker("").has_value());
    EXPECT_FALSE(make_ticker("ABCDEFGHI").has_value());
    EXPECT_FALSE(make_ticker("A B").has_value());
    EXPECT_FALSE(make_ticker(" AB").has_value());
    EXPECT_FALSE(make_ticker("AB\t").has_value());
    EXPECT_FALSE(make_ticker(std::string_view("A\0B", 3)).has_value());
    EXPECT_FALSE(make_ticker("A\x7f").has_value());
}

TEST(TickerTest, ParseAcceptsWireForm) {
    EXPECT_EQ(unwrap(parse("MSFT    ")), unwrap(make_ticker("MSFT")));
    EXPECT_TRUE(parse("ABCDEFGH").has_value());
}

TEST(TickerTest, ParseRejectsMalformedWireBytes) {
    EXPECT_FALSE(parse("        ").has_value());  // All padding means no name.
    EXPECT_FALSE(parse(" MSFT   ").has_value());  // Not left-justified.
    EXPECT_FALSE(parse("MS FT   ").has_value());  // Space inside the name.
    EXPECT_FALSE(parse("MSFT   X").has_value());  // Text after the padding.
    EXPECT_FALSE(
        parse(std::string_view("MSFT\0\0\0\0", 8)).has_value());  // Null padding instead of spaces.
}

TEST(TickerTest, KeysMatchEqualityAndSortAlphabetically) {
    const Ticker a = unwrap(make_ticker("AAPL"));
    const Ticker a2 = unwrap(make_ticker("AAPL"));
    const Ticker b = unwrap(make_ticker("AMZN"));
    const Ticker shorter = unwrap(make_ticker("AA"));
    EXPECT_EQ(a.key(), a2.key());
    EXPECT_NE(a.key(), b.key());
    EXPECT_LT(a.key(), b.key());
    // A space sorts below every name character, so a prefix sorts first.
    EXPECT_LT(shorter.key(), a.key());
}

TEST(TickerTest, WorksAtCompileTime) {
    static_assert(make_ticker("IBM").has_value());
    static_assert(!make_ticker("").has_value());
    static_assert(make_ticker("A")->key() == 0x4120202020202020ULL);
}

}  // namespace
}  // namespace litbook
