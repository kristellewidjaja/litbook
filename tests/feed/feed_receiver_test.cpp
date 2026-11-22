#include "litbook/feed/feed_receiver.hpp"

#include "litbook/feed/feed_publisher.hpp"
#include "litbook/feed/mold.hpp"
#include "litbook/feed/udp_feed_sink.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>

#include "feed/feed_test_support.hpp"
#include "support/unwrap.hpp"

// The publisher and the receiver over real multicast sockets on loopback, driven from one
// thread. The publisher gets its time by hand, so snapshots and heartbeats come exactly
// when a test asks for them. Receivers bind port 0, so each test has its own ports.

namespace litbook::feed {
namespace {

using testing::level_event;
using testing::ticker;
using testing::trade_event;
using testing::unwrap_session;

constexpr Timestamp kSpacing = 1'000'000;  // Snapshot spacing, 1 ms.
constexpr Price kTick = 100;

// A clock the silence tests move by hand. Only one thread reads it.
Timestamp g_fake_now = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

Timestamp fake_now() {
    return g_fake_now;
}

FeedReceiverConfig receiver_config(const char* incremental, const char* snapshot,
                                   std::uint16_t incremental_port = 0,
                                   std::uint16_t snapshot_port = 0, int receive_buffer = 0,
                                   ReceiverClock clock = &monotonic_nanoseconds) {
    FeedReceiverConfig c;
    c.clock = clock;
    c.incremental = MulticastGroup{.address = incremental, .port = incremental_port};
    c.snapshot = MulticastGroup{.address = snapshot, .port = snapshot_port};
    c.receive_buffer = receive_buffer;
    c.handler = FeedHandlerConfig{
        .session = "UDP", .max_symbols = 4, .max_levels_per_side = 256, .buffer_capacity = 8192};
    return c;
}

class FeedReceiverTest : public ::testing::Test {
protected:
    // Makes the receiver first, because the sender must be told the ports it got.
    void make(const char* incremental, const char* snapshot, int receive_buffer = 0,
              ReceiverClock clock = &monotonic_nanoseconds) {
        receiver_ = std::make_unique<FeedReceiver>(
            receiver_config(incremental, snapshot, 0, 0, receive_buffer, clock));
        FeedNetworkConfig network;
        network.incremental = {.address = incremental, .port = receiver_->incremental().port()};
        network.snapshot = {.address = snapshot, .port = receiver_->snapshot().port()};
        sink_ = std::make_unique<UdpFeedSink>(network);
        FeedPublisherConfig config;
        config.session = "UDP";
        config.snapshot_spacing = kSpacing;
        publisher_ = std::make_unique<FeedPublisher>(*sink_, config);
        publisher_->add_symbol({.symbol = ticker("AAPL"),
                                .tick_size = kTick,
                                .lot_size = 1,
                                .max_levels_per_side = 256});
        publisher_->add_symbol({.symbol = ticker("MSFT"),
                                .tick_size = kTick,
                                .lot_size = 1,
                                .max_levels_per_side = 256});
    }

    // A second receiver on the same groups and ports, as a late joiner would have.
    [[nodiscard]] std::unique_ptr<FeedReceiver> another_receiver(const char* incremental,
                                                                 const char* snapshot) const {
        return std::make_unique<FeedReceiver>(receiver_config(
            incremental, snapshot, receiver_->incremental().port(), receiver_->snapshot().port()));
    }

    // Sends the next snapshot, so a receiver that waits for one gets it.
    void next_snapshot() {
        now_ += kSpacing;
        publisher_->tick(now_);
    }

    // True when the receiver has every stock live and equal to the publisher's copy.
    [[nodiscard]] bool in_step(const FeedReceiver& receiver) const {
        const FeedHandler& h = receiver.handler();
        // A late joiner never sees Start of Messages, so having learned the number of
        // stocks from a snapshot counts as having heard the feed too.
        const bool heard_the_feed = h.started() || h.symbol_count().has_value();
        if (!heard_the_feed || h.recovering()) {
            return false;
        }
        for (std::size_t i = 0; i < publisher_->symbol_count(); ++i) {
            const auto l = static_cast<Locate>(i);
            if (!h.live(l) || !(*h.book(l) == publisher_->copy(l))) {
                return false;
            }
        }
        return true;
    }

    // Reads both receivers' sockets and calls `between` (for example next_snapshot) until
    // `done` is true or five seconds pass.
    bool pump_until(
        const std::vector<FeedReceiver*>& receivers, const std::function<bool()>& done,
        const std::function<void()>& between = [] {}) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            for (FeedReceiver* r : receivers) {
                while (r->poll_once(0) > 0) {
                }
            }
            if (done()) {
                return true;
            }
            between();
            for (FeedReceiver* r : receivers) {
                r->poll_once(1);
            }
        }
        return false;
    }

    // Reads until nothing more arrives for 10 ms.
    static void drain(FeedReceiver& receiver) {
        while (receiver.poll_once(10) > 0) {
        }
    }

    // Some resting levels on both stocks and sides, sent in packets of `per_packet` events.
    void publish_levels(int count, int per_packet) {
        for (int i = 0; i < count; ++i) {
            const auto locate = static_cast<Locate>(i % 2);
            const Side side = (i / 2) % 2 == 0 ? Side::Buy : Side::Sell;
            const Price price = (side == Side::Buy ? 1'000 - (i % 50) : 1'100 + (i % 50)) * kTick;
            publisher_->on_event(level_event(locate, side, price, 100U + static_cast<unsigned>(i),
                                             1U + static_cast<unsigned>(i % 3), now_));
            if ((i + 1) % per_packet == 0) {
                publisher_->flush(now_);
            }
        }
        publisher_->flush(now_);
    }

    Timestamp now_ = 1'000;
    std::unique_ptr<FeedReceiver> receiver_;
    std::unique_ptr<UdpFeedSink> sink_;
    std::unique_ptr<FeedPublisher> publisher_;
};

TEST_F(FeedReceiverTest, AnUnbrokenFeedBuildsTheSameBooks) {
    make("239.255.78.1", "239.255.78.2");
    publisher_->start(now_);
    publish_levels(200, 5);
    publisher_->on_event(trade_event(0, Side::Buy, 1'100 * kTick, 50, 1, now_));
    publisher_->flush(now_);

    ASSERT_TRUE(pump_until({receiver_.get()}, [&] {
        return in_step(*receiver_) && receiver_->handler().stats().trades == 1;
    }));
    const FeedHandlerStats& s = receiver_->handler().stats();
    EXPECT_EQ(s.gaps, 0U);
    EXPECT_EQ(s.applied, 200U);  // Only level updates count as applied.
    EXPECT_EQ(receiver_->incremental().stats().packets,
              sink_->sender(FeedStream::Incremental).stats().sent);

    publisher_->end_session(now_);
    ASSERT_TRUE(pump_until({receiver_.get()}, [&] { return receiver_->handler().ended(); }));
    EXPECT_EQ(receiver_->stats().truncated, 0U);
    EXPECT_EQ(receiver_->stats().failed, 0U);
}

// A receive buffer far too small for a burst makes the kernel drop real packets. The
// receiver must notice the gap and get back in step from the snapshot stream.
TEST_F(FeedReceiverTest, PacketsTheKernelDroppedAreRecoveredFromSnapshots) {
    make("239.255.78.3", "239.255.78.4", 4096);
    publisher_->start(now_);
    // Nobody reads while the burst goes out, so most of it cannot fit in the buffer.
    publish_levels(4'000, 10);
    // One more packet after the reader has caught up shows where the stream is now.
    drain(*receiver_);
    publisher_->on_event(level_event(1, Side::Buy, 900 * kTick, 5, 1, now_));
    publisher_->flush(now_);

    ASSERT_TRUE(pump_until(
        {receiver_.get()}, [&] { return in_step(*receiver_); }, [&] { next_snapshot(); }));
    const FeedHandlerStats& s = receiver_->handler().stats();
    EXPECT_GE(s.gaps, 1U);  // The kernel really dropped packets.
    EXPECT_GE(s.snapshots_applied, 2U);
    EXPECT_GE(s.recoveries, 1U);
}

TEST_F(FeedReceiverTest, ALateJoinerGetsInStepFromSnapshots) {
    make("239.255.78.5", "239.255.78.6");
    publisher_->start(now_);
    publish_levels(100, 4);
    ASSERT_TRUE(pump_until({receiver_.get()}, [&] { return in_step(*receiver_); }));

    // It joins after the start, so it missed the directory and the first levels.
    auto late = another_receiver("239.255.78.5", "239.255.78.6");
    publish_levels(60, 4);
    ASSERT_TRUE(pump_until(
        {receiver_.get(), late.get()}, [&] { return in_step(*receiver_) && in_step(*late); },
        [&] { next_snapshot(); }));
    EXPECT_GE(late->handler().stats().snapshots_applied, 2U);
    EXPECT_EQ(late->handler().symbol_count(), std::optional<std::uint16_t>(2));
    ASSERT_TRUE(late->handler().directory(1).has_value());
    EXPECT_EQ(litbook::testing::unwrap(late->handler().directory(1)).symbol, ticker("MSFT"));
    EXPECT_EQ(receiver_->handler().stats().gaps, 0U);  // The first receiver was never behind.
}

TEST_F(FeedReceiverTest, AnOversizedPacketIsCountedAndDropped) {
    make("239.255.78.7", "239.255.78.8");
    MulticastSender stray(MulticastSenderConfig{
        .group = {.address = "239.255.78.7", .port = receiver_->incremental().port()}});
    ASSERT_EQ(stray.send(std::vector<std::byte>(kReceiveBufferLength + 100)), SendOutcome::Sent);
    ASSERT_TRUE(pump_until({receiver_.get()}, [&] { return receiver_->stats().truncated == 1; }));
    EXPECT_EQ(receiver_->handler().stats().incremental_packets, 0U);
}

// Passes packets on to another sink only while connected is true, like a network cable.
class SwitchSink final : public FeedSink {
public:
    explicit SwitchSink(FeedSink& target) : target_(&target) {}
    void send(FeedStream stream, std::span<const std::byte> packet) override {
        if (connected) {
            target_->send(stream, packet);
        }
    }
    bool connected = true;

private:
    FeedSink* target_;
};

// The silence tests use the fake clock. Each one starts it at an hour, far from 0.
class FeedSilenceTest : public FeedReceiverTest {
protected:
    static constexpr Timestamp kStart = 3'600'000'000'000;

    void make_silent_capable(const char* incremental, const char* snapshot) {
        g_fake_now = kStart;
        make(incremental, snapshot, 0, &fake_now);
    }

    // Moves the fake clock, then lets the receiver look at it once.
    void advance_and_poll(Timestamp by) {
        g_fake_now += by;
        receiver_->poll_once(0);
    }
};

TEST_F(FeedSilenceTest, NoFeedAtAllIsNeverSilence) {
    make_silent_capable("239.255.78.13", "239.255.78.14");
    advance_and_poll(10 * kDefaultSilenceLimit);
    EXPECT_FALSE(receiver_->silent());
    EXPECT_EQ(receiver_->last_heard(), 0U);
    EXPECT_EQ(receiver_->stats().silences, 0U);
}

TEST_F(FeedSilenceTest, ALostEndOfSessionShowsAsSilenceAtExactlyTheLimit) {
    make_silent_capable("239.255.78.15", "239.255.78.16");
    publisher_->start(now_);
    publish_levels(20, 5);
    ASSERT_TRUE(pump_until({receiver_.get()}, [&] { return in_step(*receiver_); }));
    EXPECT_EQ(receiver_->last_heard(), kStart);

    // The publisher stops here without End of Session, as if those packets were lost.
    advance_and_poll(kDefaultSilenceLimit - 1);
    EXPECT_FALSE(receiver_->silent());
    advance_and_poll(1);
    EXPECT_TRUE(receiver_->silent());
    EXPECT_EQ(receiver_->stats().silences, 1U);
    EXPECT_FALSE(receiver_->handler().ended());

    // Silence changes nothing in the books. They are still right as of the last message.
    EXPECT_TRUE(in_step(*receiver_));
    advance_and_poll(kDefaultSilenceLimit);
    EXPECT_EQ(receiver_->stats().silences, 1U);  // Still the same silence.
}

TEST_F(FeedSilenceTest, ThePacketAfterASilenceClearsIt) {
    make_silent_capable("239.255.78.17", "239.255.78.18");
    publisher_->start(now_);
    drain(*receiver_);
    advance_and_poll(kDefaultSilenceLimit);
    ASSERT_TRUE(receiver_->silent());

    // A heartbeat is enough, because it proves the feed is alive.
    now_ += 2'000'000'000;
    publisher_->tick(now_);
    ASSERT_TRUE(pump_until({receiver_.get()}, [&] { return !receiver_->silent(); }));
    EXPECT_EQ(receiver_->last_heard(), g_fake_now);
    EXPECT_EQ(receiver_->handler().stats().gaps, 0U);

    // A second silence is counted again.
    advance_and_poll(kDefaultSilenceLimit);
    EXPECT_TRUE(receiver_->silent());
    EXPECT_EQ(receiver_->stats().silences, 2U);
}

TEST_F(FeedSilenceTest, AFeedThatComesBackAfterLosingPacketsRecoversAsUsual) {
    make_silent_capable("239.255.78.19", "239.255.78.20");
    // The fixture's publisher sends through this switch, so the test can cut the network.
    SwitchSink cable(*sink_);
    FeedPublisherConfig config;
    config.session = "UDP";
    config.snapshot_spacing = kSpacing;
    publisher_ = std::make_unique<FeedPublisher>(cable, config);
    publisher_->add_symbol(
        {.symbol = ticker("AAPL"), .tick_size = kTick, .lot_size = 1, .max_levels_per_side = 256});
    publisher_->add_symbol(
        {.symbol = ticker("MSFT"), .tick_size = kTick, .lot_size = 1, .max_levels_per_side = 256});
    publisher_->start(now_);
    publish_levels(10, 5);
    ASSERT_TRUE(pump_until({receiver_.get()}, [&] { return in_step(*receiver_); }));

    // The network fails. The exchange keeps going, and the receiver goes silent.
    cable.connected = false;
    publish_levels(10, 5);
    advance_and_poll(kDefaultSilenceLimit);
    ASSERT_TRUE(receiver_->silent());

    // The network is back. The next packets show the gap, and snapshots repair the books.
    cable.connected = true;
    publish_levels(4, 2);
    ASSERT_TRUE(pump_until(
        {receiver_.get()}, [&] { return in_step(*receiver_) && !receiver_->silent(); },
        [&] { next_snapshot(); }));
    EXPECT_EQ(receiver_->handler().stats().gaps, 1U);
    EXPECT_EQ(receiver_->handler().stats().recoveries, 1U);
    publisher_.reset();  // It sends through cable, which goes away first.
}

TEST_F(FeedSilenceTest, ALimitOfZeroTurnsItOff) {
    g_fake_now = kStart;
    FeedReceiverConfig c = receiver_config("239.255.78.21", "239.255.78.22", 0, 0, 0, &fake_now);
    c.silence_limit = 0;
    receiver_ = std::make_unique<FeedReceiver>(c);
    MulticastSender sender(MulticastSenderConfig{
        .group = {.address = "239.255.78.21", .port = receiver_->incremental().port()}});
    PacketBuilder builder(unwrap_session("UDP"), kMaxPacketLength);
    ASSERT_EQ(sender.send(builder.heartbeat(1)), SendOutcome::Sent);
    ASSERT_TRUE(pump_until({receiver_.get()}, [&] { return receiver_->last_heard() != 0; }));
    advance_and_poll(100 * kDefaultSilenceLimit);
    EXPECT_FALSE(receiver_->silent());
}

TEST_F(FeedSilenceTest, AnotherSessionsPacketsDoNotCountAsHearingTheFeed) {
    make_silent_capable("239.255.78.23", "239.255.78.24");
    MulticastSender stray(MulticastSenderConfig{
        .group = {.address = "239.255.78.23", .port = receiver_->incremental().port()}});
    PacketBuilder other(unwrap_session("OTHER"), kMaxPacketLength);
    ASSERT_EQ(stray.send(other.heartbeat(1)), SendOutcome::Sent);
    ASSERT_TRUE(pump_until({receiver_.get()},
                           [&] { return receiver_->handler().stats().wrong_session == 1; }));
    EXPECT_EQ(receiver_->last_heard(), 0U);
    advance_and_poll(kDefaultSilenceLimit);
    EXPECT_FALSE(receiver_->silent());
}

TEST(FeedReceiverSetupTest, BadSettingsThrow) {
    FeedReceiverConfig c = receiver_config("239.255.78.9", "239.255.78.10");
    c.max_packets_per_poll = 0;
    EXPECT_THROW(FeedReceiver{c}, std::invalid_argument);
    c = receiver_config("239.255.78.9", "127.0.0.1");
    EXPECT_THROW(FeedReceiver{c}, std::invalid_argument);
    c = receiver_config("239.255.78.9", "239.255.78.10");
    c.handler.max_symbols = 0;
    EXPECT_THROW(FeedReceiver{c}, std::invalid_argument);
    c = receiver_config("239.255.78.9", "239.255.78.10");
    c.clock = nullptr;
    EXPECT_THROW(FeedReceiver{c}, std::invalid_argument);
}

TEST(FeedReceiverSetupTest, PollingWithNothingSentReadsNothing) {
    FeedReceiver r(receiver_config("239.255.78.11", "239.255.78.12"));
    EXPECT_EQ(r.poll_once(0), 0U);
    EXPECT_EQ(r.poll_once(1), 0U);
    EXPECT_EQ(r.stats().polls, 2U);
    EXPECT_FALSE(r.handler().started());
}

}  // namespace
}  // namespace litbook::feed
