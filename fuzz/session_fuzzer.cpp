// Fuzz target for the SoupBinTCP session layer.
//
// The input is a list of records. Each record starts with one op byte and one length
// byte. The op byte picks a connection (op % 3) and an action ((op / 3) % 5).
//
// 0. receive. The next `length` input bytes arrive on the connection.
// 1. write. The network code writes out the connection's output, all of it if the length
//    byte is even and half of it if it is odd.
// 2. tick. The clock moves forward by `length` times 100 ms, and tick() runs.
// 3. drop. The connection closes as if the peer hung up, and a new one opens in its
//    place.
// 4. If the length byte is 255, the exchange ends the session, which is rare on purpose.
//    Otherwise a valid Login Request arrives, for alice if the length byte is even and
//    for bob if it is odd. Without this the fuzzer would have to discover a 49 byte login
//    with the right username and password before any order could flow.
//
// Three connections are open from the start, and two accounts exist (alice with cancel
// on disconnect, bob without). Nothing may crash or read out of bounds, and after every
// record the target checks these properties, aborting if one fails.
//
// 1. The bytes a client has been sent so far, followed by the bytes still queued for it,
//    are a run of whole packets of server types. (The queue on its own may start in the
//    middle of a packet after a partial write, just like a real socket.)
// 2. Each account's connection, if any, is open and logged in as that account.
// 3. Every sequenced message produced was given a sequence number.
// 4. The order entry layer and the books agree on the live orders, and every few
//    records the book invariants hold.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/soup.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "book/test_limits.hpp"
#include "fuzz_target.hpp"

namespace litbook::fuzz {

namespace {

using Manager = gateway::SessionManager<SortedOrderBook>;
using ConnectionId = Manager::ConnectionId;

constexpr std::size_t kConnections = 3;
constexpr OwnerId kAccounts = 2;
constexpr gateway::Timestamp kStep = 100'000'000;  // 100 ms.

void check(bool ok) {
    if (!ok) {
        std::abort();
    }
}

// A whole Login Request packet for one of the two accounts.
std::array<std::byte, gateway::soup::kHeaderSize + gateway::soup::kLoginRequestPayload>
login_packet(bool alice) {
    const auto request = alice ? gateway::soup::make_login_request("alice", "secret", "", 0)
                               : gateway::soup::make_login_request("bob", "hunter2", "", 0);
    std::array<std::byte, gateway::soup::kHeaderSize + gateway::soup::kLoginRequestPayload> out{};
    if (!request) {
        std::abort();
    }
    check(gateway::soup::encode_login_request(*request, out) == out.size());
    return out;
}

gateway::SessionConfig session_config() {
    return gateway::SessionConfig{.session_name = "1",
                                  .max_connections = kConnections,
                                  .input_buffer_bytes = 64,
                                  .output_buffer_bytes = 512,
                                  .output_high_water = 256,
                                  .heartbeat_interval = 10 * kStep,
                                  .idle_timeout = 150 * kStep,
                                  .login_timeout = 300 * kStep};
}

bool is_server_packet(std::byte type) {
    switch (static_cast<gateway::soup::PacketType>(static_cast<char>(type))) {
        case gateway::soup::PacketType::LoginAccepted:
        case gateway::soup::PacketType::LoginRejected:
        case gateway::soup::PacketType::SequencedData:
        case gateway::soup::PacketType::ServerHeartbeat:
        case gateway::soup::PacketType::EndOfSession:
            return true;
        default:
            return false;
    }
}

// Removes the whole packets at the front of `bytes`, checking each one, and returns how
// many bytes that was.
std::size_t whole_packets(std::span<const std::byte> bytes) {
    std::size_t used = 0;
    for (;;) {
        const auto p =
            gateway::soup::next_packet(bytes.subspan(used), gateway::soup::kMaxLengthField);
        if (p.status != gateway::soup::PacketStatus::Complete) {
            return used;
        }
        check(is_server_packet(p.type));
        used += p.size;
    }
}

// The client side of one connection. It keeps the bytes it was sent that do not yet form
// a whole packet.
struct Client {
    std::vector<std::byte> partial;

    void got(std::span<const std::byte> bytes) {
        partial.insert(partial.end(), bytes.begin(), bytes.end());
        partial.erase(partial.begin(),
                      partial.begin() + static_cast<std::ptrdiff_t>(whole_packets(partial)));
    }

    // What the client has, plus what is still queued, must be whole packets.
    void check_with(std::span<const std::byte> queued) const {
        std::vector<std::byte> all = partial;
        all.insert(all.end(), queued.begin(), queued.end());
        check(whole_packets(all) == all.size());
    }
};

void check_state(const Manager& m, const std::array<ConnectionId, kConnections>& ids,
                 const std::array<Client, kConnections>& clients, bool full) {
    for (std::size_t i = 0; i < kConnections; ++i) {
        clients[i].check_with(m.output(ids[i]));
    }
    std::uint64_t numbered = 0;
    std::uint32_t listed = 0;
    for (OwnerId owner = 1; owner <= kAccounts; ++owner) {
        const ConnectionId id = m.connection_of(owner);
        if (id != Manager::kNoConnection) {
            check(m.is_open(id));
            check(m.session(id).owner() == owner);
        }
        numbered += m.next_sequence(owner) - 1;
        listed += m.order_entry().open_orders(owner);
    }
    check(numbered == m.stats().messages_out);
    check(m.stats().messages_dropped <= m.stats().messages_out);
    const auto& engine = m.order_entry().engine();
    check(listed == m.order_entry().live_orders());
    check(engine.book(0)->order_count() == m.order_entry().live_orders());
    if (full) {
        check(engine.check_invariants());
    }
}

}  // namespace

int run_one(const std::uint8_t* data, std::size_t size) {
    Manager m(session_config(), {.max_owners = kAccounts, .max_live_orders = 64, .max_symbols = 1});
    m.add_account({.username = "alice", .password = "secret", .cancel_on_disconnect = true});
    m.add_account({.username = "bob", .password = "hunter2"});
    const auto ticker = make_ticker("AAPL");
    if (!ticker) {
        std::abort();
    }
    m.add_symbol(*ticker, litbook::testing::with_test_limits(BookConfig{.tick_size = 100}));

    gateway::Timestamp now = 0;
    std::array<ConnectionId, kConnections> ids{};
    std::array<Client, kConnections> clients{};
    for (ConnectionId& id : ids) {
        id = m.open(now);
        check(id != Manager::kNoConnection);
    }

    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data), size);
    std::size_t pos = 0;
    std::size_t records = 0;
    while (pos + 2 <= input.size()) {
        const auto op = static_cast<std::uint8_t>(input[pos]);
        const auto length = static_cast<std::uint8_t>(input[pos + 1]);
        pos += 2;
        const std::size_t which = op % kConnections;
        const ConnectionId id = ids[which];
        switch ((op / kConnections) % 5) {
            case 0: {
                const std::size_t n = std::min<std::size_t>(length, input.size() - pos);
                const auto bytes = input.subspan(pos, n);
                pos += n;
                check(m.receive(id, bytes, now) <= n);
                break;
            }
            case 1: {
                const auto out = m.output(id);
                const std::size_t n = length % 2 == 0 ? out.size() : out.size() / 2;
                clients[which].got(out.first(n));
                m.sent(id, n, now);
                break;
            }
            case 2:
                now += static_cast<gateway::Timestamp>(length) * kStep;
                m.tick(now);
                break;
            case 3:
                m.close(id, gateway::CloseReason::PeerClosed, now);
                clients[which] = Client{};
                ids[which] = m.open(now);
                check(ids[which] != Manager::kNoConnection);
                break;
            default:
                if (length == 255) {
                    m.end_session(now);
                } else {
                    const auto packet = login_packet(length % 2 == 0);
                    check(m.receive(id, packet, now) <= packet.size());
                }
                break;
        }
        ++records;
        check_state(m, ids, clients, records % 16 == 0);
    }
    for (std::size_t i = 0; i < kConnections; ++i) {
        m.close(ids[i], gateway::CloseReason::PeerClosed, now);
        clients[i] = Client{};
    }
    check(m.open_connections() == 0);
    // alice asked for cancel on disconnect, so none of her orders survive.
    check(m.order_entry().open_orders(1) == 0);
    check_state(m, ids, clients, true);
    return 0;
}

}  // namespace litbook::fuzz
