// Fuzz target for the market data feed.
//
// The first input byte picks one of two modes with its lowest bit.
//
// Mode 0 feeds raw bytes to a feed handler. The rest of the input is a list of records,
// each a stream byte, a 2-byte length and that many bytes of packet. The handler must
// never crash or read outside a packet (the sanitizers catch that), every book must keep
// its invariants, and every packet that passes check_packet must hold only messages that
// encode back to exactly their own bytes.
//
// Mode 1 is a differential test of recovery. The input drives a publisher with level
// updates, trades, flushes and ticks, and its packets reach a handler through a
// LossyChannel whose loss rates and seed come from the input too. At the end the channel
// turns lossless, a heartbeat interval passes and the publisher sends two full snapshot cycles,
// after which the handler must be live with exactly the publisher's books. The target aborts if any
// check fails.

#include "litbook/feed/feed_codec.hpp"
#include "litbook/feed/feed_handler.hpp"
#include "litbook/feed/feed_publisher.hpp"
#include "litbook/feed/mold.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

#include "feed/lossy_channel.hpp"
#include "fuzz_target.hpp"
#include "support/unwrap.hpp"

namespace litbook::fuzz {

namespace {

void check(bool ok) {
    if (!ok) {
        std::abort();
    }
}

constexpr std::size_t kSymbols = 2;
constexpr std::size_t kLevels = 64;

feed::FeedHandlerConfig handler_config(std::size_t buffer) {
    return feed::FeedHandlerConfig{.session = "FUZZ",
                                   .max_symbols = 4,
                                   .max_levels_per_side = kLevels,
                                   .buffer_capacity = buffer};
}

void check_books(const feed::FeedHandler& handler) {
    for (feed::Locate l = 0; l < 4; ++l) {
        check(handler.book(l)->check_invariants());
    }
}

// Every message of a packet that check_packet accepts must re-encode to the same bytes.
void check_canonical(std::span<const std::byte> packet) {
    if (feed::check_packet(packet).status != feed::PacketStatus::Ok) {
        return;
    }
    feed::BlockReader reader(packet);
    std::span<const std::byte> bytes;
    std::array<std::byte, feed::kMaxMessageLength> out{};
    while (reader.next(bytes)) {
        const feed::FeedDecoded decoded = feed::decode_message(bytes);
        if (!decoded.message) {
            std::abort();
        }
        const std::size_t n = feed::encode(*decoded.message, out);
        check(n == bytes.size());
        check(std::memcmp(out.data(), bytes.data(), n) == 0);
    }
}

void raw_packets(std::span<const std::uint8_t> input) {
    feed::FeedHandler handler(handler_config(32));
    std::size_t at = 0;
    while (input.size() - at >= 3) {
        const bool snapshot = (input[at] & 1U) != 0;
        const std::size_t wanted = (static_cast<std::size_t>(input[at + 1]) << 8U) | input[at + 2];
        at += 3;
        const std::size_t length = std::min(wanted, input.size() - at);
        const auto packet = std::as_bytes(input.subspan(at, length));
        at += length;
        check_canonical(packet);
        if (snapshot) {
            (void)handler.on_snapshot(packet);
        } else {
            (void)handler.on_incremental(packet);
        }
        check_books(handler);
    }
}

void lossy_market(std::span<const std::uint8_t> input) {
    if (input.size() < 4) {
        return;
    }
    // The first four bytes set the loss rates, the snapshot lag and the buffer size.
    const feed::testing::LossRates rates{.drop = input[0] % 200U,
                                         .duplicate = input[1] % 100U,
                                         .hold = input[2] % 100U,
                                         .snapshot_lag = input[3] % 4U};
    const std::size_t buffer = (input[3] & 0x80U) != 0 ? 8 : 1024;
    input = input.subspan(4);

    feed::FeedHandler handler(handler_config(buffer));
    feed::testing::LossyChannel channel(handler, input.size(), rates);
    feed::FeedPublisherConfig config;
    config.session = "FUZZ";
    config.snapshot_spacing = 1'000;
    config.max_packet_length = 200;  // Small packets, so snapshots span several.
    feed::FeedPublisher publisher(channel, config);
    publisher.add_symbol(
        {.symbol = litbook::testing::unwrap(make_ticker("AAA")), .max_levels_per_side = kLevels});
    publisher.add_symbol(
        {.symbol = litbook::testing::unwrap(make_ticker("BBB")), .max_levels_per_side = kLevels});

    feed::Timestamp now = 0;
    publisher.start(now);
    // Each record is three bytes, an action and two arguments.
    for (std::size_t at = 0; at + 3 <= input.size(); at += 3) {
        const std::uint8_t action = input[at];
        const std::uint8_t a = input[at + 1];
        const std::uint8_t b = input[at + 2];
        pipeline::MarketEvent e;
        e.now = now;
        e.symbol = static_cast<SymbolId>(a % kSymbols);
        e.side = (a & 0x80U) != 0 ? Side::Sell : Side::Buy;
        e.price = static_cast<Price>(1 + (a >> 1U) % 32U);
        switch (action % 8U) {
            case 0:
            case 1:
            case 2:
            case 3:
                // A level update. A quantity byte of 0 removes the level.
                e.kind = pipeline::MarketEventKind::LevelUpdate;
                e.qty = b;
                e.order_count = b == 0 ? 0 : 1 + b % 4U;
                publisher.on_event(e);
                break;
            case 4:
                e.kind = pipeline::MarketEventKind::Trade;
                e.qty = 1 + b;
                e.match_id = at;
                publisher.on_event(e);
                break;
            case 5:
                publisher.flush(now);
                break;
            default:
                now += 1 + b * 20U;
                publisher.tick(now);
                break;
        }
        check_books(handler);
    }

    // A lost packet at the very end shows up only through the next heartbeat, so a quiet
    // second passes before the snapshot cycles.
    channel.set_lossless();
    publisher.flush(now);
    now += config.heartbeat_interval;
    publisher.tick(now);
    for (std::size_t i = 0; i < 4 * kSymbols; ++i) {
        now += config.snapshot_spacing;
        publisher.tick(now);
    }
    check(!handler.recovering());
    check(handler.next_expected() == publisher.next_sequence(feed::FeedStream::Incremental));
    for (feed::Locate l = 0; l < kSymbols; ++l) {
        check(handler.live(l));
        check(*handler.book(l) == publisher.copy(l));
    }
    check(channel.stats().not_ok == 0);
}

}  // namespace

int run_one(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::span<const std::uint8_t> input(data + 1, size - 1);
    if ((data[0] & 1U) == 0) {
        raw_packets(input);
    } else {
        lossy_market(input);
    }
    return 0;
}

}  // namespace litbook::fuzz
