#include "litbook/protocol/codec.hpp"

#include "litbook/protocol/byte_order.hpp"

#include <algorithm>
#include <array>
#include <ostream>
#include <type_traits>

namespace litbook::protocol {

namespace {

// Field offsets of every message. docs/PROTOCOL.md has the same tables. Each block ends
// with a static_assert that the last field ends exactly at the message length, so a typo
// in an offset or a length cannot slip through.
namespace enter {
constexpr std::size_t kUserRef = 1;
constexpr std::size_t kSide = 5;
constexpr std::size_t kQty = 6;
constexpr std::size_t kSymbol = 10;
constexpr std::size_t kPrice = 18;
constexpr std::size_t kTif = 26;
constexpr std::size_t kType = 27;
constexpr std::size_t kPostOnly = 28;
static_assert(kPostOnly + 1 == kEnterOrderLength);
}  // namespace enter

namespace replace {
constexpr std::size_t kOrigUserRef = 1;
constexpr std::size_t kUserRef = 5;
constexpr std::size_t kQty = 9;
constexpr std::size_t kPrice = 13;
static_assert(kPrice + 8 == kReplaceOrderLength);
}  // namespace replace

namespace cancel {
constexpr std::size_t kUserRef = 1;
constexpr std::size_t kQty = 5;
static_assert(kQty + 4 == kCancelOrderLength);
}  // namespace cancel

// Every outbound message starts with the type byte and an 8-byte timestamp.
constexpr std::size_t kTimestamp = 1;

namespace accepted {
constexpr std::size_t kUserRef = 9;
constexpr std::size_t kSide = 13;
constexpr std::size_t kQty = 14;
constexpr std::size_t kSymbol = 18;
constexpr std::size_t kPrice = 26;
constexpr std::size_t kTif = 34;
constexpr std::size_t kType = 35;
constexpr std::size_t kPostOnly = 36;
constexpr std::size_t kOrderRef = 37;
static_assert(kOrderRef + 8 == kAcceptedLength);
}  // namespace accepted

namespace replaced {
constexpr std::size_t kOrigUserRef = 9;
constexpr std::size_t kUserRef = 13;
constexpr std::size_t kSide = 17;
constexpr std::size_t kQty = 18;
constexpr std::size_t kSymbol = 22;
constexpr std::size_t kPrice = 30;
constexpr std::size_t kOrderRef = 38;
constexpr std::size_t kKeptPriority = 46;
static_assert(kKeptPriority + 1 == kReplacedLength);
}  // namespace replaced

namespace canceled {
constexpr std::size_t kUserRef = 9;
constexpr std::size_t kQty = 13;
constexpr std::size_t kReason = 17;
static_assert(kReason + 1 == kCanceledLength);
}  // namespace canceled

namespace executed {
constexpr std::size_t kUserRef = 9;
constexpr std::size_t kQty = 13;
constexpr std::size_t kPrice = 17;
constexpr std::size_t kLiquidity = 25;
constexpr std::size_t kMatch = 26;
static_assert(kMatch + 8 == kExecutedLength);
}  // namespace executed

namespace rejected {
constexpr std::size_t kUserRef = 9;
constexpr std::size_t kReason = 13;
static_assert(kReason + 2 == kRejectedLength);
}  // namespace rejected

static_assert(kMaxMessageLength ==
              std::max({kEnterOrderLength, kReplaceOrderLength, kCancelOrderLength, kAcceptedLength,
                        kReplacedLength, kCanceledLength, kExecutedLength, kRejectedLength}));

// Reading fields. Each reader returns nothing for a byte value that is not allowed, and
// the decoder turns that into the matching DecodeStatus.

char load_char(std::span<const std::byte> f, std::size_t offset) noexcept {
    return static_cast<char>(f[offset]);
}

std::optional<Side> load_side(std::span<const std::byte> f, std::size_t offset) noexcept {
    switch (load_char(f, offset)) {
        case 'B':
            return Side::Buy;
        case 'S':
            return Side::Sell;
        default:
            return std::nullopt;
    }
}

// The time in force codes are FIX tag 59 values, which OUCH also uses. '0' is OUCH's Day
// order. litbook has no session close, so a Day order rests until it is filled or
// canceled, which is the book's GoodTillCancel.
std::optional<TimeInForce> load_tif(std::span<const std::byte> f, std::size_t offset) noexcept {
    switch (load_char(f, offset)) {
        case '0':
            return TimeInForce::GoodTillCancel;
        case '3':
            return TimeInForce::ImmediateOrCancel;
        case '4':
            return TimeInForce::FillOrKill;
        default:
            return std::nullopt;
    }
}

std::optional<OrderType> load_type(std::span<const std::byte> f, std::size_t offset) noexcept {
    switch (load_char(f, offset)) {
        case 'L':
            return OrderType::Limit;
        case 'M':
            return OrderType::Market;
        default:
            return std::nullopt;
    }
}

std::optional<bool> load_flag(std::span<const std::byte> f, std::size_t offset) noexcept {
    switch (load_char(f, offset)) {
        case 'Y':
            return true;
        case 'N':
            return false;
        default:
            return std::nullopt;
    }
}

std::optional<Price> load_price(std::span<const std::byte> f, std::size_t offset) noexcept {
    const auto raw = load_be<std::uint64_t>(f, offset);
    if (raw > static_cast<std::uint64_t>(kMaxWirePrice)) {
        return std::nullopt;
    }
    return static_cast<Price>(raw);
}

std::optional<Ticker> load_symbol(std::span<const std::byte> f, std::size_t offset) noexcept {
    std::array<char, Ticker::kLength> chars{};
    for (std::size_t i = 0; i < chars.size(); ++i) {
        chars[i] = load_char(f, offset + i);
    }
    return parse_ticker(chars);
}

// Writing fields. The encoders check prices before writing anything, so these never fail.

void store_char(std::span<std::byte> out, std::size_t offset, char c) noexcept {
    out[offset] = static_cast<std::byte>(c);
}

char side_char(Side side) noexcept {
    return side == Side::Buy ? 'B' : 'S';
}

char tif_char(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::GoodTillCancel:
            return '0';
        case TimeInForce::ImmediateOrCancel:
            return '3';
        case TimeInForce::FillOrKill:
            return '4';
    }
    return '?';
}

char type_char(OrderType type) noexcept {
    return type == OrderType::Limit ? 'L' : 'M';
}

char flag_char(bool flag) noexcept {
    return flag ? 'Y' : 'N';
}

bool price_fits(Price price) noexcept {
    return price >= 0 && price <= kMaxWirePrice;
}

void store_price(std::span<std::byte> out, std::size_t offset, Price price) noexcept {
    store_be(out, offset, static_cast<std::uint64_t>(price));
}

void store_symbol(std::span<std::byte> out, std::size_t offset, const Ticker& t) noexcept {
    for (std::size_t i = 0; i < Ticker::kLength; ++i) {
        store_char(out, offset + i, t.bytes()[i]);
    }
}

// Checks the frame against the expected length of its type.
DecodeStatus check_length(std::size_t actual, std::size_t expected) noexcept {
    if (actual < expected) {
        return DecodeStatus::TooShort;
    }
    if (actual > expected) {
        return DecodeStatus::TooLong;
    }
    return DecodeStatus::Ok;
}

template <class Message>
Decoded<Message> failed(DecodeStatus status) noexcept {
    return Decoded<Message>{.status = status, .message = std::nullopt};
}

// Decoders for one message each. The frame length has already been checked. Fields are
// checked in wire order, so the status names the first bad field.

Decoded<InboundMessage> decode_enter(std::span<const std::byte> f) noexcept {
    using R = InboundMessage;
    const auto side = load_side(f, enter::kSide);
    if (!side) {
        return failed<R>(DecodeStatus::BadSide);
    }
    const auto symbol = load_symbol(f, enter::kSymbol);
    if (!symbol) {
        return failed<R>(DecodeStatus::BadSymbol);
    }
    const auto price = load_price(f, enter::kPrice);
    if (!price) {
        return failed<R>(DecodeStatus::BadPrice);
    }
    const auto tif = load_tif(f, enter::kTif);
    if (!tif) {
        return failed<R>(DecodeStatus::BadTimeInForce);
    }
    const auto type = load_type(f, enter::kType);
    if (!type) {
        return failed<R>(DecodeStatus::BadOrderType);
    }
    const auto post_only = load_flag(f, enter::kPostOnly);
    if (!post_only) {
        return failed<R>(DecodeStatus::BadFlag);
    }
    return {.status = DecodeStatus::Ok,
            .message = EnterOrder{.user_ref = load_be<std::uint32_t>(f, enter::kUserRef),
                                  .side = *side,
                                  .qty = load_be<std::uint32_t>(f, enter::kQty),
                                  .symbol = *symbol,
                                  .price = *price,
                                  .tif = *tif,
                                  .type = *type,
                                  .post_only = *post_only}};
}

Decoded<InboundMessage> decode_replace(std::span<const std::byte> f) noexcept {
    const auto price = load_price(f, replace::kPrice);
    if (!price) {
        return failed<InboundMessage>(DecodeStatus::BadPrice);
    }
    return {
        .status = DecodeStatus::Ok,
        .message = ReplaceOrder{.orig_user_ref = load_be<std::uint32_t>(f, replace::kOrigUserRef),
                                .user_ref = load_be<std::uint32_t>(f, replace::kUserRef),
                                .qty = load_be<std::uint32_t>(f, replace::kQty),
                                .price = *price}};
}

Decoded<InboundMessage> decode_cancel(std::span<const std::byte> f) noexcept {
    return {.status = DecodeStatus::Ok,
            .message = CancelOrder{.user_ref = load_be<std::uint32_t>(f, cancel::kUserRef),
                                   .qty = load_be<std::uint32_t>(f, cancel::kQty)}};
}

Decoded<OutboundMessage> decode_accepted(std::span<const std::byte> f) noexcept {
    using R = OutboundMessage;
    const auto side = load_side(f, accepted::kSide);
    if (!side) {
        return failed<R>(DecodeStatus::BadSide);
    }
    const auto symbol = load_symbol(f, accepted::kSymbol);
    if (!symbol) {
        return failed<R>(DecodeStatus::BadSymbol);
    }
    const auto price = load_price(f, accepted::kPrice);
    if (!price) {
        return failed<R>(DecodeStatus::BadPrice);
    }
    const auto tif = load_tif(f, accepted::kTif);
    if (!tif) {
        return failed<R>(DecodeStatus::BadTimeInForce);
    }
    const auto type = load_type(f, accepted::kType);
    if (!type) {
        return failed<R>(DecodeStatus::BadOrderType);
    }
    const auto post_only = load_flag(f, accepted::kPostOnly);
    if (!post_only) {
        return failed<R>(DecodeStatus::BadFlag);
    }
    return {
        .status = DecodeStatus::Ok,
        .message = AcceptedMessage{.timestamp = load_be<std::uint64_t>(f, kTimestamp),
                                   .user_ref = load_be<std::uint32_t>(f, accepted::kUserRef),
                                   .side = *side,
                                   .qty = load_be<std::uint32_t>(f, accepted::kQty),
                                   .symbol = *symbol,
                                   .price = *price,
                                   .tif = *tif,
                                   .type = *type,
                                   .post_only = *post_only,
                                   .order_ref = load_be<std::uint64_t>(f, accepted::kOrderRef)}};
}

Decoded<OutboundMessage> decode_replaced(std::span<const std::byte> f) noexcept {
    using R = OutboundMessage;
    const auto side = load_side(f, replaced::kSide);
    if (!side) {
        return failed<R>(DecodeStatus::BadSide);
    }
    const auto symbol = load_symbol(f, replaced::kSymbol);
    if (!symbol) {
        return failed<R>(DecodeStatus::BadSymbol);
    }
    const auto price = load_price(f, replaced::kPrice);
    if (!price) {
        return failed<R>(DecodeStatus::BadPrice);
    }
    const auto kept = load_flag(f, replaced::kKeptPriority);
    if (!kept) {
        return failed<R>(DecodeStatus::BadFlag);
    }
    return {.status = DecodeStatus::Ok,
            .message =
                ReplacedMessage{.timestamp = load_be<std::uint64_t>(f, kTimestamp),
                                .orig_user_ref = load_be<std::uint32_t>(f, replaced::kOrigUserRef),
                                .user_ref = load_be<std::uint32_t>(f, replaced::kUserRef),
                                .side = *side,
                                .qty = load_be<std::uint32_t>(f, replaced::kQty),
                                .symbol = *symbol,
                                .price = *price,
                                .order_ref = load_be<std::uint64_t>(f, replaced::kOrderRef),
                                .kept_priority = *kept}};
}

Decoded<OutboundMessage> decode_canceled(std::span<const std::byte> f) noexcept {
    return {.status = DecodeStatus::Ok,
            .message = CanceledMessage{
                .timestamp = load_be<std::uint64_t>(f, kTimestamp),
                .user_ref = load_be<std::uint32_t>(f, canceled::kUserRef),
                .qty = load_be<std::uint32_t>(f, canceled::kQty),
                .reason = static_cast<WireCancelReason>(load_char(f, canceled::kReason))}};
}

Decoded<OutboundMessage> decode_executed(std::span<const std::byte> f) noexcept {
    const auto price = load_price(f, executed::kPrice);
    if (!price) {
        return failed<OutboundMessage>(DecodeStatus::BadPrice);
    }
    return {.status = DecodeStatus::Ok,
            .message = ExecutedMessage{
                .timestamp = load_be<std::uint64_t>(f, kTimestamp),
                .user_ref = load_be<std::uint32_t>(f, executed::kUserRef),
                .qty = load_be<std::uint32_t>(f, executed::kQty),
                .price = *price,
                .liquidity = static_cast<Liquidity>(load_char(f, executed::kLiquidity)),
                .match = load_be<std::uint64_t>(f, executed::kMatch)}};
}

Decoded<OutboundMessage> decode_rejected(std::span<const std::byte> f) noexcept {
    return {.status = DecodeStatus::Ok,
            .message = RejectedMessage{.timestamp = load_be<std::uint64_t>(f, kTimestamp),
                                       .user_ref = load_be<std::uint32_t>(f, rejected::kUserRef),
                                       .reason = static_cast<WireRejectReason>(
                                           load_be<std::uint16_t>(f, rejected::kReason))}};
}

}  // namespace

std::ostream& operator<<(std::ostream& os, DecodeStatus status) {
    switch (status) {
        case DecodeStatus::Ok:
            return os << "Ok";
        case DecodeStatus::Empty:
            return os << "Empty";
        case DecodeStatus::UnknownType:
            return os << "UnknownType";
        case DecodeStatus::TooShort:
            return os << "TooShort";
        case DecodeStatus::TooLong:
            return os << "TooLong";
        case DecodeStatus::BadSide:
            return os << "BadSide";
        case DecodeStatus::BadSymbol:
            return os << "BadSymbol";
        case DecodeStatus::BadPrice:
            return os << "BadPrice";
        case DecodeStatus::BadTimeInForce:
            return os << "BadTimeInForce";
        case DecodeStatus::BadOrderType:
            return os << "BadOrderType";
        case DecodeStatus::BadFlag:
            return os << "BadFlag";
    }
    return os << "DecodeStatus(" << static_cast<int>(status) << ")";
}

std::size_t inbound_length(std::byte type) noexcept {
    switch (static_cast<char>(type)) {
        case kEnterOrderType:
            return kEnterOrderLength;
        case kReplaceOrderType:
            return kReplaceOrderLength;
        case kCancelOrderType:
            return kCancelOrderLength;
        default:
            return 0;
    }
}

std::size_t outbound_length(std::byte type) noexcept {
    switch (static_cast<char>(type)) {
        case kAcceptedType:
            return kAcceptedLength;
        case kReplacedType:
            return kReplacedLength;
        case kCanceledType:
            return kCanceledLength;
        case kExecutedType:
            return kExecutedLength;
        case kRejectedType:
            return kRejectedLength;
        default:
            return 0;
    }
}

Decoded<InboundMessage> decode_inbound(std::span<const std::byte> frame) noexcept {
    if (frame.empty()) {
        return failed<InboundMessage>(DecodeStatus::Empty);
    }
    const std::size_t expected = inbound_length(frame[0]);
    if (expected == 0) {
        return failed<InboundMessage>(DecodeStatus::UnknownType);
    }
    if (const DecodeStatus s = check_length(frame.size(), expected); s != DecodeStatus::Ok) {
        return failed<InboundMessage>(s);
    }
    switch (static_cast<char>(frame[0])) {
        case kEnterOrderType:
            return decode_enter(frame);
        case kReplaceOrderType:
            return decode_replace(frame);
        default:
            return decode_cancel(frame);
    }
}

Decoded<OutboundMessage> decode_outbound(std::span<const std::byte> frame) noexcept {
    if (frame.empty()) {
        return failed<OutboundMessage>(DecodeStatus::Empty);
    }
    const std::size_t expected = outbound_length(frame[0]);
    if (expected == 0) {
        return failed<OutboundMessage>(DecodeStatus::UnknownType);
    }
    if (const DecodeStatus s = check_length(frame.size(), expected); s != DecodeStatus::Ok) {
        return failed<OutboundMessage>(s);
    }
    switch (static_cast<char>(frame[0])) {
        case kAcceptedType:
            return decode_accepted(frame);
        case kReplacedType:
            return decode_replaced(frame);
        case kCanceledType:
            return decode_canceled(frame);
        case kExecutedType:
            return decode_executed(frame);
        default:
            return decode_rejected(frame);
    }
}

std::size_t encode(const EnterOrder& m, std::span<std::byte> out) noexcept {
    if (out.size() < kEnterOrderLength || !price_fits(m.price)) {
        return 0;
    }
    store_char(out, 0, kEnterOrderType);
    store_be(out, enter::kUserRef, m.user_ref);
    store_char(out, enter::kSide, side_char(m.side));
    store_be(out, enter::kQty, m.qty);
    store_symbol(out, enter::kSymbol, m.symbol);
    store_price(out, enter::kPrice, m.price);
    store_char(out, enter::kTif, tif_char(m.tif));
    store_char(out, enter::kType, type_char(m.type));
    store_char(out, enter::kPostOnly, flag_char(m.post_only));
    return kEnterOrderLength;
}

std::size_t encode(const ReplaceOrder& m, std::span<std::byte> out) noexcept {
    if (out.size() < kReplaceOrderLength || !price_fits(m.price)) {
        return 0;
    }
    store_char(out, 0, kReplaceOrderType);
    store_be(out, replace::kOrigUserRef, m.orig_user_ref);
    store_be(out, replace::kUserRef, m.user_ref);
    store_be(out, replace::kQty, m.qty);
    store_price(out, replace::kPrice, m.price);
    return kReplaceOrderLength;
}

std::size_t encode(const CancelOrder& m, std::span<std::byte> out) noexcept {
    if (out.size() < kCancelOrderLength) {
        return 0;
    }
    store_char(out, 0, kCancelOrderType);
    store_be(out, cancel::kUserRef, m.user_ref);
    store_be(out, cancel::kQty, m.qty);
    return kCancelOrderLength;
}

std::size_t encode(const AcceptedMessage& m, std::span<std::byte> out) noexcept {
    if (out.size() < kAcceptedLength || !price_fits(m.price)) {
        return 0;
    }
    store_char(out, 0, kAcceptedType);
    store_be(out, kTimestamp, m.timestamp);
    store_be(out, accepted::kUserRef, m.user_ref);
    store_char(out, accepted::kSide, side_char(m.side));
    store_be(out, accepted::kQty, m.qty);
    store_symbol(out, accepted::kSymbol, m.symbol);
    store_price(out, accepted::kPrice, m.price);
    store_char(out, accepted::kTif, tif_char(m.tif));
    store_char(out, accepted::kType, type_char(m.type));
    store_char(out, accepted::kPostOnly, flag_char(m.post_only));
    store_be(out, accepted::kOrderRef, m.order_ref);
    return kAcceptedLength;
}

std::size_t encode(const ReplacedMessage& m, std::span<std::byte> out) noexcept {
    if (out.size() < kReplacedLength || !price_fits(m.price)) {
        return 0;
    }
    store_char(out, 0, kReplacedType);
    store_be(out, kTimestamp, m.timestamp);
    store_be(out, replaced::kOrigUserRef, m.orig_user_ref);
    store_be(out, replaced::kUserRef, m.user_ref);
    store_char(out, replaced::kSide, side_char(m.side));
    store_be(out, replaced::kQty, m.qty);
    store_symbol(out, replaced::kSymbol, m.symbol);
    store_price(out, replaced::kPrice, m.price);
    store_be(out, replaced::kOrderRef, m.order_ref);
    store_char(out, replaced::kKeptPriority, flag_char(m.kept_priority));
    return kReplacedLength;
}

std::size_t encode(const CanceledMessage& m, std::span<std::byte> out) noexcept {
    if (out.size() < kCanceledLength) {
        return 0;
    }
    store_char(out, 0, kCanceledType);
    store_be(out, kTimestamp, m.timestamp);
    store_be(out, canceled::kUserRef, m.user_ref);
    store_be(out, canceled::kQty, m.qty);
    store_char(out, canceled::kReason, static_cast<char>(m.reason));
    return kCanceledLength;
}

std::size_t encode(const ExecutedMessage& m, std::span<std::byte> out) noexcept {
    if (out.size() < kExecutedLength || !price_fits(m.price)) {
        return 0;
    }
    store_char(out, 0, kExecutedType);
    store_be(out, kTimestamp, m.timestamp);
    store_be(out, executed::kUserRef, m.user_ref);
    store_be(out, executed::kQty, m.qty);
    store_price(out, executed::kPrice, m.price);
    store_char(out, executed::kLiquidity, static_cast<char>(m.liquidity));
    store_be(out, executed::kMatch, m.match);
    return kExecutedLength;
}

std::size_t encode(const RejectedMessage& m, std::span<std::byte> out) noexcept {
    if (out.size() < kRejectedLength) {
        return 0;
    }
    store_char(out, 0, kRejectedType);
    store_be(out, kTimestamp, m.timestamp);
    store_be(out, rejected::kUserRef, m.user_ref);
    store_be(out, rejected::kReason, static_cast<std::uint16_t>(m.reason));
    return kRejectedLength;
}

// std::visit can throw std::bad_variant_access, which a noexcept function must not, so
// these check each alternative with std::get_if instead. A variant always holds one of
// its alternatives here, so the final return is never reached.

std::size_t encode(const InboundMessage& m, std::span<std::byte> out) noexcept {
    if (const auto* e = std::get_if<EnterOrder>(&m)) {
        return encode(*e, out);
    }
    if (const auto* r = std::get_if<ReplaceOrder>(&m)) {
        return encode(*r, out);
    }
    if (const auto* c = std::get_if<CancelOrder>(&m)) {
        return encode(*c, out);
    }
    return 0;
}

std::size_t encode(const OutboundMessage& m, std::span<std::byte> out) noexcept {
    if (const auto* a = std::get_if<AcceptedMessage>(&m)) {
        return encode(*a, out);
    }
    if (const auto* u = std::get_if<ReplacedMessage>(&m)) {
        return encode(*u, out);
    }
    if (const auto* c = std::get_if<CanceledMessage>(&m)) {
        return encode(*c, out);
    }
    if (const auto* x = std::get_if<ExecutedMessage>(&m)) {
        return encode(*x, out);
    }
    if (const auto* j = std::get_if<RejectedMessage>(&m)) {
        return encode(*j, out);
    }
    return 0;
}

}  // namespace litbook::protocol
