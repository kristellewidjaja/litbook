#pragma once

#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

namespace litbook::feed {

// The messages of the market data feed. docs/FEED.md describes every byte of them. They
// travel inside MoldUDP64 packets (mold.hpp), and feed_codec.hpp turns them into bytes and
// back.

// Nanoseconds since midnight, like the order entry protocol's timestamps.
using Timestamp = std::uint64_t;

// The number of one message within a stream. The first message of a stream is 1.
using Sequence = std::uint64_t;

// The exchange's number for a stock. It is the engine's SymbolId, so locates go from 0 up
// in the order the stocks were listed.
using Locate = std::uint16_t;
static_assert(std::is_same_v<Locate, SymbolId>);

inline constexpr char kSystemEventType = 'S';
inline constexpr char kSymbolDirectoryType = 'R';
inline constexpr char kLevelUpdateType = 'L';
inline constexpr char kTradeType = 'P';
inline constexpr char kSnapshotStartType = 'Y';
inline constexpr char kSnapshotEndType = 'Z';

inline constexpr std::size_t kSystemEventLength = 10;
inline constexpr std::size_t kSymbolDirectoryLength = 31;
inline constexpr std::size_t kLevelUpdateLength = 32;
inline constexpr std::size_t kTradeLength = 32;
inline constexpr std::size_t kSnapshotStartLength = 25;
inline constexpr std::size_t kSnapshotEndLength = 19;

// The longest message, so a buffer of this size holds any one of them.
inline constexpr std::size_t kMaxMessageLength = 32;

// The largest price the feed can carry, the same limit as the order entry protocol
// ($199,999.9900 with 4 implied decimals).
inline constexpr std::uint64_t kMaxWirePrice = 1'999'999'900;

enum class SystemEventCode : char {
    StartOfMessages = 'O',
    EndOfMessages = 'C',
};

struct SystemEvent {
    Timestamp timestamp = 0;
    SystemEventCode code = SystemEventCode::StartOfMessages;
    bool operator==(const SystemEvent&) const = default;
};

// Says which stock a locate stands for. A Ticker has no empty value, so this struct has no
// default constructor.
struct SymbolDirectory {
    Timestamp timestamp;
    Locate locate;
    Ticker symbol;
    Price tick_size;
    Qty lot_size;
    bool operator==(const SymbolDirectory&) const = default;
};

// The new state of one price level. total_qty 0 together with order_count 0 means the
// level is gone.
struct LevelUpdateMessage {
    Timestamp timestamp = 0;
    Locate locate = 0;
    Side side = Side::Buy;
    Price price = 0;
    AggregateQty total_qty = 0;
    std::uint32_t order_count = 0;
    bool operator==(const LevelUpdateMessage&) const = default;
};

struct TradeMessage {
    Timestamp timestamp = 0;
    Locate locate = 0;
    Side aggressor_side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    MatchId match_number = 0;
    bool operator==(const TradeMessage&) const = default;
};

// The first message of one stock's snapshot. as_of is the incremental sequence number of
// the last message the snapshot includes. symbol_count is how many stocks the feed has, so
// a receiver that joined late knows when it has a snapshot of every one.
struct SnapshotStart {
    Timestamp timestamp = 0;
    Locate locate = 0;
    Sequence as_of = 0;
    std::uint32_t level_count = 0;
    std::uint16_t symbol_count = 0;
    bool operator==(const SnapshotStart&) const = default;
};

struct SnapshotEnd {
    Timestamp timestamp = 0;
    Locate locate = 0;
    Sequence as_of = 0;
    bool operator==(const SnapshotEnd&) const = default;
};

using FeedMessage = std::variant<SystemEvent, SymbolDirectory, LevelUpdateMessage, TradeMessage,
                                 SnapshotStart, SnapshotEnd>;

}  // namespace litbook::feed
