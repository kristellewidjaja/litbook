#include "litbook/feed/feed_handler.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace litbook::feed {

namespace {

SessionName checked_session(const std::string& name) {
    const std::optional<SessionName> session = make_session_name(name);
    if (!session) {
        throw std::invalid_argument("feed session name must be 1 to 10 letters and digits");
    }
    return *session;
}

}  // namespace

FeedHandler::FeedHandler(const FeedHandlerConfig& config)
    : config_(config),
      session_(checked_session(config.session)),
      scratch_(config.max_levels_per_side) {
    if (config.max_symbols == 0 || config.max_symbols > std::numeric_limits<Locate>::max()) {
        throw std::invalid_argument("feed handler max_symbols must be between 1 and 65,535");
    }
    if (config.max_levels_per_side == 0 || config.buffer_capacity == 0) {
        throw std::invalid_argument("feed handler needs room for levels and buffered updates");
    }
    symbols_.reserve(config.max_symbols);
    for (std::size_t i = 0; i < config.max_symbols; ++i) {
        symbols_.emplace_back(config.max_levels_per_side);
    }
    buffer_.reserve(config.buffer_capacity);
}

bool FeedHandler::accept(std::span<const std::byte> packet, CheckedPacket& checked) {
    checked = check_packet(packet);
    if (checked.status != PacketStatus::Ok) {
        ++stats_.malformed;
        return false;
    }
    if (checked.header.session != session_) {
        ++stats_.wrong_session;
        return false;
    }
    return true;
}

PacketStatus FeedHandler::on_incremental(std::span<const std::byte> packet) {
    CheckedPacket checked;
    if (!accept(packet, checked)) {
        return checked.status;
    }
    ++stats_.incremental_packets;
    const PacketHeader& header = checked.header;
    const Sequence first = header.sequence;
    const std::uint16_t count = header.messages();

    // A heartbeat or end of session carries the next sequence number, so it shows a gap
    // even when nothing else arrives.
    if (count == 0) {
        if (first > next_expected_) {
            on_gap(first);
            next_expected_ = first;
        }
        if (header.end_of_session()) {
            ended_ = true;
        }
        return PacketStatus::Ok;
    }

    if (first + count <= next_expected_) {
        stats_.duplicates += count;
        return PacketStatus::Ok;
    }
    if (first > next_expected_) {
        on_gap(first);
        next_expected_ = first;
    }

    // The packet may start before next_expected_ if part of it was seen already, so each
    // message is checked on its own.
    BlockReader reader(packet);
    std::span<const std::byte> bytes;
    Sequence sequence = first;
    while (reader.next(bytes)) {
        if (sequence < next_expected_) {
            ++stats_.duplicates;
        } else {
            const FeedDecoded decoded = decode_message(bytes);
            if (decoded.message) {
                apply_incremental(sequence, *decoded.message);
            }
            next_expected_ = sequence + 1;
        }
        ++sequence;
    }
    return PacketStatus::Ok;
}

void FeedHandler::apply_incremental(Sequence sequence, const FeedMessage& message) {
    if (const auto* level = std::get_if<LevelUpdateMessage>(&message)) {
        apply_level(sequence, *level);
        return;
    }
    if (const auto* trade = std::get_if<TradeMessage>(&message)) {
        if (trade->locate >= symbols_.size()) {
            ++stats_.bad_locate;
            return;
        }
        Symbol& symbol = symbols_[trade->locate];
        if (!symbol.live) {
            ++stats_.trades_lost;
        } else if (sequence > symbol.through) {
            ++stats_.trades;
            symbol.through = sequence;
        } else {
            ++stats_.duplicates;
        }
        return;
    }
    if (const auto* directory = std::get_if<SymbolDirectory>(&message)) {
        record_directory(*directory);
        return;
    }
    if (const auto* event = std::get_if<SystemEvent>(&message)) {
        if (event->code == SystemEventCode::StartOfMessages) {
            started_ = true;
        }
        return;
    }
    // Snapshot Start and Snapshot End belong on the snapshot stream only.
    ++stats_.unexpected;
}

void FeedHandler::apply_level(Sequence sequence, const LevelUpdateMessage& update) {
    if (update.locate >= symbols_.size()) {
        ++stats_.bad_locate;
        return;
    }
    Symbol& symbol = symbols_[update.locate];
    if (!symbol.live) {
        buffer_update(sequence, update);
        return;
    }
    // A snapshot that was newer than the incremental stream already includes this one.
    if (sequence <= symbol.through) {
        ++stats_.duplicates;
        return;
    }
    if (!symbol.book.set(update.side, update.price, update.total_qty, update.order_count)) {
        ++stats_.book_full;
        make_stale(symbol, sequence);
        buffer_update(sequence, update);
        return;
    }
    symbol.through = sequence;
    ++stats_.applied;
}

void FeedHandler::buffer_update(Sequence sequence, const LevelUpdateMessage& update) {
    if (buffer_.size() == config_.buffer_capacity) {
        // Throwing the buffer away loses updates the stale stocks need, so each of them
        // now needs a snapshot that includes everything before this update.
        ++stats_.buffer_overflows;
        buffer_.clear();
        for (Symbol& symbol : symbols_) {
            if (!symbol.live) {
                symbol.needs_from = sequence;
            }
        }
    }
    buffer_.push_back(Buffered{.sequence = sequence, .update = update});
}

void FeedHandler::on_gap(Sequence first_received) {
    ++stats_.gaps;
    // Every buffered update is older than the lost messages, so none of them is enough any
    // more, and every stock may have lost an update.
    buffer_.clear();
    const std::size_t listed =
        symbol_count_ ? std::min<std::size_t>(*symbol_count_, symbols_.size()) : symbols_.size();
    for (std::size_t i = 0; i < listed; ++i) {
        make_stale(symbols_[i], first_received);
    }
}

void FeedHandler::make_stale(Symbol& symbol, Sequence needs_from) {
    if (symbol.live) {
        symbol.live = false;
        ++stale_;
    }
    symbol.needs_from = needs_from;
}

void FeedHandler::make_live(Symbol& symbol) {
    if (symbol.live) {
        return;
    }
    symbol.live = true;
    --stale_;
    if (stale_ == 0) {
        ++stats_.recoveries;
        buffer_.clear();
    }
}

void FeedHandler::record_directory(const SymbolDirectory& directory) {
    if (directory.locate >= symbols_.size()) {
        ++stats_.bad_locate;
        return;
    }
    symbols_[directory.locate].directory = directory;
}

PacketStatus FeedHandler::on_snapshot(std::span<const std::byte> packet) {
    CheckedPacket checked;
    if (!accept(packet, checked)) {
        return checked.status;
    }
    ++stats_.snapshot_packets;
    const Sequence first = checked.header.sequence;
    const std::uint16_t count = checked.header.messages();

    // Lost snapshot messages ruin the snapshot being put together. The first packet this
    // handler sees only tells it where the stream is.
    if (snapshot_position_known_ && first > next_snapshot_) {
        ++stats_.snapshot_gaps;
        break_partial();
    }
    if (!snapshot_position_known_ || first > next_snapshot_) {
        next_snapshot_ = first;
        snapshot_position_known_ = true;
    }
    if (count == 0) {
        return PacketStatus::Ok;
    }

    BlockReader reader(packet);
    std::span<const std::byte> bytes;
    Sequence sequence = first;
    while (reader.next(bytes)) {
        if (sequence >= next_snapshot_) {
            const FeedDecoded decoded = decode_message(bytes);
            if (decoded.message) {
                handle_snapshot_message(*decoded.message);
            }
            next_snapshot_ = sequence + 1;
        }
        ++sequence;
    }
    return PacketStatus::Ok;
}

void FeedHandler::handle_snapshot_message(const FeedMessage& message) {
    if (const auto* level = std::get_if<LevelUpdateMessage>(&message)) {
        if (!partial_.active) {
            return;
        }
        // A snapshot level is never empty, and it must belong to the stock being built.
        const bool fits =
            level->locate == partial_.locate && level->total_qty != 0 &&
            scratch_.set(level->side, level->price, level->total_qty, level->order_count);
        if (!fits) {
            break_partial();
            return;
        }
        ++partial_.received;
        return;
    }
    if (const auto* start = std::get_if<SnapshotStart>(&message)) {
        break_partial();
        learn_symbol_count(start->symbol_count);
        if (start->locate >= symbols_.size()) {
            ++stats_.bad_locate;
            return;
        }
        // A live stock does not need the snapshot, so it is not even assembled.
        if (symbols_[start->locate].live) {
            return;
        }
        partial_ = PartialSnapshot{.active = true,
                                   .locate = start->locate,
                                   .as_of = start->as_of,
                                   .level_count = start->level_count,
                                   .received = 0};
        scratch_.clear();
        return;
    }
    if (const auto* end = std::get_if<SnapshotEnd>(&message)) {
        finish_snapshot(*end);
        return;
    }
    if (const auto* directory = std::get_if<SymbolDirectory>(&message)) {
        break_partial();
        record_directory(*directory);
        return;
    }
    // Trades and system events belong on the incremental stream only.
    ++stats_.unexpected;
}

void FeedHandler::finish_snapshot(const SnapshotEnd& end) {
    if (!partial_.active) {
        return;
    }
    const PartialSnapshot p = partial_;
    partial_.active = false;
    const std::size_t levels = scratch_.level_count(Side::Buy) + scratch_.level_count(Side::Sell);
    const bool whole = end.locate == p.locate && end.as_of == p.as_of &&
                       p.received == p.level_count && levels == p.level_count;
    if (!whole) {
        ++stats_.snapshots_broken;
        return;
    }
    Symbol& symbol = symbols_[p.locate];
    if (symbol.live) {
        return;
    }
    // The snapshot is usable only if every update after it is in the buffer.
    if (p.as_of + 1 < symbol.needs_from) {
        ++stats_.snapshots_too_old;
        return;
    }
    if (!symbol.book.copy_from(scratch_)) {
        ++stats_.book_full;
        return;
    }
    symbol.through = p.as_of;
    ++stats_.snapshots_applied;

    for (const Buffered& b : buffer_) {
        if (b.update.locate != p.locate || b.sequence <= symbol.through) {
            continue;
        }
        if (!symbol.book.set(b.update.side, b.update.price, b.update.total_qty,
                             b.update.order_count)) {
            // The book cannot hold this stock. It stays stale, and the next snapshot will
            // not fit either, which the book_full count shows.
            ++stats_.book_full;
            return;
        }
        symbol.through = b.sequence;
        ++stats_.replayed;
    }
    make_live(symbol);
}

void FeedHandler::learn_symbol_count(std::uint16_t count) {
    if (symbol_count_) {
        return;
    }
    symbol_count_ = count;
    // Locates at or above the count are not stocks, so they never get a snapshot and
    // must not keep the handler recovering.
    for (std::size_t i = count; i < symbols_.size(); ++i) {
        make_live(symbols_[i]);
    }
}

void FeedHandler::break_partial() {
    if (partial_.active) {
        partial_.active = false;
        ++stats_.snapshots_broken;
    }
}

}  // namespace litbook::feed
