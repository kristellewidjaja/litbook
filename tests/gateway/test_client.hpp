#pragma once

// A small non-blocking TCP client for gateway tests and benchmarks. It sends bytes, reads
// whatever the server has sent, and splits that into SoupBinTCP packets. A single test
// thread can drive both the gateway (poll_once) and any number of these clients without
// ever blocking.

#include "litbook/gateway/byte_buffer.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/gateway/soup.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace litbook::gateway::testing {

class TestClient {
public:
    // Connects (blocking, so the connection is set up when this returns) and then makes
    // the socket non-blocking. The receive buffer is allocated here and never grows.
    explicit TestClient(std::uint16_t port, std::size_t buffer_bytes = std::size_t{1} << 20,
                        int socket_receive_buffer = 0)
        : socket_(connect_tcp("127.0.0.1", port)), in_(buffer_bytes) {
        if (socket_receive_buffer > 0 &&
            !set_receive_buffer(socket_.get(), socket_receive_buffer)) {
            throw std::runtime_error("setsockopt(SO_RCVBUF) failed");
        }
        if (!set_nonblocking(socket_.get())) {
            throw std::runtime_error("could not make the client socket non-blocking");
        }
    }

    // Sends as much as the kernel takes right now and returns how many bytes that was.
    std::size_t send_some(std::span<const std::byte> bytes) noexcept {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n =
                ::send(socket_.get(), bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n > 0) {
                done += static_cast<std::size_t>(n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break;  // EAGAIN (the kernel buffer is full) or a broken connection.
            }
        }
        return done;
    }

    // Reads everything that has arrived. Returns false once the server has closed the
    // connection (or it broke). Stops early if the receive buffer is full.
    bool read_available() noexcept {
        while (!closed_ && in_.make_room(1)) {
            const auto space = in_.writable();
            const ssize_t n = ::recv(socket_.get(), space.data(), space.size(), 0);
            if (n > 0) {
                in_.commit(static_cast<std::size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else {
                closed_ = true;  // 0 bytes is an orderly close, anything else an error.
            }
        }
        return !closed_;
    }

    // The next whole packet received, if there is one. Its payload is valid until the
    // next call to next_packet() or read_available().
    [[nodiscard]] soup::Packet next_packet() noexcept {
        const soup::Packet p = soup::next_packet(in_.readable(), soup::kMaxLengthField);
        if (p.status == soup::PacketStatus::Complete) {
            in_.consume(p.size);
        }
        return p;
    }

    // Drops the client's end of the connection.
    void disconnect() noexcept { socket_.reset(); }

    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return in_.size(); }
    [[nodiscard]] int fd() const noexcept { return socket_.get(); }

private:
    Fd socket_;
    ByteBuffer in_;
    bool closed_ = false;
};

}  // namespace litbook::gateway::testing
