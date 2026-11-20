#include "litbook/gateway/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <stdexcept>
#include <string>
#include <system_error>

namespace litbook::gateway {

namespace {

[[noreturn]] void fail(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// Builds an IPv4 socket address. Throws std::invalid_argument for text that is not an
// address like "127.0.0.1".
sockaddr_in ipv4(std::string_view address, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    const std::string text(address);  // inet_pton needs a terminated string.
    if (inet_pton(AF_INET, text.c_str(), &a.sin_addr) != 1) {
        throw std::invalid_argument("not an IPv4 address: " + text);
    }
    return a;
}

// The socket API takes a generic sockaddr pointer, so the IPv4 address has to be passed
// through a cast. This is the one place that does it.
const sockaddr* as_generic(const sockaddr_in* a) noexcept {
    return reinterpret_cast<const sockaddr*>(
        a);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

bool set_int_option(int fd, int level, int name, int value) noexcept {
    return setsockopt(fd, level, name, &value, sizeof(value)) == 0;
}

}  // namespace

void Fd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

Fd listen_tcp(std::string_view address, std::uint16_t port, int backlog) {
    const sockaddr_in a = ipv4(address, port);
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid()) {
        fail("socket");
    }
    // Lets a restarted gateway bind the same port while old connections are in TIME_WAIT.
    if (!set_int_option(fd.get(), SOL_SOCKET, SO_REUSEADDR, 1)) {
        fail("setsockopt(SO_REUSEADDR)");
    }
    if (::bind(fd.get(), as_generic(&a), sizeof(a)) != 0) {
        fail("bind");
    }
    if (::listen(fd.get(), backlog) != 0) {
        fail("listen");
    }
    return fd;
}

std::uint16_t local_port(int fd) {
    sockaddr_in a{};
    socklen_t size = sizeof(a);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &size) != 0) {
        fail("getsockname");
    }
    return ntohs(a.sin_port);
}

Fd connect_tcp(std::string_view address, std::uint16_t port) {
    const sockaddr_in a = ipv4(address, port);
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd.valid()) {
        fail("socket");
    }
    if (::connect(fd.get(), as_generic(&a), sizeof(a)) != 0) {
        fail("connect");
    }
    if (!set_no_delay(fd.get())) {
        fail("setsockopt(TCP_NODELAY)");
    }
    return fd;
}

bool set_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool set_no_delay(int fd) noexcept {
    return set_int_option(fd, IPPROTO_TCP, TCP_NODELAY, 1);
}

bool set_send_buffer(int fd, int bytes) noexcept {
    return set_int_option(fd, SOL_SOCKET, SO_SNDBUF, bytes);
}

bool set_receive_buffer(int fd, int bytes) noexcept {
    return set_int_option(fd, SOL_SOCKET, SO_RCVBUF, bytes);
}

Timestamp nanoseconds_since_midnight() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    constexpr std::uint64_t kSecondsPerDay = 86'400;
    const auto seconds = static_cast<std::uint64_t>(ts.tv_sec) % kSecondsPerDay;
    return seconds * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace litbook::gateway
