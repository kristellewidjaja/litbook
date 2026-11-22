#include "litbook/feed/feed_publisher.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace litbook::feed {

namespace {

SessionName checked_session(const std::string& name) {
    const std::optional<SessionName> session = make_session_name(name);
    if (!session) {
        throw std::invalid_argument("feed session name must be 1 to 10 letters and digits");
    }
    return *session;
}

// How long it has been since `then`. A clock that went backwards counts as no time.
Timestamp elapsed(Timestamp now, Timestamp then) noexcept {
    return now > then ? now - then : 0;
}

}  // namespace

FeedPublisher::FeedPublisher(FeedSink& sink, const FeedPublisherConfig& config)
    : sink_(&sink),
      config_(config),
      streams_{Stream{.builder =
                          PacketBuilder(checked_session(config.session), config.max_packet_length)},
               Stream{.builder = PacketBuilder(checked_session(config.session),
                                               config.max_packet_length)}} {
    if (config.max_symbols == 0 || config.max_symbols > std::numeric_limits<Locate>::max()) {
        throw std::invalid_argument("feed max_symbols must be between 1 and 65,535");
    }
    if (config.heartbeat_interval == 0) {
        throw std::invalid_argument("feed heartbeat_interval must be above 0");
    }
    symbols_.reserve(config.max_symbols);
}

Locate FeedPublisher::add_symbol(const FeedSymbol& symbol) {
    if (started_) {
        throw std::logic_error("feed symbols must be added before start()");
    }
    if (symbols_.size() >= config_.max_symbols) {
        throw std::length_error("feed already lists max_symbols stocks");
    }
    const bool tick_ok =
        symbol.tick_size > 0 && std::cmp_less_equal(symbol.tick_size, kMaxWirePrice);
    if (!tick_ok || symbol.lot_size == 0 || symbol.max_levels_per_side == 0) {
        throw std::invalid_argument(
            "feed symbol needs a tick size, a lot size and room for levels");
    }
    const auto locate = static_cast<Locate>(symbols_.size());
    symbols_.push_back(Symbol{.directory = SymbolDirectory{.timestamp = 0,
                                                           .locate = locate,
                                                           .symbol = symbol.symbol,
                                                           .tick_size = symbol.tick_size,
                                                           .lot_size = symbol.lot_size},
                              .copy = LevelBook(symbol.max_levels_per_side),
                              .copy_ok = true});
    return locate;
}

void FeedPublisher::restore(const pipeline::MarketEvent& event) {
    if (started_) {
        throw std::logic_error("feed levels can only be restored before start()");
    }
    if (event.kind != pipeline::MarketEventKind::LevelUpdate || event.symbol >= symbols_.size()) {
        return;
    }
    Symbol& symbol = symbols_[event.symbol];
    if (!symbol.copy.set(event.side, event.price, event.qty, event.order_count)) {
        symbol.copy_ok = false;
        ++stats_.copy_full;
    }
}

void FeedPublisher::rename_session(const std::string& session) {
    if (started_) {
        throw std::logic_error("the feed session can only be renamed before start()");
    }
    const SessionName name = checked_session(session);
    for (Stream& s : streams_) {
        s.builder = PacketBuilder(name, config_.max_packet_length);
    }
    config_.session = session;
}

void FeedPublisher::start(Timestamp now) {
    if (started_) {
        return;
    }
    started_ = true;
    now_ = now;
    publish(FeedStream::Incremental,
            SystemEvent{.timestamp = now, .code = SystemEventCode::StartOfMessages});
    for (Symbol& symbol : symbols_) {
        symbol.directory.timestamp = now;
        publish(FeedStream::Incremental, symbol.directory);
    }
    send_open(FeedStream::Incremental);
    stream_of(FeedStream::Snapshot).last_sent = now;
    next_snapshot_time_ = now;
}

void FeedPublisher::on_event(const pipeline::MarketEvent& event) {
    if (event.kind == pipeline::MarketEventKind::Stop) {
        return;
    }
    if (!started_ || ended_) {
        ++stats_.ignored;
        return;
    }
    if (event.symbol >= symbols_.size()) {
        ++stats_.unknown_symbol;
        return;
    }
    bool published = false;
    if (event.kind == pipeline::MarketEventKind::Trade) {
        const TradeMessage message{.timestamp = event.now,
                                   .locate = event.symbol,
                                   .aggressor_side = event.side,
                                   .price = event.price,
                                   .qty = static_cast<Qty>(event.qty),
                                   .match_number = event.match_id};
        published = publish(FeedStream::Incremental, message);
    } else {
        Symbol& symbol = symbols_[event.symbol];
        if (!symbol.copy.set(event.side, event.price, event.qty, event.order_count)) {
            // The copy is now wrong, so this stock gets no more snapshots. The incremental
            // stream is still right.
            symbol.copy_ok = false;
            ++stats_.copy_full;
        }
        const LevelUpdateMessage message{.timestamp = event.now,
                                         .locate = event.symbol,
                                         .side = event.side,
                                         .price = event.price,
                                         .total_qty = event.qty,
                                         .order_count = event.order_count};
        published = publish(FeedStream::Incremental, message);
    }
    if (published) {
        ++stats_.events;
    } else {
        ++stats_.unencodable;
    }
}

void FeedPublisher::flush(Timestamp now) {
    now_ = now;
    send_open(FeedStream::Incremental);
}

void FeedPublisher::tick(Timestamp now) {
    if (!started_ || ended_) {
        return;
    }
    now_ = now;
    send_open(FeedStream::Incremental);
    if (!symbols_.empty() && now >= next_snapshot_time_) {
        send_snapshot(next_snapshot_);
        next_snapshot_ = static_cast<Locate>((next_snapshot_ + 1U) % symbols_.size());
        next_snapshot_time_ = now + config_.snapshot_spacing;
    }
    send_heartbeat_if_quiet(FeedStream::Incremental);
    send_heartbeat_if_quiet(FeedStream::Snapshot);
}

Timestamp FeedPublisher::next_deadline() const noexcept {
    if (!started_ || ended_) {
        return std::numeric_limits<Timestamp>::max();
    }
    Timestamp deadline = std::numeric_limits<Timestamp>::max();
    if (!symbols_.empty()) {
        deadline = next_snapshot_time_;
    }
    for (const Stream& s : streams_) {
        // A heartbeat is due once heartbeat_interval has passed since the last packet. The
        // interval comes from the caller and could be huge, so the sum is capped instead of
        // allowed to wrap around.
        const Timestamp room = std::numeric_limits<Timestamp>::max() - s.last_sent;
        Timestamp heartbeat_due = std::numeric_limits<Timestamp>::max();
        if (config_.heartbeat_interval < room) {
            heartbeat_due = s.last_sent + config_.heartbeat_interval;
        }
        deadline = std::min(deadline, heartbeat_due);
    }
    return deadline;
}

void FeedPublisher::end_session(Timestamp now) {
    if (!started_ || ended_) {
        return;
    }
    now_ = now;
    publish(FeedStream::Incremental,
            SystemEvent{.timestamp = now, .code = SystemEventCode::EndOfMessages});
    send_open(FeedStream::Incremental);
    send_open(FeedStream::Snapshot);
    for (int i = 0; i < config_.end_of_session_packets; ++i) {
        for (const FeedStream stream : {FeedStream::Incremental, FeedStream::Snapshot}) {
            Stream& s = stream_of(stream);
            send_packet(stream, s.builder.end_of_session(s.next));
        }
    }
    ended_ = true;
}

void FeedPublisher::send_open(FeedStream stream) {
    Stream& s = stream_of(stream);
    if (s.builder.empty()) {
        return;
    }
    send_packet(stream, s.builder.finish());
    s.builder.start(s.next);
}

void FeedPublisher::send_packet(FeedStream stream, std::span<const std::byte> packet) {
    stream_of(stream).last_sent = now_;
    ++stats_.packets[index(stream)];
    sink_->send(stream, packet);
}

void FeedPublisher::send_heartbeat_if_quiet(FeedStream stream) {
    Stream& s = stream_of(stream);
    if (elapsed(now_, s.last_sent) < config_.heartbeat_interval) {
        return;
    }
    ++stats_.heartbeats[index(stream)];
    send_packet(stream, s.builder.heartbeat(s.next));
}

// Sends one stock's whole snapshot without handling any event in between, so the levels
// it carries are exactly the copy after incremental message as_of. Only tick() calls this,
// and tick() has already sent the incremental packet being built, so a receiver can always
// have every message up to as_of.
void FeedPublisher::send_snapshot(Locate locate) {
    const Symbol& symbol = symbols_[locate];
    if (!symbol.copy_ok) {
        return;
    }
    const Sequence as_of = stream_of(FeedStream::Incremental).next - 1;
    const LevelBook& book = symbol.copy;
    const std::size_t levels = book.level_count(Side::Buy) + book.level_count(Side::Sell);

    SymbolDirectory directory = symbol.directory;
    directory.timestamp = now_;
    publish(FeedStream::Snapshot, directory);
    publish(FeedStream::Snapshot,
            SnapshotStart{.timestamp = now_,
                          .locate = locate,
                          .as_of = as_of,
                          .level_count = static_cast<std::uint32_t>(levels),
                          .symbol_count = static_cast<std::uint16_t>(symbols_.size())});
    for (const Side side : {Side::Buy, Side::Sell}) {
        for (std::size_t rank = 0; rank < book.level_count(side); ++rank) {
            const PriceLevel& level = book.level(side, rank);
            publish(FeedStream::Snapshot, LevelUpdateMessage{.timestamp = now_,
                                                             .locate = locate,
                                                             .side = side,
                                                             .price = level.price,
                                                             .total_qty = level.total_qty,
                                                             .order_count = level.order_count});
        }
    }
    publish(FeedStream::Snapshot, SnapshotEnd{.timestamp = now_, .locate = locate, .as_of = as_of});
    send_open(FeedStream::Snapshot);
    ++stats_.snapshots;
}

}  // namespace litbook::feed
