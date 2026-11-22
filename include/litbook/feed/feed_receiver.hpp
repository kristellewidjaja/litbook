#pragma once

#include "litbook/feed/feed_handler.hpp"
#include "litbook/feed/multicast.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace litbook::feed {

// The receiver's clock, in nanoseconds. Only differences between two readings matter.
using ReceiverClock = Timestamp (*)();

// CLOCK_MONOTONIC in nanoseconds. It never jumps when the wall clock is set, so a silence
// is measured correctly even across a clock change.
[[nodiscard]] Timestamp monotonic_nanoseconds() noexcept;

// Three heartbeat intervals of the default feed (FeedPublisherConfig::heartbeat_interval).
inline constexpr Timestamp kDefaultSilenceLimit = 3'000'000'000;

struct FeedReceiverConfig {
    std::string interface_name = "lo";
    MulticastGroup incremental{.address = "239.255.0.1", .port = 15100};
    MulticastGroup snapshot{.address = "239.255.0.2", .port = 15101};
    int receive_buffer = 0;  // SO_RCVBUF of both sockets, or 0 for the kernel's default.
    // At most this many packets are read from one socket per poll_once, so a busy stream
    // cannot keep the other one waiting forever.
    std::size_t max_packets_per_poll = 64;
    // After the first packet of the session, this long with no packet of the session on
    // either stream makes the receiver silent (see FeedReceiver::silent). 0 turns it off.
    Timestamp silence_limit = kDefaultSilenceLimit;
    // Tests pass a clock they move by hand.
    ReceiverClock clock = &monotonic_nanoseconds;
    FeedHandlerConfig handler;
};

struct FeedReceiverStats {
    std::uint64_t polls = 0;
    std::uint64_t truncated = 0;  // Packets too long for the buffer, on either socket.
    std::uint64_t failed = 0;     // Failed reads on either socket.
    std::uint64_t silences = 0;   // Times the receiver became silent.
};

// A feed handler on real sockets. It joins both multicast groups and hands every packet
// it reads to its FeedHandler. The snapshot stream is always read, even when every stock
// is live, so its socket buffer never fills up with old snapshots. The handler does not
// assemble snapshots of live stocks, so they cost only a decode.
//
// Silence. A running publisher sends a heartbeat at least every second and a snapshot
// every few milliseconds, so a receiver that hears nothing for several heartbeat intervals
// knows the feed stopped or the network failed. It cannot tell which, and it cannot tell
// whether End of Session was sent and lost. So it does not pretend the session ended.
// silent() turns true, the books stay as they were, and the caller decides what to do.
// The next packet of the session clears it, and if messages were missed in between, the
// handler sees the gap from the sequence numbers and recovers as usual.
//
// Invariants. Exactly one thread uses a FeedReceiver. Packets of each stream reach the
// handler in the order the kernel delivered them. silent() is false until the first packet
// of the session and whenever the last one was less than silence_limit ago. Nothing
// allocates after the constructor.
class FeedReceiver {
public:
    // Throws like MulticastReceiver's and FeedHandler's constructors.
    explicit FeedReceiver(const FeedReceiverConfig& config);

    // Waits up to timeout_ms (0 does not wait, -1 waits until something arrives) for
    // either socket to be readable, then reads what is waiting. Returns how many packets
    // were read from both sockets together. Silence is checked at the end of every call,
    // so a caller that wants to notice it must not wait forever (timeout -1).
    std::size_t poll_once(int timeout_ms) noexcept;

    // True when the feed has gone quiet for silence_limit after it was heard (see above).
    [[nodiscard]] bool silent() const noexcept { return silent_; }
    // When the last packet of the session arrived, by the receiver's clock. 0 before any.
    [[nodiscard]] Timestamp last_heard() const noexcept { return last_heard_; }

    [[nodiscard]] const FeedHandler& handler() const noexcept { return handler_; }
    [[nodiscard]] const MulticastReceiver& incremental() const noexcept { return incremental_; }
    [[nodiscard]] const MulticastReceiver& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const FeedReceiverStats& stats() const noexcept { return stats_; }

private:
    std::size_t drain_incremental() noexcept;
    std::size_t drain_snapshot() noexcept;
    // How many packets the handler has taken as part of its session, on both streams.
    [[nodiscard]] std::uint64_t accepted_packets() const noexcept;
    void check_silence() noexcept;

    MulticastReceiver incremental_;
    MulticastReceiver snapshot_;
    FeedHandler handler_;
    std::size_t max_packets_per_poll_;
    Timestamp silence_limit_;
    ReceiverClock clock_;
    FeedReceiverStats stats_;
    std::uint64_t accepted_before_ = 0;  // accepted_packets() at the end of the last poll.
    Timestamp last_heard_ = 0;
    bool heard_ = false;
    bool silent_ = false;
};

}  // namespace litbook::feed
