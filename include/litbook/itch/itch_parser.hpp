#pragma once

#include "litbook/itch/itch_messages.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace litbook::itch {

// Decodes one ITCH 5.0 message payload (the bytes after the 2-byte length prefix) and
// passes the result to a handler. The handler is a template parameter, so each call is
// resolved at compile time, the same way the order books work (D-025).
//
// A handler needs these member functions. Any of them may ignore its argument.
//   on_system_event(const SystemEvent&)       on_stock_directory(const StockDirectory&)
//   on_stock_trading_action(const StockTradingAction&)
//   on_add_order(const AddOrder&)             on_order_executed(const OrderExecuted&)
//   on_order_executed_with_price(const OrderExecutedWithPrice&)
//   on_order_cancel(const OrderCancel&)       on_order_delete(const OrderDelete&)
//   on_order_replace(const OrderReplace&)
//
// Parsing never allocates and never throws. A payload of a known type with the wrong
// length is reported as BadLength and not passed on, because its fields cannot be
// trusted. Types this parser does not decode (trades, crosses, imbalances and so on) are
// reported as Skipped.

enum class ParseStatus : std::uint8_t { Decoded, Skipped, BadLength, Empty };

template <class H>
concept ItchHandler = requires(H& h) {
    h.on_system_event(SystemEvent{});
    h.on_stock_directory(StockDirectory{});
    h.on_stock_trading_action(StockTradingAction{});
    h.on_add_order(AddOrder{});
    h.on_order_executed(OrderExecuted{});
    h.on_order_executed_with_price(OrderExecutedWithPrice{});
    h.on_order_cancel(OrderCancel{});
    h.on_order_delete(OrderDelete{});
    h.on_order_replace(OrderReplace{});
};

namespace detail {

// Big-endian reads. The caller has already checked that the bytes exist.
inline std::uint64_t read_be(std::span<const std::byte> p, std::size_t offset,
                             std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(p[offset + i]);
    }
    return value;
}
inline std::uint16_t be16(std::span<const std::byte> p, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(read_be(p, at, 2));
}
inline std::uint32_t be32(std::span<const std::byte> p, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(read_be(p, at, 4));
}
inline std::uint64_t be48(std::span<const std::byte> p, std::size_t at) noexcept {
    return read_be(p, at, 6);
}
inline std::uint64_t be64(std::span<const std::byte> p, std::size_t at) noexcept {
    return read_be(p, at, 8);
}
inline char ch(std::span<const std::byte> p, std::size_t at) noexcept {
    return static_cast<char>(std::to_integer<unsigned char>(p[at]));
}
template <std::size_t N>
std::array<char, N> chars(std::span<const std::byte> p, std::size_t at) noexcept {
    std::array<char, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = ch(p, at + i);
    }
    return out;
}
inline Header header(std::span<const std::byte> p) noexcept {
    return {.type = ch(p, 0),
            .stock_locate = be16(p, 1),
            .tracking_number = be16(p, 3),
            .timestamp_ns = be48(p, 5)};
}

}  // namespace detail

template <ItchHandler H>
ParseStatus parse_message(std::span<const std::byte> p, H& handler) {
    using namespace detail;
    if (p.empty()) {
        return ParseStatus::Empty;
    }
    const std::size_t n = p.size();
    switch (ch(p, 0)) {
        case 'S':
            if (n != kSystemEventLength) {
                return ParseStatus::BadLength;
            }
            handler.on_system_event({.header = header(p), .event_code = ch(p, 11)});
            return ParseStatus::Decoded;
        case 'R':
            if (n != kStockDirectoryLength) {
                return ParseStatus::BadLength;
            }
            handler.on_stock_directory(
                {.header = header(p), .stock = chars<8>(p, 11), .round_lot_size = be32(p, 21)});
            return ParseStatus::Decoded;
        case 'H':
            if (n != kStockTradingActionLength) {
                return ParseStatus::BadLength;
            }
            handler.on_stock_trading_action({.header = header(p),
                                             .stock = chars<8>(p, 11),
                                             .trading_state = ch(p, 19),
                                             .reason = chars<4>(p, 21)});
            return ParseStatus::Decoded;
        case 'A':
        case 'F': {
            const bool mpid = ch(p, 0) == 'F';
            if (n != (mpid ? kAddOrderMpidLength : kAddOrderLength)) {
                return ParseStatus::BadLength;
            }
            handler.on_add_order(
                {.header = header(p),
                 .order_ref = be64(p, 11),
                 .side = ch(p, 19),
                 .shares = be32(p, 20),
                 .stock = chars<8>(p, 24),
                 .price = be32(p, 32),
                 .has_mpid = mpid,
                 .mpid = mpid ? chars<4>(p, 36) : std::array<char, 4>{' ', ' ', ' ', ' '}});
            return ParseStatus::Decoded;
        }
        case 'E':
            if (n != kOrderExecutedLength) {
                return ParseStatus::BadLength;
            }
            handler.on_order_executed({.header = header(p),
                                       .order_ref = be64(p, 11),
                                       .executed_shares = be32(p, 19),
                                       .match_number = be64(p, 23)});
            return ParseStatus::Decoded;
        case 'C':
            if (n != kOrderExecutedWithPriceLength) {
                return ParseStatus::BadLength;
            }
            handler.on_order_executed_with_price({.header = header(p),
                                                  .order_ref = be64(p, 11),
                                                  .executed_shares = be32(p, 19),
                                                  .match_number = be64(p, 23),
                                                  .printable = ch(p, 31),
                                                  .execution_price = be32(p, 32)});
            return ParseStatus::Decoded;
        case 'X':
            if (n != kOrderCancelLength) {
                return ParseStatus::BadLength;
            }
            handler.on_order_cancel(
                {.header = header(p), .order_ref = be64(p, 11), .canceled_shares = be32(p, 19)});
            return ParseStatus::Decoded;
        case 'D':
            if (n != kOrderDeleteLength) {
                return ParseStatus::BadLength;
            }
            handler.on_order_delete({.header = header(p), .order_ref = be64(p, 11)});
            return ParseStatus::Decoded;
        case 'U':
            if (n != kOrderReplaceLength) {
                return ParseStatus::BadLength;
            }
            handler.on_order_replace({.header = header(p),
                                      .original_order_ref = be64(p, 11),
                                      .new_order_ref = be64(p, 19),
                                      .shares = be32(p, 27),
                                      .price = be32(p, 31)});
            return ParseStatus::Decoded;
        default:
            return ParseStatus::Skipped;
    }
}

}  // namespace litbook::itch
