#pragma once

#include "litbook/gateway/socket.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// UDP multicast sockets for the market data feed. The publisher sends every packet once
// to a group address, and every receiver that joined the group gets a copy.
//
// The Linux socket options used here, from the ip(7) family of man pages:
//   IP_MULTICAST_IF picks the interface a socket sends on.
//   IP_MULTICAST_LOOP decides whether this host's own sockets also get the packets.
//   IP_MULTICAST_TTL is how many routers a packet may cross. 1 keeps it on the local network.
//   IP_ADD_MEMBERSHIP joins a group on one interface.
//   IP_MULTICAST_ALL is 1 by default, which delivers every group that any socket on the
//   host joined to every socket bound to the same port. Receivers set it to 0.
//
// Both sides name the interface (for example "lo" or "eth0") instead of letting the
// routing table choose, because the route differs between machines. Linux carries
// multicast on lo even though lo does not have the MULTICAST flag, as long as the socket
// asks for lo by its index.

namespace litbook::feed {

// An IPv4 multicast group address and a UDP port, for example 239.255.0.1 and 15100.
struct MulticastGroup {
    std::string address;
    std::uint16_t port = 0;
};

// Parses "ADDRESS:PORT", for example "239.255.0.1:15100". Throws std::invalid_argument if
// the text is not in that form, the address is not a multicast address (224.0.0.0 to
// 239.255.255.255), or the port is not a number from 0 to 65535.
[[nodiscard]] MulticastGroup parse_group(std::string_view text);

struct MulticastSenderConfig {
    std::string interface_name = "lo";
    MulticastGroup group;  // The port must not be 0.
    int ttl = 1;
    bool loop = true;     // Deliver to sockets on this host too.
    int send_buffer = 0;  // SO_SNDBUF in bytes, or 0 to keep the kernel's default.
};

// What happened to one packet handed to MulticastSender::send.
enum class SendOutcome : std::uint8_t {
    Sent,
    Dropped,  // The kernel had no room right now. UDP loses it, receivers recover.
    Failed,   // Any other error, such as a packet too big for UDP.
};

// Sorts an errno from send() into Dropped or Failed. EAGAIN, EWOULDBLOCK and ENOBUFS mean
// the kernel had no room for the packet at that moment.
[[nodiscard]] SendOutcome classify_send_error(int error) noexcept;

struct SenderStats {
    std::uint64_t sent = 0;
    std::uint64_t bytes = 0;
    std::uint64_t dropped = 0;
    std::uint64_t failed = 0;
    int last_error = 0;  // The errno of the last Dropped or Failed send.
};

// Sends UDP packets to one multicast group. The socket is connected to the group, so a
// send is one send() system call with no address to look up. It is non-blocking, so a
// full kernel buffer drops the packet instead of stalling the publisher thread.
//
// Invariants. The socket is always open and connected to the group. Every send() call
// adds one to exactly one of sent, dropped and failed.
class MulticastSender {
public:
    // Throws std::invalid_argument for an unknown interface, a group that is not
    // multicast, port 0 or a TTL outside 0 to 255, and std::system_error if a socket call
    // fails.
    explicit MulticastSender(const MulticastSenderConfig& config);

    SendOutcome send(std::span<const std::byte> packet) noexcept;

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] const SenderStats& stats() const noexcept { return stats_; }

private:
    gateway::Fd fd_;
    SenderStats stats_;
};

struct MulticastReceiverConfig {
    std::string interface_name = "lo";
    // Port 0 lets the kernel pick a free port, which port() then reports. Senders must be
    // told that port. Tests use it so that runs side by side never share a port.
    MulticastGroup group;
    int receive_buffer = 0;  // SO_RCVBUF in bytes, or 0 to keep the kernel's default.
};

// Room for the largest feed packet plus more, so a longer packet shows up as Truncated
// instead of passing as a shorter one.
inline constexpr std::size_t kReceiveBufferLength = 2048;

enum class ReceiveStatus : std::uint8_t {
    Packet,     // One whole packet was read.
    Empty,      // Nothing is waiting.
    Truncated,  // A packet longer than the buffer was read and cut off. It is unusable.
    Failed,     // Any other error.
};

struct ReceiveResult {
    ReceiveStatus status = ReceiveStatus::Empty;
    std::span<const std::byte> packet;  // Valid until the next receive() call.
};

struct ReceiverStats {
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;
    std::uint64_t truncated = 0;
    std::uint64_t failed = 0;
};

// Receives the packets of one multicast group. The socket is bound to the group address
// itself and has IP_MULTICAST_ALL turned off, so it gets only its own group, even when
// other sockets on the host listen to other groups on the same port. SO_REUSEADDR lets
// several receivers on one host (a late joiner next to a running one) share the port.
//
// Invariants. The socket is non-blocking and a member of the group on the named
// interface. Every receive() call that returns something other than Empty adds one to
// exactly one of packets, truncated and failed.
class MulticastReceiver {
public:
    // Throws std::invalid_argument for an unknown interface or a group that is not
    // multicast, and std::system_error if a socket call fails.
    explicit MulticastReceiver(const MulticastReceiverConfig& config);

    // Reads one waiting packet without blocking.
    ReceiveResult receive() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] const ReceiverStats& stats() const noexcept { return stats_; }

private:
    gateway::Fd fd_;
    std::uint16_t port_ = 0;
    ReceiverStats stats_;
    std::array<std::byte, kReceiveBufferLength> buffer_{};
};

}  // namespace litbook::feed
