#pragma once

// The market data feed's books, kept current by a reader thread, for callers that only
// look now and then (bots and the terminal book view written in Python). The thread waits
// on both multicast sockets, hands every packet to a FeedReceiver and its FeedHandler
// (gap detection and snapshot recovery, D-055), and counts a new version after every read
// that brought packets. Callers read copies of what they ask for under a mutex, and can
// wait for the next version.
//
// The reader has its own thread because a feed that is not read during a burst loses the
// end of it, End of Session included, and a caller that is busy (or a Python loop holding
// the interpreter) would leave the sockets unread. The handler recovers lost level
// updates from snapshots, but it cannot recover the time the books spent stale.
//
// A symbol is known once its Symbol Directory message arrived, on either stream. Its
// levels are right only while it is live. While it is stale (after a gap, until its
// snapshot arrives) best() and depth() still answer, from the last good copy, and say so.
// A book that joined late counts as stale until it has read the incremental stream once,
// because only then can it notice the updates it missed.
//
// Invariants. Only the reader thread touches the receiver, and only while holding the
// mutex, so every read sees the books as of one whole packet. version() only grows. The
// destructor stops and joins the thread.

#include "litbook/feed/feed_handler.hpp"
#include "litbook/feed/feed_receiver.hpp"
#include "litbook/feed/level_book.hpp"
#include "litbook/types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace litbook::client {

struct FeedBookConfig {
    feed::FeedReceiverConfig receiver;
    // The longest the reader waits on the sockets before it checks for a stop and for
    // silence. It bounds how long stop() takes.
    int wait_ms = 10;
};

enum class SymbolStatus : std::uint8_t {
    Unknown,  // No Symbol Directory message for it yet.
    Live,     // The book is right as of the last message.
    Stale,    // A gap happened, or nothing was read on the incremental stream yet.
};

[[nodiscard]] const char* to_string(SymbolStatus status) noexcept;

struct Quote {
    std::optional<feed::PriceLevel> bid;
    std::optional<feed::PriceLevel> ask;
    bool live = false;
};

struct FeedBookStats {
    feed::FeedHandlerStats handler;
    feed::FeedReceiverStats receiver;
    std::uint64_t version = 0;
    bool started = false;     // The first packet of the session arrived.
    bool ended = false;       // End of Session arrived.
    bool recovering = false;  // Some stock is stale.
    bool silent = false;      // Nothing heard for the silence limit (FeedReceiver).
};

class FeedBook {
public:
    // Joins both groups and starts the reader. Throws like FeedReceiver's constructor, and
    // std::invalid_argument for a wait_ms below 1.
    explicit FeedBook(const FeedBookConfig& config);
    ~FeedBook();

    FeedBook(const FeedBook&) = delete;
    FeedBook& operator=(const FeedBook&) = delete;
    FeedBook(FeedBook&&) = delete;
    FeedBook& operator=(FeedBook&&) = delete;

    // Stops and joins the reader. The books keep their last state. Calling it twice is fine.
    void stop();

    // The ports the sockets are bound to. With port 0 in the config the kernel picks them,
    // and the publisher must then be told.
    [[nodiscard]] std::uint16_t incremental_port() const noexcept { return incremental_port_; }
    [[nodiscard]] std::uint16_t snapshot_port() const noexcept { return snapshot_port_; }

    [[nodiscard]] SymbolStatus status(std::string_view symbol) const;
    // The best bid and ask, or nothing for an unknown symbol.
    [[nodiscard]] std::optional<Quote> best(std::string_view symbol) const;
    // Up to `levels` price levels of one side, best first. Empty for an unknown symbol.
    [[nodiscard]] std::vector<feed::PriceLevel> depth(std::string_view symbol, Side side,
                                                      std::size_t levels) const;
    // Every known symbol, in locate order.
    [[nodiscard]] std::vector<std::string> symbols() const;

    // How many reads brought packets so far.
    [[nodiscard]] std::uint64_t version() const;
    // Waits up to timeout_ms for version() to pass `seen`, and returns the version then.
    std::uint64_t wait_update(std::uint64_t seen, int timeout_ms) const;

    [[nodiscard]] FeedBookStats stats() const;

private:
    void run();
    // The locate of a known symbol. Called with the mutex held.
    [[nodiscard]] std::optional<feed::Locate> find(std::string_view symbol) const;
    // Live in the handler, and the incremental stream was read at least once. Needs the mutex.
    [[nodiscard]] bool live(feed::Locate locate) const;

    feed::FeedReceiver receiver_;
    int wait_ms_;
    std::size_t max_symbols_;
    std::uint16_t incremental_port_;
    std::uint16_t snapshot_port_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::uint64_t version_ = 0;  // Guarded by mutex_.
    std::atomic<bool> stopping_{false};
    std::thread reader_;
};

}  // namespace litbook::client
