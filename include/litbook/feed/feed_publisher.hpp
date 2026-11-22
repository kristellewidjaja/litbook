#pragma once

#include "litbook/feed/feed_messages.hpp"
#include "litbook/feed/level_book.hpp"
#include "litbook/feed/mold.hpp"
#include "litbook/pipeline/messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace litbook::feed {

// The two streams of the feed, each sent to its own multicast group.
enum class FeedStream : std::uint8_t { Incremental, Snapshot };
inline constexpr std::size_t kStreamCount = 2;

// Where the publisher's finished packets go. In Phase 8b this is a UDP socket. Tests use a
// sink that keeps the packets or drops some of them on purpose.
class FeedSink {
public:
    virtual ~FeedSink() = default;
    virtual void send(FeedStream stream, std::span<const std::byte> packet) = 0;

protected:
    FeedSink() = default;
    FeedSink(const FeedSink&) = default;
    FeedSink(FeedSink&&) = default;
    FeedSink& operator=(const FeedSink&) = default;
    FeedSink& operator=(FeedSink&&) = default;
};

// One stock as the feed lists it. max_levels_per_side sizes the publisher's copy of its
// book. It must be at least as many levels as the book can ever have on a side.
struct FeedSymbol {
    Ticker symbol;
    Price tick_size = 1;
    Qty lot_size = 1;
    std::size_t max_levels_per_side = 1024;
};

struct FeedPublisherConfig {
    std::string session = "1";  // 1 to 10 letters and digits.
    std::size_t max_packet_length = kDefaultPacketLength;
    std::size_t max_symbols = 1024;
    // A stream that sent nothing for this long sends a heartbeat.
    Timestamp heartbeat_interval = 1'000'000'000;
    // The time from one stock's snapshot to the next one's. A whole cycle over N stocks
    // takes N times this.
    Timestamp snapshot_spacing = 1'000'000;
    // How many end of session packets each stream sends at the end.
    int end_of_session_packets = 3;
};

struct FeedPublisherStats {
    std::array<std::uint64_t, kStreamCount> packets{};
    std::array<std::uint64_t, kStreamCount> messages{};
    std::array<std::uint64_t, kStreamCount> heartbeats{};
    std::uint64_t events = 0;          // Trades and level updates published.
    std::uint64_t snapshots = 0;       // Whole stock snapshots sent.
    std::uint64_t ignored = 0;         // Events before start() or after end_session().
    std::uint64_t unknown_symbol = 0;  // Events for a locate that was never listed.
    std::uint64_t copy_full = 0;       // Level updates that did not fit in the copy.
    std::uint64_t unencodable = 0;     // Events that could not be put on the wire.
};

// Turns the matching thread's market events into the two MoldUDP64 streams of the feed.
// It has no sockets and no clock of its own. The caller passes the time to start, flush,
// tick and end_session, and every packet goes to the FeedSink.
//
// The incremental stream gets one message per event, packed into packets until the next
// message would not fit or the caller flushes. The publisher also keeps its own copy of
// every stock's levels, built from the same events, so a snapshot of a stock is exactly
// the state after incremental message `as_of`, without asking the matching thread.
//
// Invariants. Sequence numbers on each stream go up by one per message, from 1. Before a
// snapshot is sent, every incremental message up to its as_of has been handed to the sink.
// Each copy holds the levels that the incremental stream has described so far, unless
// copy_full says a level did not fit. Nothing allocates after add_symbol.
class FeedPublisher {
public:
    // Throws std::invalid_argument for a bad session name, packet limit, symbol count or
    // heartbeat interval.
    FeedPublisher(FeedSink& sink, const FeedPublisherConfig& config);

    // Lists a stock and returns its locate (0, 1, 2, ...). Only before start(). Throws
    // std::logic_error after start, std::length_error when max_symbols are listed, and
    // std::invalid_argument for a bad tick size, lot size or level limit.
    Locate add_symbol(const FeedSymbol& symbol);

    // Recovery calls, only before start(). Both throw std::logic_error after start.
    //
    // restore() applies a level update to the stock's copy without publishing anything,
    // so after an exchange rebuilt its books from the journal, the first snapshots of the
    // new session describe those books. Trades and Stop events are ignored, and so is an
    // event for a stock that is not listed. A level that does not fit marks the copy as
    // wrong, exactly as on_event does.
    void restore(const pipeline::MarketEvent& event);
    // Gives the feed a new session name. Receivers of the old session see a new session
    // start and fill their books from the snapshot stream. Throws std::invalid_argument
    // for a name that is not 1 to 10 letters and digits.
    void rename_session(const std::string& session);

    // Sends Start of Messages and a Symbol Directory for every stock. The first snapshot
    // goes out at the first tick.
    void start(Timestamp now);

    // Publishes one trade or level update. A Stop event is ignored here, the caller
    // decides what stopping means.
    void on_event(const pipeline::MarketEvent& event);

    // Sends the incremental packet being built, if it has any messages. The publisher
    // thread calls this whenever its event queue runs empty.
    void flush(Timestamp now);

    // Sends a heartbeat on a quiet stream and the next stock's snapshot when it is due.
    void tick(Timestamp now);

    // The earliest time at which tick() has something to send, either the next snapshot or
    // a heartbeat on a quiet stream. A publisher thread that has no events can sleep until
    // then. Before start() and after end_session() nothing is ever due, so this is the
    // largest Timestamp.
    [[nodiscard]] Timestamp next_deadline() const noexcept;

    // Sends End of Messages and then the end of session packets on both streams. Events
    // after this are ignored.
    void end_session(Timestamp now);

    [[nodiscard]] std::size_t symbol_count() const noexcept { return symbols_.size(); }
    [[nodiscard]] const std::string& session() const noexcept { return config_.session; }
    // The publisher's copy of a stock's levels. The caller makes sure locate is listed.
    [[nodiscard]] const LevelBook& copy(Locate locate) const noexcept {
        return symbols_[locate].copy;
    }
    // The sequence number the stream will give its next message.
    [[nodiscard]] Sequence next_sequence(FeedStream stream) const noexcept {
        return stream_of(stream).next;
    }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] const FeedPublisherStats& stats() const noexcept { return stats_; }

private:
    struct Symbol {
        SymbolDirectory directory;
        LevelBook copy;
        bool copy_ok = true;
    };

    struct Stream {
        PacketBuilder builder;
        Sequence next = 1;
        Timestamp last_sent = 0;
    };

    // Adds one message to the stream's open packet, sending the packet first if the message
    // does not fit. Returns false if the message cannot be encoded at all.
    template <class Message>
    bool publish(FeedStream stream, const Message& message) {
        Stream& s = stream_of(stream);
        if (s.builder.empty()) {
            s.builder.start(s.next);
        }
        if (!s.builder.append(message)) {
            if (s.builder.empty()) {
                return false;
            }
            send_open(stream);
            if (!s.builder.append(message)) {
                return false;
            }
        }
        ++s.next;
        ++stats_.messages[index(stream)];
        return true;
    }

    void send_open(FeedStream stream);
    void send_packet(FeedStream stream, std::span<const std::byte> packet);
    void send_snapshot(Locate locate);
    void send_heartbeat_if_quiet(FeedStream stream);

    static std::size_t index(FeedStream stream) noexcept {
        return static_cast<std::size_t>(stream);
    }
    Stream& stream_of(FeedStream stream) noexcept { return streams_[index(stream)]; }
    [[nodiscard]] const Stream& stream_of(FeedStream stream) const noexcept {
        return streams_[index(stream)];
    }

    FeedSink* sink_;
    FeedPublisherConfig config_;
    std::array<Stream, kStreamCount> streams_;
    std::vector<Symbol> symbols_;
    FeedPublisherStats stats_;
    Timestamp now_ = 0;                 // The latest time the caller passed in.
    Timestamp next_snapshot_time_ = 0;  // When the next stock's snapshot is due.
    Locate next_snapshot_ = 0;          // Which stock it is.
    bool started_ = false;
    bool ended_ = false;
};

}  // namespace litbook::feed
