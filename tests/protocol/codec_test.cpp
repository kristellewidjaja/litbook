#include "litbook/protocol/codec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <sstream>
#include <variant>
#include <vector>

#include "protocol/frame_builder.hpp"
#include "support/unwrap.hpp"

namespace litbook::protocol {
namespace {

using testing::FrameBuilder;
using Bytes = std::vector<std::byte>;

Ticker sym(const char* name) {
    return litbook::testing::unwrap(make_ticker(name));
}

// One sample of every message, with the frame it must encode to. The frames are written
// out field by field from the tables in docs/PROTOCOL.md.

const EnterOrder kEnter{.user_ref = 7,
                        .side = Side::Buy,
                        .qty = 300,
                        .symbol = sym("AAPL"),
                        .price = 1'234'500,  // $123.4500
                        .tif = TimeInForce::GoodTillCancel,
                        .type = OrderType::Limit,
                        .post_only = true};

Bytes enter_frame() {
    return FrameBuilder()
        .ch('O')
        .u32(7)
        .ch('B')
        .u32(300)
        .text("AAPL    ")
        .u64(1'234'500)
        .ch('0')
        .ch('L')
        .ch('Y')
        .build();
}

const ReplaceOrder kReplace{.orig_user_ref = 7, .user_ref = 8, .qty = 500, .price = 1'230'000};

Bytes replace_frame() {
    return FrameBuilder().ch('U').u32(7).u32(8).u32(500).u64(1'230'000).build();
}

const CancelOrder kCancel{.user_ref = 8, .qty = 0};

Bytes cancel_frame() {
    return FrameBuilder().ch('X').u32(8).u32(0).build();
}

const AcceptedMessage kAccepted{.timestamp = 34'200'000'000'123,  // 09:30 and 123 ns
                                .user_ref = 7,
                                .side = Side::Sell,
                                .qty = 300,
                                .symbol = sym("MSFT"),
                                .price = 0,
                                .tif = TimeInForce::ImmediateOrCancel,
                                .type = OrderType::Market,
                                .post_only = false,
                                .order_ref = 0x0102030405060708ULL};

Bytes accepted_frame() {
    return FrameBuilder()
        .ch('A')
        .u64(34'200'000'000'123)
        .u32(7)
        .ch('S')
        .u32(300)
        .text("MSFT    ")
        .u64(0)
        .ch('3')
        .ch('M')
        .ch('N')
        .u64(0x0102030405060708ULL)
        .build();
}

const ReplacedMessage kReplaced{.timestamp = 5,
                                .orig_user_ref = 7,
                                .user_ref = 8,
                                .side = Side::Buy,
                                .qty = 200,
                                .symbol = sym("BRK.A"),
                                .price = kMaxWirePrice,
                                .order_ref = 99,
                                .kept_priority = true};

Bytes replaced_frame() {
    return FrameBuilder()
        .ch('U')
        .u64(5)
        .u32(7)
        .u32(8)
        .ch('B')
        .u32(200)
        .text("BRK.A   ")
        .u64(1'999'999'900)
        .u64(99)
        .ch('Y')
        .build();
}

const CanceledMessage kCanceled{
    .timestamp = 6, .user_ref = 8, .qty = 150, .reason = WireCancelReason::SelfMatchPrevention};

Bytes canceled_frame() {
    return FrameBuilder().ch('C').u64(6).u32(8).u32(150).ch('Q').build();
}

const ExecutedMessage kExecuted{.timestamp = 7,
                                .user_ref = 8,
                                .qty = 100,
                                .price = 1'230'000,
                                .liquidity = Liquidity::Removed,
                                .match = 42};

Bytes executed_frame() {
    return FrameBuilder().ch('E').u64(7).u32(8).u32(100).u64(1'230'000).ch('R').u64(42).build();
}

const RejectedMessage kRejected{
    .timestamp = 8, .user_ref = 9, .reason = WireRejectReason::InvalidSymbol};

Bytes rejected_frame() {
    return FrameBuilder().ch('J').u64(8).u32(9).u16(0x0017).build();
}

template <class Message>
Bytes encoded(const Message& m) {
    std::array<std::byte, kMaxMessageLength> buffer{};
    const std::size_t n = encode(m, buffer);
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n)};
}

std::vector<Bytes> all_inbound_frames() {
    return {enter_frame(), replace_frame(), cancel_frame()};
}

std::vector<Bytes> all_outbound_frames() {
    return {accepted_frame(), replaced_frame(), canceled_frame(), executed_frame(),
            rejected_frame()};
}

// Golden frames. Each message encodes to exactly the bytes in the protocol tables.

TEST(CodecTest, EncodesEveryMessageToTheDocumentedBytes) {
    EXPECT_EQ(encoded(kEnter), enter_frame());
    EXPECT_EQ(encoded(kReplace), replace_frame());
    EXPECT_EQ(encoded(kCancel), cancel_frame());
    EXPECT_EQ(encoded(kAccepted), accepted_frame());
    EXPECT_EQ(encoded(kReplaced), replaced_frame());
    EXPECT_EQ(encoded(kCanceled), canceled_frame());
    EXPECT_EQ(encoded(kExecuted), executed_frame());
    EXPECT_EQ(encoded(kRejected), rejected_frame());
}

TEST(CodecTest, DocumentedLengthsMatchTheFrames) {
    EXPECT_EQ(enter_frame().size(), kEnterOrderLength);
    EXPECT_EQ(replace_frame().size(), kReplaceOrderLength);
    EXPECT_EQ(cancel_frame().size(), kCancelOrderLength);
    EXPECT_EQ(accepted_frame().size(), kAcceptedLength);
    EXPECT_EQ(replaced_frame().size(), kReplacedLength);
    EXPECT_EQ(canceled_frame().size(), kCanceledLength);
    EXPECT_EQ(executed_frame().size(), kExecutedLength);
    EXPECT_EQ(rejected_frame().size(), kRejectedLength);
}

TEST(CodecTest, DecodesEveryGoldenFrame) {
    auto in = [](const Bytes& f) { return decode_inbound(f); };
    auto out = [](const Bytes& f) { return decode_outbound(f); };
    EXPECT_EQ(in(enter_frame()).message, InboundMessage{kEnter});
    EXPECT_EQ(in(replace_frame()).message, InboundMessage{kReplace});
    EXPECT_EQ(in(cancel_frame()).message, InboundMessage{kCancel});
    EXPECT_EQ(out(accepted_frame()).message, OutboundMessage{kAccepted});
    EXPECT_EQ(out(replaced_frame()).message, OutboundMessage{kReplaced});
    EXPECT_EQ(out(canceled_frame()).message, OutboundMessage{kCanceled});
    EXPECT_EQ(out(executed_frame()).message, OutboundMessage{kExecuted});
    EXPECT_EQ(out(rejected_frame()).message, OutboundMessage{kRejected});
    EXPECT_EQ(in(enter_frame()).status, DecodeStatus::Ok);
    EXPECT_EQ(out(rejected_frame()).status, DecodeStatus::Ok);
}

TEST(CodecTest, VariantEncodeMatchesTheDirectOverload) {
    EXPECT_EQ(encoded(InboundMessage{kReplace}), replace_frame());
    EXPECT_EQ(encoded(OutboundMessage{kExecuted}), executed_frame());
}

// Round trips over many field values, including the extremes of every integer field.

TEST(CodecTest, RoundTripsExtremeValues) {
    for (const std::uint32_t u32 : {0U, 1U, 0x7FFFFFFFU, 0xFFFFFFFFU}) {
        for (const Price price : {Price{0}, Price{1}, kMaxWirePrice}) {
            for (const std::uint64_t u64 : {std::uint64_t{0}, ~std::uint64_t{0}}) {
                const EnterOrder e{.user_ref = u32,
                                   .side = Side::Sell,
                                   .qty = u32,
                                   .symbol = sym("ABCDEFGH"),
                                   .price = price,
                                   .tif = TimeInForce::FillOrKill,
                                   .type = OrderType::Limit,
                                   .post_only = false};
                EXPECT_EQ(decode_inbound(encoded(e)).message, InboundMessage{e});
                const ExecutedMessage x{.timestamp = u64,
                                        .user_ref = u32,
                                        .qty = u32,
                                        .price = price,
                                        .liquidity = Liquidity::Added,
                                        .match = u64};
                EXPECT_EQ(decode_outbound(encoded(x)).message, OutboundMessage{x});
                const AcceptedMessage a{.timestamp = u64,
                                        .user_ref = u32,
                                        .side = Side::Buy,
                                        .qty = u32,
                                        .symbol = sym("Z"),
                                        .price = price,
                                        .tif = TimeInForce::GoodTillCancel,
                                        .type = OrderType::Limit,
                                        .post_only = true,
                                        .order_ref = u64};
                EXPECT_EQ(decode_outbound(encoded(a)).message, OutboundMessage{a});
            }
        }
    }
}

TEST(CodecTest, RoundTripsEveryEnumValue) {
    for (const TimeInForce tif :
         {TimeInForce::GoodTillCancel, TimeInForce::ImmediateOrCancel, TimeInForce::FillOrKill}) {
        for (const OrderType type : {OrderType::Limit, OrderType::Market}) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                EnterOrder e = kEnter;
                e.tif = tif;
                e.type = type;
                e.side = side;
                EXPECT_EQ(decode_inbound(encoded(e)).message, InboundMessage{e});
            }
        }
    }
    for (const WireCancelReason r :
         {WireCancelReason::UserRequested, WireCancelReason::ImmediateOrCancel,
          WireCancelReason::SelfMatchPrevention, WireCancelReason::System}) {
        CanceledMessage c = kCanceled;
        c.reason = r;
        EXPECT_EQ(decode_outbound(encoded(c)).message, OutboundMessage{c});
    }
    for (const WireRejectReason r :
         {WireRejectReason::Other, WireRejectReason::InvalidQuantity,
          WireRejectReason::InvalidSymbol, WireRejectReason::InvalidPrice,
          WireRejectReason::MaxQuantityExceeded, WireRejectReason::PostOnlyWouldCross,
          WireRejectReason::InvalidOrderType, WireRejectReason::MaxOpenOrdersExceeded}) {
        RejectedMessage j = kRejected;
        j.reason = r;
        EXPECT_EQ(decode_outbound(encoded(j)).message, OutboundMessage{j});
    }
}

// Malformed frames. Every one of these must be refused with the right status and no
// message, and none of them may crash or read past the frame (ASan checks that).

void expect_inbound(const Bytes& f, DecodeStatus status) {
    const auto d = decode_inbound(f);
    EXPECT_EQ(d.status, status);
    EXPECT_FALSE(d.message.has_value());
}

void expect_outbound(const Bytes& f, DecodeStatus status) {
    const auto d = decode_outbound(f);
    EXPECT_EQ(d.status, status);
    EXPECT_FALSE(d.message.has_value());
}

TEST(CodecMalformedTest, EmptyFrame) {
    expect_inbound({}, DecodeStatus::Empty);
    expect_outbound({}, DecodeStatus::Empty);
}

TEST(CodecMalformedTest, EveryUnknownTypeByte) {
    const std::set<char> inbound{'O', 'U', 'X'};
    const std::set<char> outbound{'A', 'U', 'C', 'E', 'J'};
    for (int b = 0; b < 256; ++b) {
        const auto c = static_cast<char>(b);
        // A long frame, so only the type byte can be the problem.
        Bytes f(kMaxMessageLength + 1, std::byte{0});
        f[0] = static_cast<std::byte>(b);
        if (!inbound.contains(c)) {
            expect_inbound(f, DecodeStatus::UnknownType);
            EXPECT_EQ(inbound_length(f[0]), 0U);
        }
        if (!outbound.contains(c)) {
            expect_outbound(f, DecodeStatus::UnknownType);
            EXPECT_EQ(outbound_length(f[0]), 0U);
        }
    }
}

TEST(CodecMalformedTest, DirectionMatters) {
    // An outbound Accepted frame sent to the exchange is not a message it knows, and an
    // inbound Enter Order frame is not something a client should receive.
    expect_inbound(accepted_frame(), DecodeStatus::UnknownType);
    expect_outbound(enter_frame(), DecodeStatus::UnknownType);
    // 'U' exists in both directions with different lengths.
    expect_inbound(replaced_frame(), DecodeStatus::TooLong);
    expect_outbound(replace_frame(), DecodeStatus::TooShort);
}

TEST(CodecMalformedTest, EveryTruncationIsTooShort) {
    for (const Bytes& full : all_inbound_frames()) {
        for (std::size_t n = 1; n < full.size(); ++n) {
            SCOPED_TRACE(n);
            expect_inbound(Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n)),
                           DecodeStatus::TooShort);
        }
    }
    for (const Bytes& full : all_outbound_frames()) {
        for (std::size_t n = 1; n < full.size(); ++n) {
            SCOPED_TRACE(n);
            expect_outbound(Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n)),
                            DecodeStatus::TooShort);
        }
    }
}

TEST(CodecMalformedTest, TrailingBytesAreTooLong) {
    for (Bytes f : all_inbound_frames()) {
        f.push_back(std::byte{0});
        expect_inbound(f, DecodeStatus::TooLong);
    }
    for (Bytes f : all_outbound_frames()) {
        f.push_back(std::byte{' '});
        expect_outbound(f, DecodeStatus::TooLong);
    }
}

Bytes with_byte(Bytes f, std::size_t offset, char c) {
    f.at(offset) = static_cast<std::byte>(c);
    return f;
}

Bytes with_u64(Bytes f, std::size_t offset, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        f.at(offset + static_cast<std::size_t>(i)) = static_cast<std::byte>(v & 0xFFU);
        v >>= 8U;
    }
    return f;
}

Bytes with_text(Bytes f, std::size_t offset, std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        f.at(offset + i) = static_cast<std::byte>(s[i]);
    }
    return f;
}

// Offsets below are the documented ones from docs/PROTOCOL.md.

TEST(CodecMalformedTest, EnterOrderBadFields) {
    const Bytes f = enter_frame();
    for (const char c : {'b', 'T', 'E', ' ', '\0'}) {  // T and E are OUCH short sales.
        expect_inbound(with_byte(f, 5, c), DecodeStatus::BadSide);
    }
    for (const char c : {'1', '2', '5', '6', 'E', ' '}) {  // Real OUCH codes litbook lacks.
        expect_inbound(with_byte(f, 26, c), DecodeStatus::BadTimeInForce);
    }
    for (const char c : {'l', 'P', ' '}) {
        expect_inbound(with_byte(f, 27, c), DecodeStatus::BadOrderType);
    }
    for (const char c : {'y', '1', ' '}) {
        expect_inbound(with_byte(f, 28, c), DecodeStatus::BadFlag);
    }
    expect_inbound(with_u64(f, 18, 1'999'999'901), DecodeStatus::BadPrice);
    expect_inbound(with_u64(f, 18, 0x7FFFFFFF), DecodeStatus::BadPrice);  // OUCH cross price.
    expect_inbound(with_u64(f, 18, ~std::uint64_t{0}), DecodeStatus::BadPrice);
    EXPECT_EQ(decode_inbound(with_u64(f, 18, 1'999'999'900)).status, DecodeStatus::Ok);
}

TEST(CodecMalformedTest, BadSymbols) {
    const Bytes f = enter_frame();
    for (const char* s : {"        ", " AAPL   ", "AA PL   ", "AAPL   X", "AAPL\t   "}) {
        SCOPED_TRACE(s);
        expect_inbound(with_text(f, 10, s), DecodeStatus::BadSymbol);
    }
    Bytes nul = f;
    for (std::size_t i = 14; i < 18; ++i) {
        nul[i] = std::byte{0};  // Null padding instead of spaces.
    }
    expect_inbound(nul, DecodeStatus::BadSymbol);
    expect_outbound(with_text(accepted_frame(), 18, "        "), DecodeStatus::BadSymbol);
    expect_outbound(with_text(replaced_frame(), 22, " X      "), DecodeStatus::BadSymbol);
}

TEST(CodecMalformedTest, FirstBadFieldIsReported) {
    // Side comes before symbol on the wire, so a frame with both wrong reports the side.
    const Bytes f = with_text(with_byte(enter_frame(), 5, 'Q'), 10, "        ");
    expect_inbound(f, DecodeStatus::BadSide);
}

TEST(CodecMalformedTest, ReplaceAndOutboundBadFields) {
    expect_inbound(with_u64(replace_frame(), 13, 2'000'000'000), DecodeStatus::BadPrice);
    expect_outbound(with_byte(accepted_frame(), 13, 'X'), DecodeStatus::BadSide);
    expect_outbound(with_u64(accepted_frame(), 26, 2'000'000'000), DecodeStatus::BadPrice);
    expect_outbound(with_byte(accepted_frame(), 34, '9'), DecodeStatus::BadTimeInForce);
    expect_outbound(with_byte(accepted_frame(), 35, 'Q'), DecodeStatus::BadOrderType);
    expect_outbound(with_byte(accepted_frame(), 36, 'n'), DecodeStatus::BadFlag);
    expect_outbound(with_byte(replaced_frame(), 17, 's'), DecodeStatus::BadSide);
    expect_outbound(with_u64(replaced_frame(), 30, 2'000'000'000), DecodeStatus::BadPrice);
    expect_outbound(with_byte(replaced_frame(), 46, '?'), DecodeStatus::BadFlag);
    expect_outbound(with_u64(executed_frame(), 17, 2'000'000'000), DecodeStatus::BadPrice);
}

TEST(CodecMalformedTest, OutboundCodesAreOpenSets) {
    // A client must keep working when the exchange adds a reason code or a liquidity flag,
    // so these decode and re-encode unchanged.
    const Bytes c = with_byte(canceled_frame(), 17, 'T');
    ASSERT_EQ(decode_outbound(c).status, DecodeStatus::Ok);
    EXPECT_EQ(
        encoded(std::get<CanceledMessage>(litbook::testing::unwrap(decode_outbound(c).message))),
        c);
    const Bytes x = with_byte(executed_frame(), 25, 'k');
    ASSERT_EQ(decode_outbound(x).status, DecodeStatus::Ok);
    EXPECT_EQ(
        encoded(std::get<ExecutedMessage>(litbook::testing::unwrap(decode_outbound(x).message))),
        x);
    const Bytes j = FrameBuilder().ch('J').u64(1).u32(2).u16(0xABCD).build();
    ASSERT_EQ(decode_outbound(j).status, DecodeStatus::Ok);
    EXPECT_EQ(
        encoded(std::get<RejectedMessage>(litbook::testing::unwrap(decode_outbound(j).message))),
        j);
}

TEST(CodecMalformedTest, TradingRuleViolationsStillDecode) {
    // A zero quantity or a market order with a price is a well-formed frame. Refusing it
    // is a trading decision for the order entry layer, which answers with Rejected.
    EnterOrder zero = kEnter;
    zero.qty = 0;
    EXPECT_EQ(decode_inbound(encoded(zero)).status, DecodeStatus::Ok);
    EnterOrder priced_market = kEnter;
    priced_market.type = OrderType::Market;
    EXPECT_EQ(decode_inbound(encoded(priced_market)).status, DecodeStatus::Ok);
}

// Encoder failures.

TEST(CodecEncodeTest, TooSmallBufferWritesNothing) {
    const std::vector<std::pair<OutboundMessage, std::size_t>> out{{kAccepted, kAcceptedLength},
                                                                   {kReplaced, kReplacedLength},
                                                                   {kCanceled, kCanceledLength},
                                                                   {kExecuted, kExecutedLength},
                                                                   {kRejected, kRejectedLength}};
    for (const auto& [m, length] : out) {
        for (std::size_t n = 0; n < length; ++n) {
            Bytes buffer(n, std::byte{0xAA});
            EXPECT_EQ(encode(m, buffer), 0U);
            EXPECT_EQ(buffer, Bytes(n, std::byte{0xAA}));
        }
    }
    const std::vector<std::pair<InboundMessage, std::size_t>> in{{kEnter, kEnterOrderLength},
                                                                 {kReplace, kReplaceOrderLength},
                                                                 {kCancel, kCancelOrderLength}};
    for (const auto& [m, length] : in) {
        for (std::size_t n = 0; n < length; ++n) {
            Bytes buffer(n, std::byte{0xAA});
            EXPECT_EQ(encode(m, buffer), 0U);
            EXPECT_EQ(buffer, Bytes(n, std::byte{0xAA}));
        }
    }
}

TEST(CodecEncodeTest, LargerBufferUsesOnlyTheFront) {
    Bytes buffer(64, std::byte{0xAA});
    ASSERT_EQ(encode(kCancel, buffer), kCancelOrderLength);
    EXPECT_EQ(Bytes(buffer.begin(), buffer.begin() + kCancelOrderLength), cancel_frame());
    EXPECT_EQ(buffer[kCancelOrderLength], std::byte{0xAA});
}

TEST(CodecEncodeTest, PriceOutsideTheWireRangeIsRefused) {
    Bytes buffer(kMaxMessageLength, std::byte{0xAA});
    for (const Price bad : {Price{-1}, kMaxWirePrice + 1}) {
        EnterOrder e = kEnter;
        e.price = bad;
        EXPECT_EQ(encode(e, buffer), 0U);
        ReplaceOrder r = kReplace;
        r.price = bad;
        EXPECT_EQ(encode(r, buffer), 0U);
        AcceptedMessage a = kAccepted;
        a.price = bad;
        EXPECT_EQ(encode(a, buffer), 0U);
        ReplacedMessage u = kReplaced;
        u.price = bad;
        EXPECT_EQ(encode(u, buffer), 0U);
        ExecutedMessage x = kExecuted;
        x.price = bad;
        EXPECT_EQ(encode(x, buffer), 0U);
    }
    EXPECT_EQ(buffer, Bytes(kMaxMessageLength, std::byte{0xAA}));
}

TEST(CodecTest, LengthLookup) {
    EXPECT_EQ(inbound_length(std::byte{'O'}), kEnterOrderLength);
    EXPECT_EQ(inbound_length(std::byte{'U'}), kReplaceOrderLength);
    EXPECT_EQ(inbound_length(std::byte{'X'}), kCancelOrderLength);
    EXPECT_EQ(outbound_length(std::byte{'U'}), kReplacedLength);
    EXPECT_EQ(outbound_length(std::byte{'J'}), kRejectedLength);
}

TEST(CodecTest, StatusPrints) {
    std::ostringstream os;
    os << DecodeStatus::TooShort << ' ' << DecodeStatus::BadSymbol;
    EXPECT_EQ(os.str(), "TooShort BadSymbol");
}

}  // namespace
}  // namespace litbook::protocol
