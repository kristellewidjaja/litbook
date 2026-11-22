#pragma once

// Pieces shared by the latency tools that talk to the gateway over TCP
// (litbook_gateway_latency and litbook_feed_latency). A blocking SoupBinTCP client, the
// login, packet encoding, and the way results are printed. Each printed percentile is the
// median of that percentile over the timed passes, and max is the largest of any pass
// (D-034).

#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/byte_buffer.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/gateway/soup.hpp"
#include "litbook/metrics/latency_histogram.hpp"
#include "litbook/metrics/tick_clock.hpp"
#include "litbook/protocol/codec.hpp"
#include "litbook/protocol/messages.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace litbook::bench {

using metrics::LatencyHistogram;
using metrics::TickClock;

// A blocking client that sends packets and reads reply packets one at a time.
class Client {
public:
    explicit Client(std::uint16_t port)
        : socket_(gateway::connect_tcp("127.0.0.1", port)), in_(4096) {}

    void send(std::span<const std::byte> bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const ssize_t n =
                ::send(socket_.get(), bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n <= 0) {
                throw std::runtime_error("send failed");
            }
            done += static_cast<std::size_t>(n);
        }
    }

    // Blocks until one whole packet other than a heartbeat has arrived and returns its
    // type. A heartbeat can come in whenever the server sent nothing for a second.
    gateway::soup::PacketType wait_packet() {
        for (;;) {
            const auto p =
                gateway::soup::next_packet(in_.readable(), gateway::soup::kMaxLengthField);
            if (p.status == gateway::soup::PacketStatus::Complete) {
                in_.consume(p.size);
                const auto type = static_cast<gateway::soup::PacketType>(static_cast<char>(p.type));
                if (type == gateway::soup::PacketType::ServerHeartbeat) {
                    continue;
                }
                return type;
            }
            if (!in_.make_room(1)) {
                throw std::runtime_error("client buffer full");
            }
            const auto space = in_.writable();
            const ssize_t n = ::recv(socket_.get(), space.data(), space.size(), 0);
            if (n <= 0) {
                throw std::runtime_error("connection closed");
            }
            in_.commit(static_cast<std::size_t>(n));
        }
    }

    void expect_reply(const char* what) {
        if (wait_packet() != gateway::soup::PacketType::SequencedData) {
            throw std::runtime_error(what);
        }
    }

private:
    gateway::Fd socket_;
    gateway::ByteBuffer in_;
};

inline void append_packet(std::vector<std::byte>& out, const protocol::InboundMessage& m) {
    std::array<std::byte, protocol::kMaxMessageLength> frame{};
    const std::size_t n = protocol::encode(m, frame);
    const std::size_t at = out.size();
    out.resize(at + gateway::soup::kHeaderSize + n);
    (void)gateway::soup::encode_packet(gateway::soup::PacketType::UnsequencedData,
                                       std::span<const std::byte>(frame.data(), n),
                                       std::span<std::byte>(out).subspan(at));
}

// The median over passes of one percentile, in microseconds. The values are in ticks.
inline double median_us(const std::vector<const LatencyHistogram*>& passes, double percent) {
    std::vector<std::uint64_t> values;
    values.reserve(passes.size());
    for (const LatencyHistogram* h : passes) {
        values.push_back(h->percentile(percent));
    }
    std::ranges::sort(values);
    return static_cast<double>(TickClock::to_ns(values[values.size() / 2])) / 1000.0;
}

inline void print_row(std::string_view label, const std::vector<const LatencyHistogram*>& passes) {
    std::uint64_t max = 0;
    for (const LatencyHistogram* h : passes) {
        max = std::max(max, h->max());
    }
    std::printf("%-22.*s %8.2f %8.2f %8.2f %8.2f %8.2f %10.2f\n", static_cast<int>(label.size()),
                label.data(), median_us(passes, 50), median_us(passes, 90), median_us(passes, 99),
                median_us(passes, 99.9), median_us(passes, 99.99),
                static_cast<double>(TickClock::to_ns(max)) / 1000.0);
}

inline void log_in(Client& client) {
    std::array<std::byte, gateway::soup::kHeaderSize + gateway::soup::kLoginRequestPayload> login{};
    const auto request = gateway::soup::make_login_request("bench", "bench", "", 0);
    if (!request) {
        throw std::runtime_error("bad login request");
    }
    (void)gateway::soup::encode_login_request(*request, login);
    client.send(login);
    if (client.wait_packet() != gateway::soup::PacketType::LoginAccepted) {
        throw std::runtime_error("login failed");
    }
}

// Reads a whole number that must be at least `low`. Returns nothing for anything else.
inline std::optional<long long> parse_number(std::string_view text, long long low) {
    if (text.empty()) {
        return std::nullopt;
    }
    long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9' || value > 1'000'000'000LL) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    if (value < low) {
        return std::nullopt;
    }
    return value;
}

// Every packet of one pass, one after the other, and where each one starts. Building
// them before the pass keeps the encoding out of the timed loop.
struct PassPackets {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> starts;  // One more entry than there are packets.

    [[nodiscard]] std::size_t count() const { return starts.size() - 1; }
    [[nodiscard]] std::span<const std::byte> range(std::size_t first, std::size_t end) const {
        return std::span<const std::byte>(bytes).subspan(starts[first],
                                                         starts[end] - starts[first]);
    }
};

// n pairs of a buy Enter Order for qty at price, then a Cancel Order for it. UserRefNums
// continue from `next_ref`.
inline PassPackets make_enter_cancel_packets(int n, protocol::UserRefNum& next_ref, Ticker symbol,
                                             Price price, Qty qty) {
    PassPackets packets;
    packets.starts.push_back(0);
    for (int i = 0; i < n; ++i) {
        const protocol::UserRefNum ref = next_ref++;
        append_packet(packets.bytes, protocol::EnterOrder{.user_ref = ref,
                                                          .side = Side::Buy,
                                                          .qty = qty,
                                                          .symbol = symbol,
                                                          .price = price,
                                                          .tif = TimeInForce::GoodTillCancel,
                                                          .type = OrderType::Limit,
                                                          .post_only = false});
        packets.starts.push_back(packets.bytes.size());
        append_packet(packets.bytes, protocol::CancelOrder{.user_ref = ref, .qty = 0});
        packets.starts.push_back(packets.bytes.size());
    }
    return packets;
}

}  // namespace litbook::bench
