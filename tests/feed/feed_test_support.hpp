#pragma once

#include "litbook/feed/feed_codec.hpp"
#include "litbook/feed/feed_handler.hpp"
#include "litbook/feed/feed_publisher.hpp"
#include "litbook/feed/mold.hpp"
#include "litbook/pipeline/messages.hpp"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "support/unwrap.hpp"

namespace litbook::feed::testing {

using Bytes = std::vector<std::byte>;

// One packet as a sink received it.
struct SentPacket {
    FeedStream stream;
    Bytes bytes;
};

// Keeps every packet the publisher sends, in order.
class RecordingSink final : public FeedSink {
public:
    void send(FeedStream stream, std::span<const std::byte> packet) override {
        packets.push_back(
            SentPacket{.stream = stream, .bytes = Bytes(packet.begin(), packet.end())});
    }

    [[nodiscard]] std::vector<Bytes> of(FeedStream stream) const {
        std::vector<Bytes> out;
        for (const SentPacket& p : packets) {
            if (p.stream == stream) {
                out.push_back(p.bytes);
            }
        }
        return out;
    }

    std::vector<SentPacket> packets;
};

// A packet taken apart for tests. It throws if the packet does not pass check_packet,
// which makes a bad packet fail the test that reads it.
struct DecodedPacket {
    PacketHeader header;
    std::vector<FeedMessage> messages;
};

inline DecodedPacket decode_packet(std::span<const std::byte> packet) {
    const CheckedPacket checked = check_packet(packet);
    if (checked.status != PacketStatus::Ok) {
        throw std::runtime_error("packet does not pass check_packet");
    }
    DecodedPacket out{.header = checked.header, .messages = {}};
    BlockReader reader(packet);
    std::span<const std::byte> bytes;
    while (reader.next(bytes)) {
        out.messages.push_back(litbook::testing::unwrap(decode_message(bytes).message));
    }
    return out;
}

// Every message of a stream in order, across packets.
inline std::vector<FeedMessage> messages_of(const RecordingSink& sink, FeedStream stream) {
    std::vector<FeedMessage> out;
    for (const Bytes& packet : sink.of(stream)) {
        const DecodedPacket decoded = decode_packet(packet);
        out.insert(out.end(), decoded.messages.begin(), decoded.messages.end());
    }
    return out;
}

inline Ticker ticker(std::string_view name) {
    return litbook::testing::unwrap(make_ticker(name));
}

inline SessionName unwrap_session(std::string_view name) {
    return litbook::testing::unwrap(make_session_name(name));
}

inline pipeline::MarketEvent level_event(Locate locate, Side side, Price price, AggregateQty qty,
                                         std::uint32_t count, Timestamp now = 0) {
    pipeline::MarketEvent e;
    e.now = now;
    e.kind = pipeline::MarketEventKind::LevelUpdate;
    e.side = side;
    e.symbol = locate;
    e.order_count = count;
    e.price = price;
    e.qty = qty;
    return e;
}

inline pipeline::MarketEvent trade_event(Locate locate, Side side, Price price, AggregateQty qty,
                                         MatchId match, Timestamp now = 0) {
    pipeline::MarketEvent e;
    e.now = now;
    e.kind = pipeline::MarketEventKind::Trade;
    e.side = side;
    e.symbol = locate;
    e.price = price;
    e.qty = qty;
    e.match_id = match;
    return e;
}

// True when a level copy is exactly an exchange book's levels on both sides. Book is any
// order book with depth(side, max_levels), such as SortedOrderBook.
template <class Book>
bool copy_matches_book(const LevelBook& copy, const Book& book) {
    for (const Side side : {Side::Buy, Side::Sell}) {
        const auto depth = book.depth(side, 100'000);
        if (depth.size() != copy.level_count(side)) {
            return false;
        }
        for (std::size_t rank = 0; rank < depth.size(); ++rank) {
            const PriceLevel& level = copy.level(side, rank);
            if (level.price != depth[rank].price || level.total_qty != depth[rank].total_qty ||
                level.order_count != depth[rank].order_count) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace litbook::feed::testing
