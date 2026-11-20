#pragma once

#include "litbook/gateway/session.hpp"

#include <cstdint>
#include <string_view>

// Small helpers around Linux sockets for the gateway, its tests and its benchmark.
namespace litbook::gateway {

// Owns one file descriptor and closes it when destroyed, so a socket can never leak on an
// early return. It can be moved but not copied, because two owners would close the same
// descriptor twice.
//
// The invariant is that get() is either -1 (empty) or a descriptor nobody else closes.
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    // Closes the current descriptor (if any) and takes ownership of `fd`.
    void reset(int fd = -1) noexcept;

    // Gives up ownership without closing and returns the descriptor.
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_ = -1;
};

// The functions that set things up throw std::system_error when a system call fails.
// They are for startup and for test and benchmark clients, never for the message path.

// A non-blocking TCP socket listening on an IPv4 address such as "127.0.0.1". Port 0 asks
// the kernel for any free port, and local_port() says which one it picked.
[[nodiscard]] Fd listen_tcp(std::string_view address, std::uint16_t port, int backlog);

// The port a bound socket is using.
[[nodiscard]] std::uint16_t local_port(int fd);

// A blocking TCP connection to an IPv4 address and port, with TCP_NODELAY turned on.
[[nodiscard]] Fd connect_tcp(std::string_view address, std::uint16_t port);

// These return false if the system call failed.
bool set_nonblocking(int fd) noexcept;
// Turns off Nagle's algorithm, which would otherwise hold back small packets for up to
// tens of milliseconds while it waits to combine them.
bool set_no_delay(int fd) noexcept;
bool set_send_buffer(int fd, int bytes) noexcept;
bool set_receive_buffer(int fd, int bytes) noexcept;

// Nanoseconds since midnight UTC from the system's real time clock, the unit of every
// OUCH timestamp. It jumps back to 0 at midnight, and the session layer treats a clock
// that went backwards as no time passing.
[[nodiscard]] Timestamp nanoseconds_since_midnight() noexcept;

}  // namespace litbook::gateway
