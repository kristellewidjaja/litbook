#include "litbook/feed/multicast.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <charconv>
#include <stdexcept>
#include <system_error>

#include <net/if.h>

namespace litbook::feed {

namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// The group address in network byte order. Throws if it is not an IPv4 multicast address.
in_addr multicast_address(const std::string& text) {
    in_addr address{};
    if (::inet_pton(AF_INET, text.c_str(), &address) != 1) {
        throw std::invalid_argument("not an IPv4 address: " + text);
    }
    // Multicast addresses are 224.0.0.0 to 239.255.255.255, so the top four bits are 1110.
    const std::uint32_t host_order = ntohl(address.s_addr);
    if ((host_order >> 28U) != 0xEU) {
        throw std::invalid_argument("not a multicast address: " + text);
    }
    return address;
}

// The index of a network interface such as "lo". Throws if there is no such interface.
unsigned interface_index(const std::string& name) {
    const unsigned index = ::if_nametoindex(name.c_str());
    if (index == 0) {
        throw std::invalid_argument("unknown network interface: " + name);
    }
    return index;
}

// A non-blocking UDP socket. Throws if it cannot be made.
gateway::Fd udp_socket() {
    gateway::Fd fd(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid()) {
        throw_errno("socket");
    }
    return fd;
}

void set_int_option(int fd, int level, int option, int value, const char* what) {
    if (::setsockopt(fd, level, option, &value, sizeof(value)) != 0) {
        throw_errno(what);
    }
}

sockaddr_in socket_address(in_addr address, std::uint16_t port) {
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_addr = address;
    out.sin_port = htons(port);
    return out;
}

}  // namespace

MulticastGroup parse_group(std::string_view text) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) {
        throw std::invalid_argument("expected ADDRESS:PORT, got " + std::string(text));
    }
    MulticastGroup group;
    group.address = std::string(text.substr(0, colon));
    (void)multicast_address(group.address);  // Throws if it is not multicast.

    const std::string_view port_text = text.substr(colon + 1);
    unsigned port = 0;
    const auto [end, error] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (error != std::errc{} || end != port_text.data() + port_text.size() || port > 65535) {
        throw std::invalid_argument("bad port in " + std::string(text));
    }
    group.port = static_cast<std::uint16_t>(port);
    return group;
}

SendOutcome classify_send_error(int error) noexcept {
    // EWOULDBLOCK has the same value as EAGAIN on Linux, but POSIX allows them to differ.
    if (error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS) {
        return SendOutcome::Dropped;
    }
    return SendOutcome::Failed;
}

MulticastSender::MulticastSender(const MulticastSenderConfig& config) {
    const in_addr group = multicast_address(config.group.address);
    if (config.group.port == 0) {
        throw std::invalid_argument("a multicast sender needs a port above 0");
    }
    if (config.ttl < 0 || config.ttl > 255) {
        throw std::invalid_argument("multicast TTL must be 0 to 255");
    }
    const unsigned index = interface_index(config.interface_name);

    fd_ = udp_socket();
    // ip_mreqn names the interface by its index. The group and local address fields are
    // not used by IP_MULTICAST_IF and stay zero.
    ip_mreqn interface{};
    interface.imr_ifindex = static_cast<int>(index);
    if (::setsockopt(fd_.get(), IPPROTO_IP, IP_MULTICAST_IF, &interface, sizeof(interface)) != 0) {
        throw_errno("setsockopt IP_MULTICAST_IF");
    }
    set_int_option(fd_.get(), IPPROTO_IP, IP_MULTICAST_TTL, config.ttl,
                   "setsockopt IP_MULTICAST_TTL");
    set_int_option(fd_.get(), IPPROTO_IP, IP_MULTICAST_LOOP, config.loop ? 1 : 0,
                   "setsockopt IP_MULTICAST_LOOP");
    if (config.send_buffer > 0 && !gateway::set_send_buffer(fd_.get(), config.send_buffer)) {
        throw_errno("setsockopt SO_SNDBUF");
    }

    // Connecting a UDP socket only stores the destination, so each send() needs no address.
    const sockaddr_in destination = socket_address(group, config.group.port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) The sockets API needs it.
    if (::connect(fd_.get(), reinterpret_cast<const sockaddr*>(&destination),
                  sizeof(destination)) != 0) {
        throw_errno("connect to multicast group");
    }
}

SendOutcome MulticastSender::send(std::span<const std::byte> packet) noexcept {
    const ssize_t n = ::send(fd_.get(), packet.data(), packet.size(), MSG_NOSIGNAL);
    if (n >= 0) {
        // A datagram socket sends the whole packet or nothing.
        ++stats_.sent;
        stats_.bytes += static_cast<std::uint64_t>(n);
        return SendOutcome::Sent;
    }
    stats_.last_error = errno;
    const SendOutcome outcome = classify_send_error(errno);
    if (outcome == SendOutcome::Dropped) {
        ++stats_.dropped;
    } else {
        ++stats_.failed;
    }
    return outcome;
}

MulticastReceiver::MulticastReceiver(const MulticastReceiverConfig& config) {
    const in_addr group = multicast_address(config.group.address);
    const unsigned index = interface_index(config.interface_name);

    fd_ = udp_socket();
    set_int_option(fd_.get(), SOL_SOCKET, SO_REUSEADDR, 1, "setsockopt SO_REUSEADDR");
    set_int_option(fd_.get(), IPPROTO_IP, IP_MULTICAST_ALL, 0, "setsockopt IP_MULTICAST_ALL");
    if (config.receive_buffer > 0 &&
        !gateway::set_receive_buffer(fd_.get(), config.receive_buffer)) {
        throw_errno("setsockopt SO_RCVBUF");
    }

    // Binding to the group address, not to any address, means the kernel only hands this
    // socket packets sent to that group.
    const sockaddr_in local = socket_address(group, config.group.port);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) The sockets API needs it.
    if (::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        throw_errno("bind to multicast group");
    }
    port_ = gateway::local_port(fd_.get());

    ip_mreqn membership{};
    membership.imr_multiaddr = group;
    membership.imr_ifindex = static_cast<int>(index);
    if (::setsockopt(fd_.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) !=
        0) {
        throw_errno("setsockopt IP_ADD_MEMBERSHIP");
    }
}

ReceiveResult MulticastReceiver::receive() noexcept {
    // With MSG_TRUNC a datagram socket returns the packet's real length even when it was
    // longer than the buffer, which is how a cut packet is noticed.
    const ssize_t n = ::recv(fd_.get(), buffer_.data(), buffer_.size(), MSG_TRUNC);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ReceiveResult{};
        }
        ++stats_.failed;
        return ReceiveResult{.status = ReceiveStatus::Failed, .packet = {}};
    }
    const auto length = static_cast<std::size_t>(n);
    if (length > buffer_.size()) {
        ++stats_.truncated;
        return ReceiveResult{.status = ReceiveStatus::Truncated, .packet = {}};
    }
    ++stats_.packets;
    stats_.bytes += length;
    return ReceiveResult{.status = ReceiveStatus::Packet,
                         .packet = std::span<const std::byte>(buffer_.data(), length)};
}

}  // namespace litbook::feed
