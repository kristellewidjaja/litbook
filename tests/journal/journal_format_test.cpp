#include "litbook/journal/journal_format.hpp"

#include "litbook/journal/crc32c.hpp"
#include "litbook/protocol/byte_order.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "journal/journal_test_support.hpp"

namespace litbook::journal {
namespace {

using testing::bytes;
using testing::encoded;
using testing::reseal;
using testing::reseal_header;
using testing::sample_frame;
using testing::sample_listing;
using testing::unwrap;

// Appends the CRC32C of everything so far, lowest byte first, as the format ends every
// record and header. The tests compute it here with the table version, so the layout is
// checked independently of how the encoder fills in the checksum.
std::vector<std::byte> with_checksum(std::vector<std::byte> b) {
    const std::uint32_t crc = crc32c_software(0, b);
    for (unsigned i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::byte>((crc >> (8U * i)) & 0xFFU));
    }
    return b;
}

std::vector<std::byte> concat(std::vector<std::byte> a, const std::vector<std::byte>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// The 24-byte record header for the golden tests. Every field is little-endian.
std::vector<std::byte> golden_header(unsigned length, char type, unsigned owner) {
    return bytes({length & 0xFFU, length >> 8U, static_cast<unsigned char>(type), 0x00,
                  owner & 0xFFU, (owner >> 8U) & 0xFFU, 0, 0,
                  // Sequence 7.
                  0x07, 0, 0, 0, 0, 0, 0, 0,
                  // Timestamp 0x0102030405060708.
                  0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01});
}

constexpr Timestamp kTime = 0x0102030405060708ULL;

// Golden bytes

TEST(JournalFormatTest, FileHeaderBytes) {
    std::array<std::byte, kFileHeaderLength> out{};
    ASSERT_EQ(encode_file_header(out, 0x1122), kFileHeaderLength);
    const std::vector<std::byte> expected = with_checksum(bytes({
        'L',  'I',  'T', 'B', 'O', 'O', 'K', 'J',  // magic
        0x01, 0x00,                                // version 1
        0x20, 0x00,                                // header length 32
        0,    0,    0,   0,                        // flags
        0x22, 0x11, 0,   0,   0,   0,   0,   0,    // first sequence 0x1122
        0,    0,    0,   0,                        // reserved
    }));
    EXPECT_EQ(std::vector<std::byte>(out.begin(), out.end()), expected);
}

TEST(JournalFormatTest, FrameRecordBytes) {
    // A 9-byte Cancel Order as the client sent it ('X', UserRefNum 5, quantity 0). The
    // OUCH bytes stay big-endian inside the little-endian record.
    const auto request = sample_frame(3, kTime, {'X', 0, 0, 0, 5, 0, 0, 0, 0});
    const auto out = encoded([&](auto o) { return encode_request(o, 7, request); });
    const auto expected =
        with_checksum(concat(golden_header(37, 'F', 3), bytes({'X', 0, 0, 0, 5, 0, 0, 0, 0})));
    EXPECT_EQ(out, expected);
}

TEST(JournalFormatTest, CancelAllAndStopRecordBytes) {
    const auto cancel_all = encoded([&](auto o) {
        return encode_request(o, 7, pipeline::make_cancel_all_request(258, kTime));
    });
    EXPECT_EQ(cancel_all, with_checksum(golden_header(28, 'C', 258)));

    // A Stop request has no account, so the record's owner is 0 even if the request's
    // owner field held something else.
    pipeline::Request stop = pipeline::make_stop_request(kTime);
    stop.owner = 9;
    const auto out = encoded([&](auto o) { return encode_request(o, 7, stop); });
    EXPECT_EQ(out, with_checksum(golden_header(28, 'E', 0)));
}

TEST(JournalFormatTest, SessionStartRecordBytes) {
    protocol::OrderEntryConfig config;
    config.max_owners = 4;
    config.max_live_orders = 0x10000;
    config.max_symbols = 300;
    const auto out = encoded([&](auto o) { return encode_session_start(o, 7, kTime, config); });
    const auto expected = with_checksum(
        concat(golden_header(40, 'B', 0), bytes({4, 0, 0, 0, 0, 0, 1, 0, 0x2C, 0x01, 0, 0})));
    EXPECT_EQ(out, expected);
}

TEST(JournalFormatTest, AddSymbolRecordBytes) {
    SymbolListing listing = sample_listing(0x0102, "MSFT");
    listing.config.min_price = -2;
    const auto out = encoded([&](auto o) { return encode_add_symbol(o, 7, kTime, listing); });
    const auto payload = bytes({
        0x02, 0x01,                                      // symbol id 0x0102
        'M',  'S',  'F',  'T',  ' ',  ' ',  ' ',  ' ',   // ticker, as on the wire
        0x01,                                            // CancelOldest
        0x00,                                            // reserved
        0x01, 0,    0,    0,                             // lot size 1
        0x00, 0x10, 0,    0,                             // max orders 4096
        100,  0,    0,    0,    0,    0,    0,    0,     // tick size 100
        0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // min price -2
        0x80, 0x96, 0x98, 0,    0,    0,    0,    0,     // max price 10,000,000
    });
    ASSERT_EQ(payload.size(), kAddSymbolPayload);
    EXPECT_EQ(out, with_checksum(concat(golden_header(72, 'S', 0), payload)));
}

TEST(JournalFormatTest, RiskLimitsRecordBytes) {
    const protocol::RiskLimits limits{.max_order_qty = 1000, .max_open_orders = 3};
    const auto out = encoded([&](auto o) { return encode_risk_limits(o, 7, kTime, 2, limits); });
    EXPECT_EQ(out, with_checksum(
                       concat(golden_header(36, 'R', 2), bytes({0xE8, 0x03, 0, 0, 3, 0, 0, 0}))));
}

// Round trips

TEST(JournalFormatTest, EveryRecordTypeRoundTrips) {
    protocol::OrderEntryConfig config;
    config.max_owners = 7;
    config.max_live_orders = 99;
    config.max_symbols = 65'535;
    SymbolListing listing = sample_listing(12, "ZVZZT");
    listing.config.min_price = std::numeric_limits<Price>::min();
    listing.config.max_price = std::numeric_limits<Price>::max();
    listing.config.self_trade_policy = SelfTradePolicy::Decrement;
    listing.config.max_orders = std::numeric_limits<std::uint32_t>::max();
    const protocol::RiskLimits limits{.max_order_qty = 0, .max_open_orders = 0xFFFFFFFFU};
    std::vector<std::byte> longest(kMaxPayload, std::byte{0xAB});
    const pipeline::Request frame = pipeline::make_frame_request(0xFFFFFFFFU, longest, ~0ULL);

    const std::vector<std::vector<std::byte>> records{
        encoded([&](auto o) { return encode_session_start(o, 1, 11, config); }),
        encoded([&](auto o) { return encode_add_symbol(o, 2, 12, listing); }),
        encoded([&](auto o) { return encode_risk_limits(o, 3, 13, 5, limits); }),
        encoded([&](auto o) { return encode_request(o, 4, frame); }),
        encoded(
            [&](auto o) { return encode_request(o, 5, pipeline::make_cancel_all_request(0, 15)); }),
        encoded([&](auto o) { return encode_request(o, ~0ULL, pipeline::make_stop_request(0)); }),
    };
    for (const auto& r : records) {
        ASSERT_FALSE(r.empty());
        const DecodedRecord d = decode_record(r);
        ASSERT_EQ(d.status, RecordStatus::Ok);
        EXPECT_EQ(d.length, r.size());
        // Encoding the decoded record gives back exactly the same bytes.
        EXPECT_EQ(encoded([&](auto o) { return encode_record(o, d.record); }), r);
    }

    EXPECT_EQ(unwrap(session_config_of(decode_record(records[0]).record)).max_symbols, 65'535U);
    const SymbolListing back = unwrap(symbol_listing_of(decode_record(records[1]).record));
    EXPECT_EQ(back.id, 12);
    EXPECT_EQ(back.ticker, listing.ticker);
    EXPECT_EQ(back.config.min_price, std::numeric_limits<Price>::min());
    EXPECT_EQ(back.config.max_price, std::numeric_limits<Price>::max());
    EXPECT_EQ(back.config.self_trade_policy, SelfTradePolicy::Decrement);
    EXPECT_EQ(back.config.max_orders, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(unwrap(risk_limits_of(decode_record(records[2]).record)), limits);

    const JournalRecord f = decode_record(records[3]).record;
    EXPECT_EQ(f.owner, 0xFFFFFFFFU);
    EXPECT_EQ(f.sequence, 4U);
    EXPECT_EQ(f.now, ~0ULL);
    const pipeline::Request back_frame = unwrap(request_of(f));
    EXPECT_EQ(back_frame.kind, pipeline::RequestKind::Frame);
    EXPECT_EQ(back_frame.owner, frame.owner);
    EXPECT_EQ(back_frame.now, frame.now);
    EXPECT_EQ(std::vector<std::byte>(back_frame.bytes().begin(), back_frame.bytes().end()),
              longest);
    EXPECT_EQ(unwrap(request_of(decode_record(records[4]).record)).kind,
              pipeline::RequestKind::CancelAll);
    EXPECT_EQ(unwrap(request_of(decode_record(records[5]).record)).kind,
              pipeline::RequestKind::Stop);
}

TEST(JournalFormatTest, TypedAccessorsRefuseOtherTypes) {
    const JournalRecord stop = decode_record(encoded([&](auto o) {
                                   return encode_request(o, 1, pipeline::make_stop_request(0));
                               })).record;
    EXPECT_FALSE(session_config_of(stop).has_value());
    EXPECT_FALSE(symbol_listing_of(stop).has_value());
    EXPECT_FALSE(risk_limits_of(stop).has_value());
    const JournalRecord setup = decode_record(encoded([&](auto o) {
                                    return encode_risk_limits(o, 1, 0, 1, protocol::RiskLimits{});
                                })).record;
    EXPECT_FALSE(request_of(setup).has_value());
}

TEST(JournalFormatTest, BytesAfterARecordAreIgnored) {
    auto r = encoded(
        [&](auto o) { return encode_request(o, 1, pipeline::make_cancel_all_request(1, 0)); });
    const std::size_t length = r.size();
    r.insert(r.end(), 10, std::byte{0xEE});
    const DecodedRecord d = decode_record(r);
    EXPECT_EQ(d.status, RecordStatus::Ok);
    EXPECT_EQ(d.length, length);
}

// What the encoders refuse

TEST(JournalFormatTest, EncodersRefuseAnOutputOneByteTooSmall) {
    const auto request = sample_frame(1, 0, {1, 2, 3});
    const protocol::OrderEntryConfig config;
    std::array<std::byte, kMaxRecordLength> buffer{};
    const std::span<std::byte> out(buffer);
    const std::size_t frame_length = encode_request(out, 1, request);
    EXPECT_EQ(encode_request(out.first(frame_length - 1), 1, request), 0U);
    const std::size_t start_length = encode_session_start(out, 1, 0, config);
    EXPECT_EQ(encode_session_start(out.first(start_length - 1), 1, 0, config), 0U);
    const std::size_t symbol_length = encode_add_symbol(out, 1, 0, sample_listing());
    EXPECT_EQ(encode_add_symbol(out.first(symbol_length - 1), 1, 0, sample_listing()), 0U);
    const std::size_t limits_length = encode_risk_limits(out, 1, 0, 1, {});
    EXPECT_EQ(encode_risk_limits(out.first(limits_length - 1), 1, 0, 1, {}), 0U);
    std::array<std::byte, kFileHeaderLength - 1> small_header{};
    EXPECT_EQ(encode_file_header(small_header, 1), 0U);
}

TEST(JournalFormatTest, EncodersRefuseValuesTheFormatCannotHold) {
    std::array<std::byte, kMaxRecordLength> out{};
    EXPECT_EQ(encode_request(out, 1, pipeline::make_frame_request(1, {}, 0)), 0U);
    pipeline::Request too_long = sample_frame(1, 0, {1});
    too_long.length = kMaxPayload + 1;
    EXPECT_EQ(encode_request(out, 1, too_long), 0U);
    pipeline::Request unknown = pipeline::make_stop_request(0);
    // A kind that does not exist, on purpose.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    unknown.kind = static_cast<pipeline::RequestKind>(9);
    EXPECT_EQ(encode_request(out, 1, unknown), 0U);

    protocol::OrderEntryConfig config;
    config.max_owners = 0;
    EXPECT_EQ(encode_session_start(out, 1, 0, config), 0U);
    config = {};
    config.max_live_orders = 0;
    EXPECT_EQ(encode_session_start(out, 1, 0, config), 0U);
    config = {};
    config.max_symbols = 0;
    EXPECT_EQ(encode_session_start(out, 1, 0, config), 0U);
    config.max_symbols = 65'536;  // One more than a SymbolId can number.
    EXPECT_EQ(encode_session_start(out, 1, 0, config), 0U);

    for (int broken = 0; broken < 4; ++broken) {
        SymbolListing listing = sample_listing();
        if (broken == 0) {
            listing.config.tick_size = 0;
        } else if (broken == 1) {
            listing.config.lot_size = 0;
        } else if (broken == 2) {
            listing.config.min_price = listing.config.max_price + 1;
        } else {
            listing.config.max_orders = 0;
        }
        EXPECT_EQ(encode_add_symbol(out, 1, 0, listing), 0U) << "case " << broken;
    }
    EXPECT_EQ(encode_risk_limits(out, 1, 0, kNoOwner, {}), 0U);
    EXPECT_EQ(encode_file_header(out, 0), 0U);
}

TEST(JournalFormatTest, BookConfigCheckAgreesWithValidateConfig) {
    std::vector<BookConfig> configs(6);
    configs[1].tick_size = 0;
    configs[2].lot_size = 0;
    configs[3].min_price = 5;
    configs[3].max_price = 4;
    configs[4].max_orders = 0;
    configs[5].min_price = 5;
    configs[5].max_price = 5;
    for (const BookConfig& c : configs) {
        bool throws = false;
        try {
            validate_config(c);
        } catch (const std::invalid_argument&) {
            throws = true;
        }
        EXPECT_EQ(book_config_is_valid(c), !throws);
    }
}

// What the decoder refuses

TEST(JournalFormatTest, EveryCutRecordIsIncomplete) {
    const std::vector<std::vector<std::byte>> records{
        encoded([&](auto o) { return encode_request(o, 1, sample_frame(1, 2, {1, 2, 3, 4})); }),
        encoded([&](auto o) { return encode_add_symbol(o, 1, 2, sample_listing()); }),
    };
    for (const auto& r : records) {
        for (std::size_t n = 0; n < r.size(); ++n) {
            EXPECT_EQ(decode_record(std::span<const std::byte>(r).first(n)).status,
                      RecordStatus::Incomplete)
                << "cut to " << n;
        }
    }
}

TEST(JournalFormatTest, LengthsOutsideTheLimitsAreBad) {
    auto r = encoded([&](auto o) { return encode_request(o, 1, sample_frame(1, 2, {1})); });
    for (const unsigned length : {0U, 1U, 27U, 76U, 0xFFFFU}) {
        protocol::store_le(std::span<std::byte>(r), 0, static_cast<std::uint16_t>(length));
        EXPECT_EQ(decode_record(r).status, RecordStatus::BadLength) << length;
    }
}

TEST(JournalFormatTest, ALengthThatDoesNotFitTheTypeIsBad) {
    // A Stop with one payload byte, and a Frame with none, each with a correct checksum.
    auto stop = encoded([&](auto o) { return encode_request(o, 1, sample_frame(0, 2, {1})); });
    stop[2] = static_cast<std::byte>('E');
    reseal(stop);
    EXPECT_EQ(decode_record(stop).status, RecordStatus::BadLength);

    auto frame = encoded(
        [&](auto o) { return encode_request(o, 1, pipeline::make_cancel_all_request(1, 2)); });
    frame[2] = static_cast<std::byte>('F');
    reseal(frame);
    EXPECT_EQ(decode_record(frame).status, RecordStatus::BadLength);

    auto limits = encoded([&](auto o) { return encode_request(o, 1, sample_frame(1, 2, {1, 2})); });
    limits[2] = static_cast<std::byte>('R');
    reseal(limits);
    EXPECT_EQ(decode_record(limits).status, RecordStatus::BadLength);
}

TEST(JournalFormatTest, AnyChangedByteAfterTheLengthBreaksTheChecksum) {
    const auto r = encoded([&](auto o) { return encode_add_symbol(o, 9, 8, sample_listing()); });
    for (std::size_t i = 2; i < r.size(); ++i) {
        auto broken = r;
        broken[i] ^= std::byte{0x01};
        EXPECT_EQ(decode_record(broken).status, RecordStatus::BadChecksum) << "byte " << i;
    }
}

TEST(JournalFormatTest, UnknownTypeIsBad) {
    auto r = encoded(
        [&](auto o) { return encode_request(o, 1, pipeline::make_cancel_all_request(1, 2)); });
    for (const unsigned letter : {0U, unsigned{'Z'}, unsigned{'f'}, 0xFFU}) {
        r[2] = static_cast<std::byte>(letter);
        reseal(r);
        EXPECT_EQ(decode_record(r).status, RecordStatus::BadType) << letter;
    }
}

TEST(JournalFormatTest, ReservedByteMustBeZero) {
    auto r = encoded(
        [&](auto o) { return encode_request(o, 1, pipeline::make_cancel_all_request(1, 2)); });
    r[3] = std::byte{1};
    reseal(r);
    EXPECT_EQ(decode_record(r).status, RecordStatus::BadReserved);
}

TEST(JournalFormatTest, OwnersAreCheckedPerType) {
    const auto set_owner = [](std::vector<std::byte> r, std::uint32_t owner) {
        protocol::store_le(std::span<std::byte>(r), 4, owner);
        reseal(r);
        return r;
    };
    const auto stop =
        encoded([&](auto o) { return encode_request(o, 1, pipeline::make_stop_request(2)); });
    EXPECT_EQ(decode_record(set_owner(stop, 1)).status, RecordStatus::BadOwner);
    const auto start = encoded(
        [&](auto o) { return encode_session_start(o, 1, 2, protocol::OrderEntryConfig{}); });
    EXPECT_EQ(decode_record(set_owner(start, 1)).status, RecordStatus::BadOwner);
    const auto symbol =
        encoded([&](auto o) { return encode_add_symbol(o, 1, 2, sample_listing()); });
    EXPECT_EQ(decode_record(set_owner(symbol, 1)).status, RecordStatus::BadOwner);
    const auto limits = encoded([&](auto o) { return encode_risk_limits(o, 1, 2, 4, {}); });
    EXPECT_EQ(decode_record(set_owner(limits, 0)).status, RecordStatus::BadOwner);
    // Frames and cancels may carry any owner, because replay must see what the matching
    // thread saw, and the order entry layer counts a bad owner itself.
    const auto frame =
        encoded([&](auto o) { return encode_request(o, 1, sample_frame(1, 2, {1})); });
    EXPECT_EQ(decode_record(set_owner(frame, 0)).status, RecordStatus::Ok);
    EXPECT_EQ(decode_record(set_owner(frame, 0xFFFFFFFFU)).status, RecordStatus::Ok);
}

TEST(JournalFormatTest, BadSetupPayloadsAreRefused) {
    const auto symbol =
        encoded([&](auto o) { return encode_add_symbol(o, 1, 2, sample_listing()); });
    const std::size_t p = kRecordHeaderLength;  // Where the payload starts.
    struct Change {
        const char* what;
        std::size_t offset;
        unsigned value;
    };
    // Each change breaks exactly one rule of the AddSymbol payload.
    const std::vector<Change> changes{
        {"ticker starts with a space", p + 2, ' '},
        {"ticker has a control character", p + 3, 0x01},
        {"ticker has a letter after the padding", p + 9, 'X'},
        {"self trade policy past the last one", p + 10, 4},
        {"reserved byte", p + 11, 1},
        {"lot size 0", p + 12, 0},
        {"max orders 0", p + 17, 0},
        {"tick size 0", p + 20, 0},
        {"min price above max price", p + 35, 0x7F},
    };
    for (const Change& c : changes) {
        auto r = symbol;
        r[c.offset] = static_cast<std::byte>(c.value);
        reseal(r);
        EXPECT_EQ(decode_record(r).status, RecordStatus::BadPayload) << c.what;
    }

    const auto start = encoded(
        [&](auto o) { return encode_session_start(o, 1, 2, protocol::OrderEntryConfig{}); });
    for (const std::size_t field : {p, p + 4, p + 8}) {
        auto r = start;
        protocol::store_le(std::span<std::byte>(r), field, std::uint32_t{0});
        reseal(r);
        EXPECT_EQ(decode_record(r).status, RecordStatus::BadPayload) << "field at " << field;
    }
    auto many = start;
    protocol::store_le(std::span<std::byte>(many), p + 8, std::uint32_t{65'536});
    reseal(many);
    EXPECT_EQ(decode_record(many).status, RecordStatus::BadPayload);
}

// File header

TEST(JournalFormatTest, HeaderDecodes) {
    std::array<std::byte, kFileHeaderLength + 3> out{};
    ASSERT_EQ(encode_file_header(out, 42), kFileHeaderLength);
    const DecodedHeader h = decode_file_header(out);
    EXPECT_EQ(h.status, HeaderStatus::Ok);
    EXPECT_EQ(h.first_sequence, 42U);
}

TEST(JournalFormatTest, EveryHeaderFieldIsChecked) {
    std::vector<std::byte> good(kFileHeaderLength);
    ASSERT_EQ(encode_file_header(good, 1), kFileHeaderLength);
    for (std::size_t n = 0; n < kFileHeaderLength; ++n) {
        EXPECT_EQ(decode_file_header(std::span<const std::byte>(good).first(n)).status,
                  HeaderStatus::Incomplete);
    }
    struct Change {
        std::size_t offset;
        unsigned value;
        HeaderStatus expected;
        bool reseal;
    };
    const std::vector<Change> changes{
        {0, 'X', HeaderStatus::BadMagic, false},      {7, 'j', HeaderStatus::BadMagic, true},
        {8, 2, HeaderStatus::BadVersion, true},       {10, 31, HeaderStatus::BadLength, true},
        {12, 1, HeaderStatus::BadReserved, true},     {24, 1, HeaderStatus::BadReserved, true},
        {16, 0, HeaderStatus::BadSequence, true},     {20, 1, HeaderStatus::BadChecksum, false},
        {29, 0xAA, HeaderStatus::BadChecksum, false},
    };
    for (const Change& c : changes) {
        auto h = good;
        h[c.offset] = static_cast<std::byte>(c.value);
        if (c.reseal) {
            reseal_header(h);
        }
        EXPECT_EQ(decode_file_header(h).status, c.expected) << "offset " << c.offset;
    }
}

}  // namespace
}  // namespace litbook::journal
