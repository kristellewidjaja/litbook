#pragma once

#include "litbook/feed/feed_messages.hpp"
#include "litbook/feed/level_book.hpp"
#include "litbook/feed/mold.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace litbook::feed {

struct FeedHandlerConfig {
    std::string session = "1";
    // Every stock's book reserves room for max_levels_per_side levels on each side up front,
    // so the memory is about max_symbols x 2 x max_levels_per_side x 24 bytes (3 MB here).
    std::size_t max_symbols = 64;
    std::size_t max_levels_per_side = 1024;
    // How many incremental level updates can wait for snapshots during a recovery.
    std::size_t buffer_capacity = 65'536;
};

struct FeedHandlerStats {
    std::uint64_t incremental_packets = 0;
    std::uint64_t snapshot_packets = 0;
    std::uint64_t applied = 0;        // Incremental messages applied to a live book.
    std::uint64_t duplicates = 0;     // Incremental messages seen before, dropped.
    std::uint64_t gaps = 0;           // Times incremental messages were found missing.
    std::uint64_t snapshot_gaps = 0;  // Times snapshot messages were found missing.
    std::uint64_t snapshots_applied = 0;
    std::uint64_t snapshots_too_old = 0;  // Complete, but older than what the stock needs.
    std::uint64_t snapshots_broken = 0;   // Cut by a gap or not matching their own counts.
    std::uint64_t replayed = 0;           // Buffered updates applied after a snapshot.
    std::uint64_t buffer_overflows = 0;
    std::uint64_t recoveries = 0;     // Times every stock was live again after a gap.
    std::uint64_t trades = 0;         // Trades seen for a live stock.
    std::uint64_t trades_lost = 0;    // Trades seen for a stale stock.
    std::uint64_t malformed = 0;      // Packets dropped because check_packet failed.
    std::uint64_t wrong_session = 0;  // Packets of another session, dropped.
    std::uint64_t bad_locate = 0;     // Messages for a locate above max_symbols.
    std::uint64_t book_full = 0;      // Level updates that did not fit in a book.
    std::uint64_t unexpected = 0;     // Messages on the wrong stream.
};

// The receiving side of the feed. It keeps a copy of every stock's levels from the
// incremental stream, notices lost messages from the sequence numbers, and gets back to a
// correct copy with the snapshot stream. It has no sockets. The caller hands it each
// packet with on_incremental or on_snapshot, and reads the books.
//
// How recovery works (docs/FEED.md has the same steps). Each stock is live or stale. A
// live stock's book is right as of its `through` sequence number. A gap in the incremental
// sequence numbers makes every stock stale, because a lost packet could have been about
// any of them. From then on the level updates of stale stocks are kept in a buffer. A
// stale stock needs a snapshot that includes everything before the first update it
// buffered (`needs_from`). When one arrives whole, the stock's book becomes the snapshot,
// its buffered updates after the snapshot are applied, and it is live again. A buffer that
// fills up is emptied, and every stale stock then needs a newer snapshot.
//
// Invariants. A live stock's book equals the exchange's book after incremental message
// `through`. Buffered updates are in sequence order, all for stale stocks, all at or after
// those stocks' needs_from. Nothing allocates after the constructor.
class FeedHandler {
public:
    // Throws std::invalid_argument for a bad session name or a size of 0.
    explicit FeedHandler(const FeedHandlerConfig& config);

    // Handles one packet of each stream. A packet that fails check_packet is dropped as a
    // whole and its status returned.
    PacketStatus on_incremental(std::span<const std::byte> packet);
    PacketStatus on_snapshot(std::span<const std::byte> packet);

    // True while some stock is stale. A real receiver reads the snapshot stream only then.
    [[nodiscard]] bool recovering() const noexcept { return stale_ > 0; }
    [[nodiscard]] bool live(Locate locate) const noexcept {
        return locate < symbols_.size() && symbols_[locate].live;
    }
    // A stock's book, or nullptr for a locate above max_symbols. Only a live book is right.
    [[nodiscard]] const LevelBook* book(Locate locate) const noexcept {
        return locate < symbols_.size() ? &symbols_[locate].book : nullptr;
    }
    // The stock's directory entry, once a Symbol Directory message for it arrived.
    [[nodiscard]] const std::optional<SymbolDirectory>& directory(Locate locate) const noexcept {
        return symbols_[locate].directory;
    }
    // The last incremental sequence number a stock's book includes.
    [[nodiscard]] Sequence through(Locate locate) const noexcept {
        return symbols_[locate].through;
    }
    // The incremental sequence number the handler expects next.
    [[nodiscard]] Sequence next_expected() const noexcept { return next_expected_; }
    // How many stocks the feed has, once a snapshot said so.
    [[nodiscard]] std::optional<std::uint16_t> symbol_count() const noexcept {
        return symbol_count_;
    }
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] const FeedHandlerStats& stats() const noexcept { return stats_; }

private:
    struct Symbol {
        explicit Symbol(std::size_t max_levels) : book(max_levels) {}
        LevelBook book;
        std::optional<SymbolDirectory> directory;
        Sequence through = 0;
        Sequence needs_from = 0;  // While stale, the first sequence number it buffered.
        bool live = true;
    };

    struct Buffered {
        Sequence sequence = 0;
        LevelUpdateMessage update;
    };

    // A snapshot being put together from the snapshot stream.
    struct PartialSnapshot {
        bool active = false;
        Locate locate = 0;
        Sequence as_of = 0;
        std::uint32_t level_count = 0;
        std::uint32_t received = 0;
    };

    // Checks the packet and its session. Returns false if it must be dropped.
    bool accept(std::span<const std::byte> packet, CheckedPacket& checked);

    void apply_incremental(Sequence sequence, const FeedMessage& message);
    void apply_level(Sequence sequence, const LevelUpdateMessage& update);
    void handle_snapshot_message(const FeedMessage& message);
    void finish_snapshot(const SnapshotEnd& end);
    void learn_symbol_count(std::uint16_t count);
    void record_directory(const SymbolDirectory& directory);

    void on_gap(Sequence first_received);
    void make_stale(Symbol& symbol, Sequence needs_from);
    void make_live(Symbol& symbol);
    void buffer_update(Sequence sequence, const LevelUpdateMessage& update);
    void break_partial();

    FeedHandlerConfig config_;
    SessionName session_;
    std::vector<Symbol> symbols_;
    std::vector<Buffered> buffer_;
    LevelBook scratch_;  // Where a snapshot is assembled before it replaces a book.
    PartialSnapshot partial_;
    FeedHandlerStats stats_;
    Sequence next_expected_ = 1;
    Sequence next_snapshot_ = 0;  // The snapshot sequence number expected next.
    bool snapshot_position_known_ = false;
    std::optional<std::uint16_t> symbol_count_;
    std::size_t stale_ = 0;  // How many stocks are stale.
    bool started_ = false;
    bool ended_ = false;
};

}  // namespace litbook::feed
