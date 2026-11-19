// Tests for the ITCH 5.0 message decoder. Each message is built byte by byte at the
// offsets given in the specification's field tables, with a different value in every
// field, so a field read from the wrong offset or with the wrong width shows up.

#include "litbook/itch/itch_parser.hpp"

#include "litbook/itch/itch_messages.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace litbook::itch {
namespace {

// Writes fields into a message buffer at fixed offsets, big-endian like the wire format.
class Bytes {
public:
    explicit Bytes(std::size_t size) : bytes_(size, std::byte{0}) {}

    Bytes& uint(std::size_t offset, std::size_t width, std::uint64_t value) {
        for (std::size_t i = 0; i < width; ++i) {
            bytes_.at(offset + width - 1 - i) = static_cast<std::byte>(value & 0xFFU);
            value >>= 8U;
        }
        return *this;
    }
    Bytes& text(std::size_t offset, std::string_view s) {
        for (std::size_t i = 0; i < s.size(); ++i) {
            bytes_.at(offset + i) = static_cast<std::byte>(s[i]);
        }
        return *this;
    }
    // The 11-byte header every message starts with.
    Bytes& header(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
        return text(0, std::string_view(&type, 1))
            .uint(1, 2, locate)
            .uint(3, 2, tracking)
            .uint(5, 6, ts);
    }
    [[nodiscard]] std::span<const std::byte> span() const { return bytes_; }
    void resize(std::size_t n) { bytes_.resize(n, std::byte{0}); }

private:
    std::vector<std::byte> bytes_;
};

// Remembers the last message of each kind it was given, and how many calls it got.
struct Recorder {
    int calls = 0;
    SystemEvent system_event{};
    StockDirectory directory{};
    StockTradingAction trading_action{};
    AddOrder add{};
    OrderExecuted executed{};
    OrderExecutedWithPrice executed_with_price{};
    OrderCancel cancel{};
    OrderDelete del{};
    OrderReplace replace{};

    void on_system_event(const SystemEvent& m) { system_event = m, ++calls; }
    void on_stock_directory(const StockDirectory& m) { directory = m, ++calls; }
    void on_stock_trading_action(const StockTradingAction& m) { trading_action = m, ++calls; }
    void on_add_order(const AddOrder& m) { add = m, ++calls; }
    void on_order_executed(const OrderExecuted& m) { executed = m, ++calls; }
    void on_order_executed_with_price(const OrderExecutedWithPrice& m) {
        executed_with_price = m, ++calls;
    }
    void on_order_cancel(const OrderCancel& m) { cancel = m, ++calls; }
    void on_order_delete(const OrderDelete& m) { del = m, ++calls; }
    void on_order_replace(const OrderReplace& m) { replace = m, ++calls; }
};

static_assert(ItchHandler<Recorder>);

// A timestamp that uses all 6 bytes, so a 4-byte or 8-byte read would get it wrong.
constexpr std::uint64_t kTs = 0x0102'0304'0506;

void expect_header(const Header& h, char type, std::uint16_t locate, std::uint16_t tracking) {
    EXPECT_EQ(h.type, type);
    EXPECT_EQ(h.stock_locate, locate);
    EXPECT_EQ(h.tracking_number, tracking);
    EXPECT_EQ(h.timestamp_ns, kTs);
}

TEST(ItchParserTest, SystemEvent) {
    Bytes b(kSystemEventLength);
    b.header('S', 0, 7, kTs).text(11, "Q");
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.system_event.header, 'S', 0, 7);
    EXPECT_EQ(r.system_event.event_code, 'Q');
}

TEST(ItchParserTest, StockDirectory) {
    Bytes b(kStockDirectoryLength);
    b.header('R', 13, 2, kTs).text(11, "AAPL    ").text(19, "Q").uint(21, 4, 100);
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.directory.header, 'R', 13, 2);
    EXPECT_EQ(std::string_view(r.directory.stock.data(), 8), "AAPL    ");
    EXPECT_EQ(r.directory.round_lot_size, 100U);
}

TEST(ItchParserTest, StockTradingAction) {
    Bytes b(kStockTradingActionLength);
    b.header('H', 13, 2, kTs).text(11, "AAPL    ").text(19, "P").text(20, " ").text(21, "LUDP");
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.trading_action.header, 'H', 13, 2);
    EXPECT_EQ(std::string_view(r.trading_action.stock.data(), 8), "AAPL    ");
    EXPECT_EQ(r.trading_action.trading_state, 'P');
    EXPECT_EQ(std::string_view(r.trading_action.reason.data(), 4), "LUDP");
}

TEST(ItchParserTest, AddOrderWithoutMpid) {
    Bytes b(kAddOrderLength);
    b.header('A', 13, 3, kTs)
        .uint(11, 8, 0x1122'3344'5566'7788)
        .text(19, "B")
        .uint(20, 4, 300)
        .text(24, "AAPL    ")
        .uint(32, 4, 2'934'500);  // $293.45
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.add.header, 'A', 13, 3);
    EXPECT_EQ(r.add.order_ref, 0x1122'3344'5566'7788U);
    EXPECT_EQ(r.add.side, 'B');
    EXPECT_EQ(r.add.shares, 300U);
    EXPECT_EQ(std::string_view(r.add.stock.data(), 8), "AAPL    ");
    EXPECT_EQ(r.add.price, 2'934'500U);
    EXPECT_FALSE(r.add.has_mpid);
}

TEST(ItchParserTest, AddOrderWithMpid) {
    Bytes b(kAddOrderMpidLength);
    b.header('F', 9, 4, kTs)
        .uint(11, 8, 42)
        .text(19, "S")
        .uint(20, 4, 0xFFFF'FFFF)  // the largest share count
        .text(24, "MSFT    ")
        .uint(32, 4, 2'000'000'000)  // $200,000.0000, the largest price the spec allows
        .text(36, "GSCO");
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.add.header, 'F', 9, 4);
    EXPECT_EQ(r.add.order_ref, 42U);
    EXPECT_EQ(r.add.side, 'S');
    EXPECT_EQ(r.add.shares, 0xFFFF'FFFFU);
    EXPECT_EQ(r.add.price, 2'000'000'000U);
    EXPECT_TRUE(r.add.has_mpid);
    EXPECT_EQ(std::string_view(r.add.mpid.data(), 4), "GSCO");
}

TEST(ItchParserTest, OrderExecuted) {
    Bytes b(kOrderExecutedLength);
    b.header('E', 5, 6, kTs).uint(11, 8, 77).uint(19, 4, 25).uint(23, 8, 0xAABB'CCDD'EEFF'0011);
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.executed.header, 'E', 5, 6);
    EXPECT_EQ(r.executed.order_ref, 77U);
    EXPECT_EQ(r.executed.executed_shares, 25U);
    EXPECT_EQ(r.executed.match_number, 0xAABB'CCDD'EEFF'0011U);
}

TEST(ItchParserTest, OrderExecutedWithPrice) {
    Bytes b(kOrderExecutedWithPriceLength);
    b.header('C', 5, 6, kTs)
        .uint(11, 8, 78)
        .uint(19, 4, 10)
        .uint(23, 8, 999)
        .text(31, "N")
        .uint(32, 4, 1'000'100);
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.executed_with_price.header, 'C', 5, 6);
    EXPECT_EQ(r.executed_with_price.order_ref, 78U);
    EXPECT_EQ(r.executed_with_price.executed_shares, 10U);
    EXPECT_EQ(r.executed_with_price.match_number, 999U);
    EXPECT_EQ(r.executed_with_price.printable, 'N');
    EXPECT_EQ(r.executed_with_price.execution_price, 1'000'100U);
}

TEST(ItchParserTest, OrderCancel) {
    Bytes b(kOrderCancelLength);
    b.header('X', 1, 2, kTs).uint(11, 8, 79).uint(19, 4, 40);
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.cancel.header, 'X', 1, 2);
    EXPECT_EQ(r.cancel.order_ref, 79U);
    EXPECT_EQ(r.cancel.canceled_shares, 40U);
}

TEST(ItchParserTest, OrderDelete) {
    Bytes b(kOrderDeleteLength);
    b.header('D', 1, 2, kTs).uint(11, 8, 80);
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.del.header, 'D', 1, 2);
    EXPECT_EQ(r.del.order_ref, 80U);
}

TEST(ItchParserTest, OrderReplace) {
    Bytes b(kOrderReplaceLength);
    b.header('U', 1, 2, kTs).uint(11, 8, 81).uint(19, 8, 82).uint(27, 4, 500).uint(31, 4, 123'400);
    Recorder r;
    EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Decoded);
    expect_header(r.replace.header, 'U', 1, 2);
    EXPECT_EQ(r.replace.original_order_ref, 81U);
    EXPECT_EQ(r.replace.new_order_ref, 82U);
    EXPECT_EQ(r.replace.shares, 500U);
    EXPECT_EQ(r.replace.price, 123'400U);
}

// A known type whose payload is one byte too short or too long is rejected and never
// reaches the handler, because its fields cannot be trusted.
TEST(ItchParserTest, WrongLengthIsRejected) {
    const std::array<std::pair<char, std::size_t>, 10> known = {
        {{'S', kSystemEventLength},
         {'R', kStockDirectoryLength},
         {'H', kStockTradingActionLength},
         {'A', kAddOrderLength},
         {'F', kAddOrderMpidLength},
         {'E', kOrderExecutedLength},
         {'C', kOrderExecutedWithPriceLength},
         {'X', kOrderCancelLength},
         {'D', kOrderDeleteLength},
         {'U', kOrderReplaceLength}}};
    for (const auto& [type, length] : known) {
        for (const std::size_t n : {std::size_t{1}, length - 1, length + 1}) {
            Bytes b(n);
            b.text(0, std::string_view(&type, 1));
            Recorder r;
            EXPECT_EQ(parse_message(b.span(), r), ParseStatus::BadLength) << type << " " << n;
            EXPECT_EQ(r.calls, 0) << type;
        }
    }
}

// Message types that do not change the book (trades, crosses, imbalance indicators,
// trading actions and so on) are skipped without being decoded.
TEST(ItchParserTest, OtherTypesAreSkipped) {
    for (const char type : {'P', 'Q', 'B', 'I', 'N', 'Y', 'L', 'V', 'W', 'K', 'J', 'h', 'O'}) {
        Bytes b(20);
        b.text(0, std::string_view(&type, 1));
        Recorder r;
        EXPECT_EQ(parse_message(b.span(), r), ParseStatus::Skipped) << type;
        EXPECT_EQ(r.calls, 0);
    }
}

TEST(ItchParserTest, EmptyPayload) {
    Recorder r;
    EXPECT_EQ(parse_message(std::span<const std::byte>{}, r), ParseStatus::Empty);
    EXPECT_EQ(r.calls, 0);
}

}  // namespace
}  // namespace litbook::itch
