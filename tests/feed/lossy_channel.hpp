#pragma once

#include "litbook/feed/feed_handler.hpp"
#include "litbook/feed/feed_publisher.hpp"
#include "litbook/feed/mold.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>

namespace litbook::feed::testing {

// Chances out of 1,000 for each packet.
struct LossRates {
    std::uint32_t drop = 0;
    std::uint32_t duplicate = 0;
    std::uint32_t hold = 0;  // Held back and delivered after the next packet of its stream.
    // How many packets the snapshot stream runs behind the incremental one, as if its
    // multicast group took a slower path. At most LossyChannel::kMaxLag.
    std::size_t snapshot_lag = 0;
};

struct ChannelStats {
    std::uint64_t sent = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duplicated = 0;
    std::uint64_t reordered = 0;
    std::uint64_t not_ok = 0;  // Deliveries the handler did not answer with Ok.
};

// Stands between the publisher and a handler like a bad network. It drops, duplicates
// and reorders packets at random but reproducibly, on each stream on its own. It keeps a
// held packet in a fixed buffer, so it never allocates.
class LossyChannel final : public FeedSink {
public:
    static constexpr std::size_t kMaxLag = 8;

    LossyChannel(FeedHandler& handler, std::uint64_t seed, LossRates rates)
        : handler_(&handler), rng_(seed), rates_(rates) {}

    void send(FeedStream stream, std::span<const std::byte> packet) override {
        ++stats_.sent;
        if (lossless_) {
            release(stream);
            deliver(stream, packet);
            return;
        }
        auto roll = static_cast<std::uint32_t>(rng_() % 1000);
        if (roll < rates_.drop) {
            ++stats_.dropped;
            return;
        }
        roll -= rates_.drop;
        if (roll < rates_.duplicate) {
            ++stats_.duplicated;
            deliver(stream, packet);
            deliver(stream, packet);
            release(stream);
            return;
        }
        roll -= rates_.duplicate;
        Held& held = held_[index(stream)];
        if (roll < rates_.hold && held.size == 0) {
            std::ranges::copy(packet, held.bytes.begin());
            held.size = packet.size();
            return;
        }
        deliver(stream, packet);
        release(stream);
    }

    // From now on every packet arrives once and in order. Held and lagging packets go out
    // first.
    void set_lossless() {
        lossless_ = true;
        release(FeedStream::Incremental);
        release(FeedStream::Snapshot);
        while (lag_count_ > 0) {
            deliver_oldest_lagged();
        }
    }

    // True while an incremental packet is held back. Late snapshot packets do not count,
    // because a handler with every stock live ignores snapshots.
    [[nodiscard]] bool holding_incremental() const noexcept {
        return held_[index(FeedStream::Incremental)].size != 0;
    }
    [[nodiscard]] const ChannelStats& stats() const noexcept { return stats_; }

private:
    struct Held {
        std::array<std::byte, kMaxPacketLength> bytes{};
        std::size_t size = 0;
    };

    static std::size_t index(FeedStream stream) noexcept {
        return static_cast<std::size_t>(stream);
    }

    void deliver(FeedStream stream, std::span<const std::byte> packet) {
        const std::size_t lag = std::min(rates_.snapshot_lag, kMaxLag);
        if (stream == FeedStream::Snapshot && lag > 0 && !lossless_) {
            Held& slot = lagged_[(lag_head_ + lag_count_) % kMaxLag];
            std::ranges::copy(packet, slot.bytes.begin());
            slot.size = packet.size();
            ++lag_count_;
            if (lag_count_ > lag) {
                deliver_oldest_lagged();
            }
            return;
        }
        deliver_now(stream, packet);
    }

    void deliver_oldest_lagged() {
        Held& slot = lagged_[lag_head_];
        lag_head_ = (lag_head_ + 1) % kMaxLag;
        --lag_count_;
        deliver_now(FeedStream::Snapshot, std::span<const std::byte>(slot.bytes).first(slot.size));
    }

    void deliver_now(FeedStream stream, std::span<const std::byte> packet) {
        const PacketStatus status = stream == FeedStream::Incremental
                                        ? handler_->on_incremental(packet)
                                        : handler_->on_snapshot(packet);
        if (status != PacketStatus::Ok) {
            ++stats_.not_ok;
        }
    }

    // Delivers the held packet of this stream, if there is one, after a newer one went.
    void release(FeedStream stream) {
        Held& held = held_[index(stream)];
        if (held.size == 0) {
            return;
        }
        const std::size_t size = held.size;
        held.size = 0;
        ++stats_.reordered;
        deliver(stream, std::span<const std::byte>(held.bytes).first(size));
    }

    FeedHandler* handler_;
    std::mt19937_64 rng_;
    LossRates rates_;
    std::array<Held, kStreamCount> held_{};
    std::array<Held, kMaxLag + 1> lagged_{};  // A ring of the snapshot packets running behind.
    std::size_t lag_head_ = 0;
    std::size_t lag_count_ = 0;
    ChannelStats stats_;
    bool lossless_ = false;
};

}  // namespace litbook::feed::testing
