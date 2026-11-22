#pragma once

#include "litbook/feed/feed_publisher.hpp"
#include "litbook/feed/multicast.hpp"

#include <span>
#include <string>

namespace litbook::feed {

// Where the two streams of the feed go. Each stream has its own multicast group, so a
// receiver that is not recovering can ignore the snapshot stream without reading it.
struct FeedNetworkConfig {
    std::string interface_name = "lo";
    MulticastGroup incremental{.address = "239.255.0.1", .port = 15100};
    MulticastGroup snapshot{.address = "239.255.0.2", .port = 15101};
    int ttl = 1;
    bool loop = true;
    int send_buffer = 0;
};

// The FeedSink that puts the publisher's packets on the network, one MulticastSender per
// stream. A packet the kernel had no room for is lost, as UDP packets can be, and the
// sender counts it. Receivers find the gap from the sequence numbers and recover.
//
// Invariants. Both senders exist for the sink's whole life, and every packet goes to the
// sender of its own stream.
class UdpFeedSink final : public FeedSink {
public:
    // Throws like MulticastSender's constructor.
    explicit UdpFeedSink(const FeedNetworkConfig& config)
        : incremental_(sender_config(config, config.incremental)),
          snapshot_(sender_config(config, config.snapshot)) {}

    void send(FeedStream stream, std::span<const std::byte> packet) override {
        writable(stream).send(packet);
    }

    [[nodiscard]] const MulticastSender& sender(FeedStream stream) const noexcept {
        return stream == FeedStream::Incremental ? incremental_ : snapshot_;
    }

private:
    static MulticastSenderConfig sender_config(const FeedNetworkConfig& config,
                                               const MulticastGroup& group) {
        return MulticastSenderConfig{.interface_name = config.interface_name,
                                     .group = group,
                                     .ttl = config.ttl,
                                     .loop = config.loop,
                                     .send_buffer = config.send_buffer};
    }

    MulticastSender& writable(FeedStream stream) noexcept {
        return stream == FeedStream::Incremental ? incremental_ : snapshot_;
    }

    MulticastSender incremental_;
    MulticastSender snapshot_;
};

}  // namespace litbook::feed
