#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

#include "gateway/soup_test_support.hpp"
#include "gateway/test_client.hpp"

// Helpers for tests that talk to a running ThreadedGateway over loopback TCP.

namespace litbook::pipeline::testing {

using gateway::soup::PacketType;
using gateway::testing::Bytes;
using gateway::testing::ServerPacket;
using gateway::testing::TestClient;

// A client plus every packet it has received so far.
struct Peer {
    explicit Peer(std::uint16_t port) : client(std::make_unique<TestClient>(port)) {}

    void collect() {
        client->read_available();
        for (;;) {
            const gateway::soup::Packet p = client->next_packet();
            if (p.status != gateway::soup::PacketStatus::Complete) {
                return;
            }
            packets.push_back(
                ServerPacket{.type = static_cast<PacketType>(static_cast<char>(p.type)),
                             .payload = Bytes(p.payload.begin(), p.payload.end())});
        }
    }

    [[nodiscard]] std::size_t count(PacketType type) const {
        std::size_t n = 0;
        for (const ServerPacket& p : packets) {
            n += p.type == type ? 1 : 0;
        }
        return n;
    }

    template <class Message>
    [[nodiscard]] std::vector<Message> all() const {
        std::vector<Message> out;
        for (const ServerPacket& p : packets) {
            const auto m = p.message();
            if (m && std::holds_alternative<Message>(*m)) {
                out.push_back(std::get<Message>(*m));
            }
        }
        return out;
    }

    void send(const Bytes& bytes) const { ASSERT_EQ(client->send_some(bytes), bytes.size()); }

    std::unique_ptr<TestClient> client;
    std::vector<ServerPacket> packets;
};

// Lets the peers collect what arrived until `done` is true or ten seconds pass. Returns
// whether `done` became true. The time limit only matters when something is broken, and
// sanitizer builds are slow, so it is generous.
inline bool wait_until(const std::vector<Peer*>& peers, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        for (Peer* p : peers) {
            p->collect();
        }
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
}

}  // namespace litbook::pipeline::testing
