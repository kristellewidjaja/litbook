#include "litbook/feed/feed_receiver.hpp"

#include <poll.h>

#include <array>
#include <ctime>
#include <stdexcept>

namespace litbook::feed {

namespace {

MulticastReceiverConfig receiver_config(const FeedReceiverConfig& config,
                                        const MulticastGroup& group) {
    return MulticastReceiverConfig{.interface_name = config.interface_name,
                                   .group = group,
                                   .receive_buffer = config.receive_buffer};
}

}  // namespace

Timestamp monotonic_nanoseconds() noexcept {
    timespec t{};
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    constexpr Timestamp kNanosecondsPerSecond = 1'000'000'000;
    return (static_cast<Timestamp>(t.tv_sec) * kNanosecondsPerSecond) +
           static_cast<Timestamp>(t.tv_nsec);
}

FeedReceiver::FeedReceiver(const FeedReceiverConfig& config)
    : incremental_(receiver_config(config, config.incremental)),
      snapshot_(receiver_config(config, config.snapshot)),
      handler_(config.handler),
      max_packets_per_poll_(config.max_packets_per_poll),
      silence_limit_(config.silence_limit),
      clock_(config.clock) {
    if (max_packets_per_poll_ == 0) {
        throw std::invalid_argument("FeedReceiver max_packets_per_poll must be above 0");
    }
    if (clock_ == nullptr) {
        throw std::invalid_argument("FeedReceiver needs a clock");
    }
}

std::size_t FeedReceiver::poll_once(int timeout_ms) noexcept {
    ++stats_.polls;
    std::array<pollfd, 2> fds{pollfd{.fd = incremental_.fd(), .events = POLLIN, .revents = 0},
                              pollfd{.fd = snapshot_.fd(), .events = POLLIN, .revents = 0}};
    std::size_t read = 0;
    // A result of 0 or less means nothing arrived in time or a signal interrupted the
    // wait. Either way there is nothing to read now, but silence is still checked.
    if (::poll(fds.data(), fds.size(), timeout_ms) > 0) {
        // The incremental stream goes first. A snapshot is only usable together with the
        // incremental messages buffered for it, so those should be in before it arrives.
        read = drain_incremental();
        read += drain_snapshot();
    }
    check_silence();
    return read;
}

std::uint64_t FeedReceiver::accepted_packets() const noexcept {
    const FeedHandlerStats& s = handler_.stats();
    return s.incremental_packets + s.snapshot_packets;
}

void FeedReceiver::check_silence() noexcept {
    const Timestamp now = clock_();
    // Only packets the handler took as its own session count. A stray packet of another
    // session says nothing about whether this feed is alive.
    const std::uint64_t accepted = accepted_packets();
    if (accepted != accepted_before_) {
        accepted_before_ = accepted;
        last_heard_ = now;
        heard_ = true;
        silent_ = false;
        return;
    }
    if (!heard_ || silent_ || silence_limit_ == 0) {
        return;
    }
    // The clock should never go backwards, but if a test clock does, that is no silence.
    if (now >= last_heard_ && now - last_heard_ >= silence_limit_) {
        silent_ = true;
        ++stats_.silences;
    }
}

std::size_t FeedReceiver::drain_incremental() noexcept {
    std::size_t read = 0;
    while (read < max_packets_per_poll_) {
        const ReceiveResult r = incremental_.receive();
        if (r.status == ReceiveStatus::Empty) {
            break;
        }
        ++read;
        if (r.status == ReceiveStatus::Packet) {
            handler_.on_incremental(r.packet);
        } else if (r.status == ReceiveStatus::Truncated) {
            ++stats_.truncated;
        } else {
            ++stats_.failed;
        }
    }
    return read;
}

std::size_t FeedReceiver::drain_snapshot() noexcept {
    std::size_t read = 0;
    while (read < max_packets_per_poll_) {
        const ReceiveResult r = snapshot_.receive();
        if (r.status == ReceiveStatus::Empty) {
            break;
        }
        ++read;
        if (r.status == ReceiveStatus::Packet) {
            handler_.on_snapshot(r.packet);
        } else if (r.status == ReceiveStatus::Truncated) {
            ++stats_.truncated;
        } else {
            ++stats_.failed;
        }
    }
    return read;
}

}  // namespace litbook::feed
