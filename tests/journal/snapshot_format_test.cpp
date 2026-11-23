// Tests for the snapshot file format: the byte layout, round trips of real states, and
// every way a damaged or foreign file is refused.

#include "litbook/journal/snapshot_format.hpp"

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/crc32c.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/journal/snapshot_store.hpp"
#include "litbook/protocol/byte_order.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "journal/replay_test_support.hpp"

namespace litbook::journal {
namespace {

// A small snapshot with one of everything, whose byte offsets the tests know.
//   header 0..39, setup count 40, Session Start record 44..61, replies 62, last
//   UserRefNum 70, scalars 74..129, book count 130, book 134..145, order count 146,
//   order 150..184, resting count 185, resting order 189..214, checksum 215..218.
Snapshot small_snapshot() {
    protocol::OrderEntryConfig config;
    config.max_owners = 1;
    Snapshot s;
    s.as_of = 77;
    s.as_of_time = 123'456;
    s.setup.push_back(session_start_record(config));
    s.replies = {0, 5};
    s.state.last_order_ref = 9;
    s.state.last_match_id = 4;
    s.state.stats = {.malformed = 1,
                     .bad_owner = 2,
                     .stale_user_ref = 3,
                     .unknown_order = 4,
                     .no_effect_cancel = 5};
    s.state.last_user_ref = {0, 7};
    s.state.books = {protocol::BookState{.next_match_id = 3, .order_count = 1}};
    s.state.orders = {protocol::LiveOrderState{.ref = 9,
                                               .owner = 1,
                                               .user_ref = 7,
                                               .symbol = 0,
                                               .side = Side::Sell,
                                               .price = -500,
                                               .open = 30,
                                               .executed = 20}};
    s.state.resting = {RestingOrder{.id = 9,
                                    .owner = 1,
                                    .side = Side::Sell,
                                    .price = -500,
                                    .remaining = 30,
                                    .post_only = true}};
    return s;
}

constexpr std::size_t kSmallLength = 219;

std::vector<std::byte> encoded(const Snapshot& s) {
    std::vector<std::byte> out(snapshot_size(s));
    EXPECT_EQ(encode_snapshot(s, out), out.size());
    return out;
}

// Writes a fresh checksum after changing bytes, so the decoder reaches the field checks.
void reseal(std::vector<std::byte>& bytes) {
    const std::size_t crc_at = bytes.size() - kChecksumLength;
    protocol::store_le<std::uint32_t>(bytes, crc_at,
                                      crc32c(std::span<const std::byte>(bytes).first(crc_at)));
}

SnapshotStatus decode(const std::vector<std::byte>& bytes) {
    Snapshot out;
    return decode_snapshot(bytes, out);
}

template <class T>
T field(const std::vector<std::byte>& bytes, std::size_t offset) {
    return protocol::load_le<T>(bytes, offset);
}

// A real state, from a random stream through an order entry layer.
Snapshot real_snapshot() {
    testing::RecordingReplies replies;
    protocol::OrderEntry<SortedOrderBook> orders(replies, testing::replay_order_entry_config());
    testing::set_up_exchange(orders);
    const std::vector<pipeline::Request> stream = testing::replay_stream(20261122, 3000);
    for (const pipeline::Request& request : stream) {
        testing::apply_request(orders, request);
    }
    Snapshot s;
    s.as_of = 3000;
    s.as_of_time = stream.back().now;
    s.setup.push_back(session_start_record(testing::replay_order_entry_config()));
    s.setup.push_back(risk_limits_record(testing::kLimitedOwner, testing::kLimitedRisk));
    s.replies.assign(testing::kReplayOwners + 1, 0);
    for (const testing::RecordedReply& reply : replies.replies) {
        ++s.replies.at(reply.owner);
    }
    orders.capture(s.state);
    return s;
}

TEST(SnapshotFormatTest, TheHeaderAndFieldsSitWhereTheLayoutSays) {
    const std::vector<std::byte> bytes = encoded(small_snapshot());
    ASSERT_EQ(bytes.size(), kSmallLength);
    for (std::size_t i = 0; i < kSnapshotMagic.size(); ++i) {
        EXPECT_EQ(bytes[i], static_cast<std::byte>(kSnapshotMagic[i]));
    }
    EXPECT_EQ(field<std::uint16_t>(bytes, 8), kSnapshotVersion);
    EXPECT_EQ(field<std::uint16_t>(bytes, 10), 0U);
    EXPECT_EQ(field<std::uint32_t>(bytes, 12), 1U);  // One account.
    EXPECT_EQ(field<std::uint64_t>(bytes, 16), 77U);
    EXPECT_EQ(field<std::uint64_t>(bytes, 24), 123'456U);
    EXPECT_EQ(field<std::uint32_t>(bytes, 32), kSmallLength - kSnapshotHeaderLength - 4);
    EXPECT_EQ(field<std::uint32_t>(bytes, 36), 0U);

    EXPECT_EQ(field<std::uint32_t>(bytes, 40), 1U);  // One setup record.
    EXPECT_EQ(field<std::uint8_t>(bytes, 44), static_cast<std::uint8_t>(RecordType::SessionStart));
    EXPECT_EQ(field<std::uint8_t>(bytes, 49), kSessionStartPayload);
    EXPECT_EQ(field<std::uint64_t>(bytes, 62), 5U);   // Replies of account 1.
    EXPECT_EQ(field<std::uint32_t>(bytes, 70), 7U);   // Its last UserRefNum.
    EXPECT_EQ(field<std::uint64_t>(bytes, 74), 9U);   // Last order ref.
    EXPECT_EQ(field<std::uint64_t>(bytes, 82), 4U);   // Last match.
    EXPECT_EQ(field<std::uint64_t>(bytes, 122), 5U);  // The fifth stat.
    EXPECT_EQ(field<std::uint32_t>(bytes, 130), 1U);  // One book.
    EXPECT_EQ(field<std::uint64_t>(bytes, 134), 3U);
    EXPECT_EQ(field<std::uint32_t>(bytes, 142), 1U);
    EXPECT_EQ(field<std::uint32_t>(bytes, 146), 1U);  // One live order.
    EXPECT_EQ(field<std::uint64_t>(bytes, 150), 9U);
    EXPECT_EQ(field<std::uint8_t>(bytes, 168), 'S');
    EXPECT_EQ(static_cast<std::int64_t>(field<std::uint64_t>(bytes, 169)), -500);
    EXPECT_EQ(field<std::uint32_t>(bytes, 177), 30U);
    EXPECT_EQ(field<std::uint32_t>(bytes, 181), 20U);
    EXPECT_EQ(field<std::uint32_t>(bytes, 185), 1U);  // One resting order.
    EXPECT_EQ(field<std::uint64_t>(bytes, 189), 9U);
    EXPECT_EQ(field<std::uint8_t>(bytes, 201), 'S');
    EXPECT_EQ(field<std::uint32_t>(bytes, 210), 30U);
    EXPECT_EQ(field<std::uint8_t>(bytes, 214), 1U);  // Post-only.
    const std::span<const std::byte> all(bytes);
    EXPECT_EQ(field<std::uint32_t>(bytes, 215), crc32c(all.first(215)));
}

TEST(SnapshotFormatTest, ASmallSnapshotRoundTrips) {
    const Snapshot s = small_snapshot();
    const std::vector<std::byte> bytes = encoded(s);
    Snapshot out;
    ASSERT_EQ(decode_snapshot(bytes, out), SnapshotStatus::Ok);
    EXPECT_EQ(out, s);
    EXPECT_EQ(encoded(out), bytes);
}

TEST(SnapshotFormatTest, ARealStateRoundTripsByteForByte) {
    const Snapshot s = real_snapshot();
    ASSERT_GT(s.state.orders.size(), 20U);
    const std::vector<std::byte> bytes = encoded(s);
    EXPECT_EQ(bytes.size(), snapshot_size(s));
    Snapshot out;
    ASSERT_EQ(decode_snapshot(bytes, out), SnapshotStatus::Ok);
    EXPECT_EQ(out, s);
    EXPECT_EQ(encoded(out), bytes);
}

// The header's Body Length must say exactly where the checksum starts. Bytes between the
// body and the checksum, with a checksum that covers them, would otherwise decode and
// encode back into different bytes.
TEST(SnapshotFormatTest, BytesBetweenTheBodyAndTheChecksumAreRefused) {
    const std::vector<std::byte> good = encoded(real_snapshot());
    std::vector<std::byte> longer(good.begin(), good.end() - kChecksumLength);
    for (int i = 0; i < 4; ++i) {
        longer.push_back(std::byte{0});
    }
    // Body Length stays what it was. Only the checksum is made to match again.
    const std::uint32_t crc = crc32c(longer);
    longer.resize(longer.size() + kChecksumLength);
    protocol::store_le(std::span<std::byte>(longer), longer.size() - kChecksumLength, crc);
    Snapshot out;
    EXPECT_EQ(decode_snapshot(longer, out), SnapshotStatus::BadLength);
}

TEST(SnapshotFormatTest, EncodeRefusesASnapshotThatBreaksAnInvariant) {
    std::vector<Snapshot> bad(6, small_snapshot());
    bad[0].replies = {0};                                       // No account.
    bad[1].replies = {0, 5, 6};                                 // Two accounts, one last ref.
    bad[2].replies[0] = 1;                                      // Entry 0 used.
    bad[3].state.last_user_ref[0] = 1;                          // Entry 0 used.
    bad[4].setup[0].type = RecordType::Frame;                   // Not a setup record.
    bad[5].setup[0].payload_length = kSessionStartPayload + 1;  // Wrong payload length.
    for (std::size_t i = 0; i < bad.size(); ++i) {
        EXPECT_EQ(snapshot_size(bad[i]), 0U) << "case " << i;
        std::vector<std::byte> out(1024, std::byte{0xAA});
        EXPECT_EQ(encode_snapshot(bad[i], out), 0U) << "case " << i;
        EXPECT_EQ(out, std::vector<std::byte>(1024, std::byte{0xAA})) << "case " << i;
    }
}

TEST(SnapshotFormatTest, EncodeRefusesABufferThatIsTooSmallAndWritesNothing) {
    std::vector<std::byte> out(kSmallLength - 1, std::byte{0xAA});
    EXPECT_EQ(encode_snapshot(small_snapshot(), out), 0U);
    EXPECT_EQ(out, std::vector<std::byte>(kSmallLength - 1, std::byte{0xAA}));
}

TEST(SnapshotFormatTest, EveryCutIsRefused) {
    const std::vector<std::byte> bytes = encoded(small_snapshot());
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        const std::vector<std::byte> cut(bytes.begin(),
                                         bytes.begin() + static_cast<std::ptrdiff_t>(length));
        const SnapshotStatus status = decode(cut);
        EXPECT_TRUE(status == SnapshotStatus::TooShort || status == SnapshotStatus::BadChecksum)
            << "length " << length << " gave " << status;
    }
}

TEST(SnapshotFormatTest, EveryChangedByteIsRefused) {
    const std::vector<std::byte> bytes = encoded(small_snapshot());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::vector<std::byte> changed = bytes;
        changed[i] ^= std::byte{0x01};
        const SnapshotStatus status = decode(changed);
        EXPECT_NE(status, SnapshotStatus::Ok) << "byte " << i;
        if (i >= 10) {
            EXPECT_EQ(status, SnapshotStatus::BadChecksum) << "byte " << i;
        }
    }
}

TEST(SnapshotFormatTest, EachFieldRuleHasItsOwnStatus) {
    const std::vector<std::byte> good = encoded(small_snapshot());
    struct Case {
        std::size_t offset;
        std::byte value;
        SnapshotStatus expected;
    };
    const std::vector<Case> cases{
        {0, std::byte{'X'}, SnapshotStatus::BadMagic},
        {8, std::byte{2}, SnapshotStatus::BadVersion},
        {10, std::byte{1}, SnapshotStatus::BadHeader},  // Reserved 16 bits.
        {36, std::byte{1}, SnapshotStatus::BadHeader},  // Reserved 32 bits.
        {12, std::byte{0}, SnapshotStatus::BadHeader},  // No accounts.
        {32, std::byte{0}, SnapshotStatus::BadLength},  // Body length wrong.
        {44, std::byte{'F'}, SnapshotStatus::BadSetupRecord},
        {49, std::byte{11}, SnapshotStatus::BadSetupRecord},
        {168, std::byte{'X'}, SnapshotStatus::BadField},  // Side of a live order.
        {201, std::byte{'b'}, SnapshotStatus::BadField},  // Side of a resting order.
        {214, std::byte{2}, SnapshotStatus::BadField},    // Post-only flag.
        {146, std::byte{2}, SnapshotStatus::BadLength},   // One more live order than fits.
        {130, std::byte{0}, SnapshotStatus::BadLength},   // No books, so bytes are left over.
    };
    for (const Case& c : cases) {
        std::vector<std::byte> bytes = good;
        bytes[c.offset] = c.value;
        if (c.expected != SnapshotStatus::BadMagic && c.expected != SnapshotStatus::BadVersion) {
            reseal(bytes);
        }
        EXPECT_EQ(decode(bytes), c.expected) << "offset " << c.offset;
    }
}

TEST(SnapshotFormatTest, AHugeCountIsRefusedBeforeAnythingIsAllocated) {
    std::vector<std::byte> bytes = encoded(small_snapshot());
    protocol::store_le<std::uint32_t>(bytes, 185, 0xFFFF'FFFFU);  // Resting order count.
    reseal(bytes);
    EXPECT_EQ(decode(bytes), SnapshotStatus::BadLength);
    bytes = encoded(small_snapshot());
    protocol::store_le<std::uint32_t>(bytes, 12, 0xFFFF'FFFFU);  // Accounts.
    reseal(bytes);
    EXPECT_EQ(decode(bytes), SnapshotStatus::BadLength);
}

TEST(SnapshotFormatTest, ADecodedRealSnapshotRestoresIntoAFreshLayer) {
    const Snapshot s = real_snapshot();
    Snapshot out;
    ASSERT_EQ(decode_snapshot(encoded(s), out), SnapshotStatus::Ok);
    testing::RecordingReplies replies;
    protocol::OrderEntry<FastOrderBook> orders(replies, testing::replay_order_entry_config());
    testing::set_up_exchange(orders);
    orders.restore(out.state);
    protocol::OrderEntryState again;
    orders.capture(again);
    EXPECT_EQ(again, s.state);
}

TEST(SnapshotFormatTest, StatusesPrintTheirNames) {
    std::ostringstream os;
    os << SnapshotStatus::BadChecksum << ' ' << SnapshotStatus::BadSetupRecord;
    EXPECT_EQ(os.str(), "BadChecksum BadSetupRecord");
}

// The fuzz seeds were written by a separate Python version of the format, made from the
// layout in docs/JOURNAL.md. The C++ decoder must agree with it on every file, which
// checks the format and its description against each other.
LoadedSnapshot read_seed(const std::string& name) {
    // Each seed starts with the fuzz mode byte, which is not part of the snapshot.
    std::ifstream in(std::string(LITBOOK_SNAPSHOT_SEEDS) + "/" + name, std::ios::binary);
    EXPECT_TRUE(in.good()) << name;
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(raw.empty()) << name;
    const auto bytes = std::as_bytes(std::span<const char>(raw)).subspan(1);
    LoadedSnapshot loaded;
    loaded.status = decode_snapshot(bytes, loaded.snapshot);
    return loaded;
}

TEST(SnapshotFormatTest, SeedsFromTheIndependentEncoderDecodeAndRestore) {
    for (const char* name : {"raw-empty.bin", "raw-two-orders.bin", "raw-two-stocks.bin"}) {
        SCOPED_TRACE(name);
        const LoadedSnapshot loaded = read_seed(name);
        ASSERT_EQ(loaded.status, SnapshotStatus::Ok);
        testing::RecordingReplies replies;
        Replayer<SortedOrderBook> replayer(replies);
        ASSERT_EQ(replayer.start_from(loaded.snapshot), ReplayStatus::Ok);
        EXPECT_TRUE(replayer.order_entry().engine().check_invariants());
    }
    const LoadedSnapshot two = read_seed("raw-two-orders.bin");
    EXPECT_EQ(two.snapshot.as_of, 9U);
    EXPECT_EQ(two.snapshot.as_of_time, 777U);
    EXPECT_EQ(two.snapshot.replies, (std::vector<std::uint64_t>{0, 1, 4}));
    ASSERT_EQ(two.snapshot.state.orders.size(), 2U);
    EXPECT_EQ(two.snapshot.state.orders[1].executed, 10U);
    ASSERT_EQ(two.snapshot.state.resting.size(), 2U);
    EXPECT_TRUE(two.snapshot.state.resting[1].post_only);
}

TEST(SnapshotFormatTest, DamagedSeedsFromTheIndependentEncoderAreRefusedForTheirReason) {
    EXPECT_EQ(read_seed("raw-bad-side.bin").status, SnapshotStatus::BadField);
    EXPECT_EQ(read_seed("raw-huge-count.bin").status, SnapshotStatus::BadLength);
    EXPECT_EQ(read_seed("raw-cut.bin").status, SnapshotStatus::BadChecksum);
    EXPECT_EQ(read_seed("raw-bad-checksum.bin").status, SnapshotStatus::BadChecksum);
    // Well formed, but its live order and its resting order disagree.
    const LoadedSnapshot refused = read_seed("raw-restore-refused.bin");
    ASSERT_EQ(refused.status, SnapshotStatus::Ok);
    testing::RecordingReplies replies;
    Replayer<SortedOrderBook> replayer(replies);
    EXPECT_EQ(replayer.start_from(refused.snapshot), ReplayStatus::SnapshotRefused);
}

}  // namespace
}  // namespace litbook::journal
