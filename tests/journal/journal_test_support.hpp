#pragma once

#include "litbook/journal/crc32c.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/protocol/byte_order.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "support/unwrap.hpp"

// Small helpers shared by the journal tests.
namespace litbook::journal::testing {

using litbook::testing::unwrap;

inline std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// Runs an encode function into a buffer of the largest record size and returns exactly
// the bytes it wrote (nothing if it refused).
template <class Encode>
std::vector<std::byte> encoded(Encode encode) {
    std::array<std::byte, kMaxRecordLength> buffer{};
    const std::size_t n = encode(std::span<std::byte>(buffer));
    return {buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n)};
}

// Puts a fresh checksum on a record (the whole vector), so a test can change one field
// and see that field's own check fail instead of the checksum.
inline void reseal(std::vector<std::byte>& record) {
    const std::size_t checked = record.size() - kChecksumLength;
    protocol::store_le(std::span<std::byte>(record), checked,
                       crc32c(std::span<const std::byte>(record).first(checked)));
}

// The same for a file header.
inline void reseal_header(std::vector<std::byte>& header) {
    protocol::store_le(std::span<std::byte>(header), 28,
                       crc32c(std::span<const std::byte>(header).first(28)));
}

inline SymbolListing sample_listing(SymbolId id = 0, const char* ticker = "AAPL") {
    BookConfig config;
    config.tick_size = 100;
    config.lot_size = 1;
    config.self_trade_policy = SelfTradePolicy::CancelOldest;
    config.min_price = 100;
    config.max_price = 10'000'000;
    config.max_orders = 4096;
    return SymbolListing{.id = id, .ticker = unwrap(make_ticker(ticker)), .config = config};
}

inline pipeline::Request sample_frame(OwnerId owner, Timestamp now,
                                      std::initializer_list<unsigned> frame) {
    const std::vector<std::byte> b = bytes(frame);
    return pipeline::make_frame_request(owner, b, now);
}

}  // namespace litbook::journal::testing
