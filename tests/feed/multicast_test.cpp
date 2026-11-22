#include "litbook/feed/multicast.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

// Real multicast sockets on the loopback interface. Every receiver binds port 0, so the
// kernel gives each test its own port and tests can run side by side.

namespace litbook::feed {
namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes_of(std::size_t length, std::uint8_t first) {
    Bytes out(length);
    for (std::size_t i = 0; i < length; ++i) {
        out[i] = static_cast<std::byte>(first + i);
    }
    return out;
}

MulticastReceiver make_receiver(const char* group, std::uint16_t port = 0) {
    return MulticastReceiver(MulticastReceiverConfig{
        .interface_name = "lo", .group = {.address = group, .port = port}, .receive_buffer = 0});
}

MulticastSender make_sender(const char* group, std::uint16_t port) {
    return MulticastSender(
        MulticastSenderConfig{.interface_name = "lo", .group = {.address = group, .port = port}});
}

// Waits up to five seconds for the receiver to have something other than Empty, and
// copies the packet out. Loopback delivery is fast, so the limit only matters when
// something is broken.
ReceiveStatus wait_for(MulticastReceiver& receiver, Bytes& packet) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const ReceiveResult r = receiver.receive();
        if (r.status != ReceiveStatus::Empty) {
            packet.assign(r.packet.begin(), r.packet.end());
            return r.status;
        }
        pollfd fd{.fd = receiver.fd(), .events = POLLIN, .revents = 0};
        ::poll(&fd, 1, 10);
    }
    return ReceiveStatus::Empty;
}

int int_option(int fd, int option) {
    int value = -1;
    socklen_t length = sizeof(value);
    if (::getsockopt(fd, IPPROTO_IP, option, &value, &length) != 0) {
        return -1;
    }
    return value;
}

TEST(ParseGroupTest, ReadsAddressAndPort) {
    const MulticastGroup g = parse_group("239.255.0.1:15100");
    EXPECT_EQ(g.address, "239.255.0.1");
    EXPECT_EQ(g.port, 15100);
    EXPECT_EQ(parse_group("224.0.0.1:0").port, 0);
    EXPECT_EQ(parse_group("239.255.255.255:65535").port, 65535);
}

TEST(ParseGroupTest, RefusesAnythingElse) {
    EXPECT_THROW((void)parse_group(""), std::invalid_argument);
    EXPECT_THROW((void)parse_group("239.255.0.1"), std::invalid_argument);
    EXPECT_THROW((void)parse_group(":15100"), std::invalid_argument);
    EXPECT_THROW((void)parse_group("239.255.0.1:"), std::invalid_argument);
    EXPECT_THROW((void)parse_group("239.255.0.1:65536"), std::invalid_argument);
    EXPECT_THROW((void)parse_group("239.255.0.1:15a"), std::invalid_argument);
    EXPECT_THROW((void)parse_group("239.255.0.1:-1"), std::invalid_argument);
    // Unicast addresses on either side of the multicast range.
    EXPECT_THROW((void)parse_group("127.0.0.1:15100"), std::invalid_argument);
    EXPECT_THROW((void)parse_group("223.255.255.255:15100"), std::invalid_argument);
    EXPECT_THROW((void)parse_group("240.0.0.0:15100"), std::invalid_argument);
    EXPECT_THROW((void)parse_group("not.an.address:15100"), std::invalid_argument);
}

TEST(ClassifySendErrorTest, NoRoomIsADropAndAnythingElseAFailure) {
    EXPECT_EQ(classify_send_error(EAGAIN), SendOutcome::Dropped);
    EXPECT_EQ(classify_send_error(EWOULDBLOCK), SendOutcome::Dropped);
    EXPECT_EQ(classify_send_error(ENOBUFS), SendOutcome::Dropped);
    EXPECT_EQ(classify_send_error(EMSGSIZE), SendOutcome::Failed);
    EXPECT_EQ(classify_send_error(EBADF), SendOutcome::Failed);
    EXPECT_EQ(classify_send_error(ECONNREFUSED), SendOutcome::Failed);
}

TEST(MulticastSocketTest, BadSettingsThrow) {
    EXPECT_THROW(MulticastSender(MulticastSenderConfig{.interface_name = "no-such-if",
                                                       .group = {"239.255.77.1", 15100}}),
                 std::invalid_argument);
    EXPECT_THROW(MulticastSender(MulticastSenderConfig{.group = {"127.0.0.1", 15100}}),
                 std::invalid_argument);
    EXPECT_THROW(MulticastSender(MulticastSenderConfig{.group = {"239.255.77.1", 0}}),
                 std::invalid_argument);
    EXPECT_THROW(
        MulticastSender(MulticastSenderConfig{.group = {"239.255.77.1", 15100}, .ttl = 256}),
        std::invalid_argument);
    EXPECT_THROW(
        MulticastSender(MulticastSenderConfig{.group = {"239.255.77.1", 15100}, .ttl = -1}),
        std::invalid_argument);
    EXPECT_THROW(MulticastReceiver(MulticastReceiverConfig{.interface_name = "no-such-if",
                                                           .group = {"239.255.77.1", 0}}),
                 std::invalid_argument);
    EXPECT_THROW(MulticastReceiver(MulticastReceiverConfig{.group = {"10.0.0.1", 0}}),
                 std::invalid_argument);
}

TEST(MulticastSocketTest, TheSenderSetsTheOptionsItWasGiven) {
    const MulticastSender plain = make_sender("239.255.77.1", 15100);
    EXPECT_EQ(int_option(plain.fd(), IP_MULTICAST_TTL), 1);
    EXPECT_EQ(int_option(plain.fd(), IP_MULTICAST_LOOP), 1);
    const MulticastSender other(
        MulticastSenderConfig{.group = {.address = "239.255.77.1", .port = 15100},
                              .ttl = 3,
                              .loop = false,
                              .send_buffer = 0});
    EXPECT_EQ(int_option(other.fd(), IP_MULTICAST_TTL), 3);
    EXPECT_EQ(int_option(other.fd(), IP_MULTICAST_LOOP), 0);
}

TEST(MulticastSocketTest, TheReceiverTurnsOffMulticastAll) {
    const MulticastReceiver r = make_receiver("239.255.77.1");
    EXPECT_EQ(int_option(r.fd(), IP_MULTICAST_ALL), 0);
    EXPECT_NE(r.port(), 0);  // Port 0 asked the kernel for a free one.
}

// The second half of keeping groups apart. Binding to the group address means the kernel
// only matches packets sent to that group, even if IP_MULTICAST_ALL were left on.
TEST(MulticastSocketTest, TheReceiverIsBoundToTheGroupAddress) {
    const MulticastReceiver r = make_receiver("239.255.77.8");
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) The sockets API needs it.
    ASSERT_EQ(::getsockname(r.fd(), reinterpret_cast<sockaddr*>(&local), &length), 0);
    std::array<char, INET_ADDRSTRLEN> text{};
    ASSERT_NE(::inet_ntop(AF_INET, &local.sin_addr, text.data(), text.size()), nullptr);
    EXPECT_STREQ(text.data(), "239.255.77.8");
    EXPECT_EQ(ntohs(local.sin_port), r.port());
}

TEST(MulticastSocketTest, PacketsArriveWholeAndInOrder) {
    MulticastReceiver receiver = make_receiver("239.255.77.2");
    MulticastSender sender = make_sender("239.255.77.2", receiver.port());
    EXPECT_EQ(receiver.receive().status, ReceiveStatus::Empty);  // Nothing sent yet.

    for (std::uint8_t i = 0; i < 3; ++i) {
        ASSERT_EQ(sender.send(bytes_of(100 + i, i)), SendOutcome::Sent);
    }
    for (std::uint8_t i = 0; i < 3; ++i) {
        Bytes got;
        ASSERT_EQ(wait_for(receiver, got), ReceiveStatus::Packet);
        EXPECT_EQ(got, bytes_of(100 + i, i));
    }
    EXPECT_EQ(receiver.receive().status, ReceiveStatus::Empty);
    EXPECT_EQ(sender.stats().sent, 3U);
    EXPECT_EQ(sender.stats().bytes, 303U);
    EXPECT_EQ(receiver.stats().packets, 3U);
    EXPECT_EQ(receiver.stats().bytes, 303U);
}

// The reason receivers bind to the group address and turn IP_MULTICAST_ALL off. Before,
// a socket on the same port heard groups it never joined.
TEST(MulticastSocketTest, TwoGroupsOnOnePortStaySeparate) {
    MulticastReceiver a = make_receiver("239.255.77.3");
    MulticastReceiver b = make_receiver("239.255.77.4", a.port());
    MulticastSender to_b = make_sender("239.255.77.4", a.port());
    ASSERT_EQ(to_b.send(bytes_of(10, 7)), SendOutcome::Sent);

    Bytes got;
    ASSERT_EQ(wait_for(b, got), ReceiveStatus::Packet);
    EXPECT_EQ(got, bytes_of(10, 7));
    // The kernel delivers one loopback packet to every matching socket at the same time,
    // so once b has it, a would have it too if it were going to get it.
    EXPECT_EQ(a.receive().status, ReceiveStatus::Empty);
    EXPECT_EQ(a.stats().packets, 0U);
}

TEST(MulticastSocketTest, TwoReceiversOfOneGroupBothGetEveryPacket) {
    MulticastReceiver first = make_receiver("239.255.77.5");
    MulticastReceiver second = make_receiver("239.255.77.5", first.port());
    MulticastSender sender = make_sender("239.255.77.5", first.port());
    ASSERT_EQ(sender.send(bytes_of(40, 1)), SendOutcome::Sent);
    Bytes got;
    ASSERT_EQ(wait_for(first, got), ReceiveStatus::Packet);
    EXPECT_EQ(got, bytes_of(40, 1));
    ASSERT_EQ(wait_for(second, got), ReceiveStatus::Packet);
    EXPECT_EQ(got, bytes_of(40, 1));
}

TEST(MulticastSocketTest, APacketLongerThanTheBufferIsTruncatedNotShortened) {
    MulticastReceiver receiver = make_receiver("239.255.77.6");
    MulticastSender sender = make_sender("239.255.77.6", receiver.port());
    ASSERT_EQ(sender.send(bytes_of(kReceiveBufferLength + 1, 0)), SendOutcome::Sent);
    ASSERT_EQ(sender.send(bytes_of(kReceiveBufferLength, 0)), SendOutcome::Sent);
    Bytes got;
    EXPECT_EQ(wait_for(receiver, got), ReceiveStatus::Truncated);
    EXPECT_TRUE(got.empty());
    // A packet of exactly the buffer's length still fits.
    ASSERT_EQ(wait_for(receiver, got), ReceiveStatus::Packet);
    EXPECT_EQ(got.size(), kReceiveBufferLength);
    EXPECT_EQ(receiver.stats().truncated, 1U);
    EXPECT_EQ(receiver.stats().packets, 1U);
}

TEST(MulticastSocketTest, APacketTooBigForUdpFailsAndIsCounted) {
    MulticastSender sender = make_sender("239.255.77.7", 15100);
    // The largest UDP payload over IPv4 is 65,507 bytes.
    EXPECT_EQ(sender.send(bytes_of(70'000, 0)), SendOutcome::Failed);
    EXPECT_EQ(sender.stats().failed, 1U);
    EXPECT_EQ(sender.stats().sent, 0U);
    EXPECT_EQ(sender.stats().dropped, 0U);
    EXPECT_EQ(sender.stats().last_error, EMSGSIZE);
}

}  // namespace
}  // namespace litbook::feed
