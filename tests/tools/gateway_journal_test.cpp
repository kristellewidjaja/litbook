// End-to-end tests of litbook_gateway with --journal. The smoke tests start the real
// program, trade a little, stop it and start it again on the same journal. The crash test
// kills it with SIGKILL again and again at random points and checks after every kill that
// the journal explains every reply any client saw, and after every restart that the
// accounts carry on with the right sequence numbers.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/replay.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gateway/soup_test_support.hpp"
#include "gateway/test_client.hpp"
#include "journal/replay_test_support.hpp"
#include "support/temp_dir.hpp"
#include "support/unwrap.hpp"
#include "tools/child_process.hpp"

namespace litbook::gateway {
namespace {

using testing::Bytes;
using testing::TestClient;

class GatewayProcess : public litbook::testing::ChildProcess {
public:
    explicit GatewayProcess(std::vector<std::string> args)
        : ChildProcess(LITBOOK_GATEWAY, std::move(args)) {}
};

struct Account {
    OwnerId owner;
    const char* username;
    const char* password;
};

// The accounts in the order of their --account flags, so their OwnerIds are 1, 2, 3.
constexpr std::array<Account, 3> kAccounts{
    Account{.owner = 1, .username = "alice", .password = "secret"},
    Account{.owner = 2, .username = "bob", .password = "hunter2"},
    Account{.owner = 3, .username = "carol", .password = "pw"}};

std::vector<std::string> gateway_args(const std::string& journal) {
    return {"--port",           "0",         "--journal",   journal,     "--account",
            "alice:secret:cod", "--account", "bob:hunter2", "--account", "carol:pw",
            "--symbol",         "AAPL",      "--symbol",    "MSFT"};
}

// A client of one account, which remembers the sequence number of every message it got.
struct Client {
    Client(std::uint16_t port, const Account& a) : account(a), tcp(port) {}

    // Logs in and waits for the answer. Returns false if the login was not accepted.
    bool log_in() {
        (void)tcp.send_some(testing::login(account.username, account.password));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            tcp.read_available();
            const soup::Packet p = tcp.next_packet();
            if (p.status == soup::PacketStatus::Complete) {
                if (p.type != std::byte{'A'}) {
                    return false;
                }
                const auto accepted = soup::decode_login_accepted(p.payload);
                if (!accepted) {
                    return false;
                }
                first_sequence = accepted->sequence;
                return true;
            }
            if (tcp.closed()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return false;
    }

    // Reads every packet that arrived and keeps the Sequenced Data ones by number.
    void collect(std::map<std::uint64_t, Bytes>& seen) {
        tcp.read_available();
        for (;;) {
            const soup::Packet p = tcp.next_packet();
            if (p.status != soup::PacketStatus::Complete) {
                return;
            }
            if (p.type == std::byte{'S'}) {
                seen[first_sequence + received] = Bytes(p.payload.begin(), p.payload.end());
                ++received;
            }
        }
    }

    Account account;
    TestClient tcp;
    std::uint64_t first_sequence = 0;
    std::uint64_t received = 0;
};

// Every reply that replaying the journal gives, per account, in order.
struct Replayed {
    std::map<OwnerId, std::vector<Bytes>> replies;
    std::uint32_t live_orders = 0;
    journal::TailStatus tail = journal::TailStatus::BadHeader;
};

Replayed replay_file(const std::string& path) {
    Replayed out;
    journal::testing::RecordingReplies replies;
    journal::Replayer<SortedOrderBook> replayer(replies);
    const journal::MappedFile file(path);
    const journal::ReplayOutcome outcome = journal::replay_journal(file.bytes(), replayer);
    EXPECT_EQ(outcome.status, journal::ReplayStatus::Ok);
    out.tail = outcome.scan.tail;
    for (const auto& r : replies.replies) {
        out.replies[r.owner].push_back(r.bytes);
    }
    if (replayer.started()) {
        out.live_orders = replayer.order_entry().live_orders();
    }
    return out;
}

protocol::EnterOrder random_order(std::mt19937_64& rng, protocol::UserRefNum ref) {
    const bool aapl = (rng() % 2) == 0;
    const auto offset = static_cast<Price>(rng() % 11) - 5;
    return protocol::EnterOrder{
        .user_ref = ref,
        .side = (rng() % 2) == 0 ? Side::Buy : Side::Sell,
        .qty = static_cast<Qty>(1 + rng() % 100),
        .symbol = litbook::testing::unwrap(make_ticker(aapl ? "AAPL" : "MSFT")),
        .price = 1'000'000 + offset * 100,
        .tif = TimeInForce::GoodTillCancel,
        .type = OrderType::Limit,
        .post_only = false};
}

// Starts the gateway, logs every account in and checks that each one's next sequence
// number is 1 plus the replies the journal gives it. Returns the clients.
std::vector<std::unique_ptr<Client>> start_and_log_in(GatewayProcess& gateway,
                                                      const std::string& journal, bool expect_new) {
    const std::uint16_t port = litbook::testing::gateway_port(gateway);
    const std::string recovery = gateway.line();
    EXPECT_NE(recovery.find(expect_new ? ": new" : ": recovered"), std::string::npos) << recovery;
    // The recovery records are on disk before the program prints its lines.
    const Replayed before = replay_file(journal);
    std::vector<std::unique_ptr<Client>> clients;
    for (const Account& a : kAccounts) {
        auto c = std::make_unique<Client>(port, a);
        EXPECT_TRUE(c->log_in()) << a.username;
        const auto it = before.replies.find(a.owner);
        const std::uint64_t produced = it == before.replies.end() ? 0 : it->second.size();
        EXPECT_EQ(c->first_sequence, produced + 1) << a.username;
        clients.push_back(std::move(c));
    }
    return clients;
}

TEST(GatewayJournalProgram, ARestartCarriesOnTheBookAndTheSequenceNumbers) {
    const litbook::testing::TempDir dir;
    const std::string journal = dir.file("exchange.journal");
    std::map<OwnerId, std::map<std::uint64_t, Bytes>> seen;
    {
        GatewayProcess gateway(gateway_args(journal));
        auto clients = start_and_log_in(gateway, journal, true);
        // bob rests an order, which a clean stop leaves in the book (no cancel on disconnect).
        std::mt19937_64 rng(1);
        protocol::EnterOrder sell = random_order(rng, 1);
        sell.side = Side::Sell;
        (void)clients[1]->tcp.send_some(testing::unsequenced(sell));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (clients[1]->received == 0 && std::chrono::steady_clock::now() < deadline) {
            clients[1]->collect(seen[2]);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        ASSERT_EQ(clients[1]->received, 1U);
        EXPECT_EQ(gateway.stop(SIGTERM), 0);
        (void)gateway.line();  // stopped: ...
        EXPECT_NE(gateway.line().find("journal: records"), std::string::npos);
    }
    GatewayProcess again(gateway_args(journal));
    auto clients = start_and_log_in(again, journal, false);
    EXPECT_EQ(clients[1]->first_sequence, 2U);
    EXPECT_EQ(again.stop(SIGTERM), 0);
    const std::string summary = again.line();
    EXPECT_NE(summary.find("live orders 1"), std::string::npos) << summary;
}

TEST(GatewayJournalProgram, AJournalThatCannotBeUsedStopsTheProgramWithExitCodeOne) {
    const litbook::testing::TempDir dir;
    const std::string garbage = dir.file("garbage.journal");
    {
        std::ofstream out(garbage);
        out << "not a journal";
    }
    GatewayProcess bad_header(gateway_args(garbage));
    EXPECT_EQ(bad_header.wait_for_exit(std::chrono::seconds(30)), 1);

    // A journal written with other stocks.
    const std::string journal = dir.file("exchange.journal");
    {
        GatewayProcess first(gateway_args(journal));
        (void)litbook::testing::gateway_port(first);
        EXPECT_EQ(first.stop(SIGTERM), 0);
    }
    std::vector<std::string> args = gateway_args(journal);
    args.back() = "ZVZZT";
    GatewayProcess other_setup(args);
    EXPECT_EQ(other_setup.wait_for_exit(std::chrono::seconds(30)), 1);
}

TEST(GatewayJournalProgram, RejectsBadJournalArguments) {
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"--sync", "sync"},
          std::vector<std::string>{"--journal", "/tmp/x", "--sync", "sometimes"},
          std::vector<std::string>{"--journal", ""}}) {
        GatewayProcess gateway(args);
        EXPECT_EQ(gateway.stop(0), 2) << args.back();
    }
}

TEST(GatewayJournalProgram, TheSyncPolicySyncsEveryBatch) {
    const litbook::testing::TempDir dir;
    std::vector<std::string> args = gateway_args(dir.file("synced.journal"));
    args.emplace_back("--sync");
    args.emplace_back("sync");
    GatewayProcess gateway(args);
    (void)litbook::testing::gateway_port(gateway);
    (void)gateway.line();
    EXPECT_EQ(gateway.stop(SIGTERM), 0);
    (void)gateway.line();
    const std::string journal_line = gateway.line();
    // The setup and Stop each needed a sync.
    EXPECT_NE(journal_line.find("journal: records"), std::string::npos) << journal_line;
    EXPECT_EQ(journal_line.find("syncs 0,"), std::string::npos) << journal_line;
}

// The crash test. 20 rounds of start, trade a random amount, SIGKILL. Every fourth round
// is killed right after it was spawned, so sometimes during recovery itself. All waits
// have deadlines, and nothing waits a fixed time.
TEST(GatewayJournalCrashTest, EveryReplyAClientSawSurvivesRandomKills) {
    constexpr int kRounds = 20;
    constexpr std::uint64_t kSeed = 20261122;
    std::mt19937_64 rng(kSeed);
    const litbook::testing::TempDir dir;
    const std::string journal = dir.file("crash.journal");
    // Every message each account ever received, by sequence number, over all rounds.
    std::map<OwnerId, std::map<std::uint64_t, Bytes>> seen;
    std::map<OwnerId, protocol::UserRefNum> next_ref{{1, 1}, {2, 1}, {3, 1}};
    int torn = 0;

    for (int round = 0; round < kRounds; ++round) {
        SCOPED_TRACE("seed " + std::to_string(kSeed) + " round " + std::to_string(round));
        GatewayProcess gateway(gateway_args(journal));
        if (round % 4 == 3) {
            // Killed while it may still be recovering.
            EXPECT_EQ(gateway.stop(SIGKILL), -1);
        } else {
            auto clients = start_and_log_in(gateway, journal, round == 0);
            const std::uint64_t target = 5 + rng() % 200;
            std::uint64_t got = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (got < target && std::chrono::steady_clock::now() < deadline) {
                for (auto& c : clients) {
                    protocol::UserRefNum& ref = next_ref[c->account.owner];
                    if (rng() % 5 == 0 && ref > 1) {
                        const protocol::CancelOrder cancel{
                            .user_ref = static_cast<protocol::UserRefNum>(1 + rng() % (ref - 1)),
                            .qty = 0};
                        (void)c->tcp.send_some(testing::unsequenced(cancel));
                    } else {
                        (void)c->tcp.send_some(testing::unsequenced(random_order(rng, ref++)));
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                got = 0;
                for (auto& c : clients) {
                    c->collect(seen[c->account.owner]);
                    got += c->received;
                }
            }
            EXPECT_GE(got, target);
            EXPECT_EQ(gateway.stop(SIGKILL), -1);
            // Whatever was still on its way to a client is lost, as in a real crash.
        }

        // The write-ahead check. Every message a client ever saw must be in the replay of
        // the journal, with the same sequence number and the same bytes.
        if (!std::filesystem::exists(journal)) {
            continue;  // Killed before the journal was even created.
        }
        const Replayed replayed = replay_file(journal);
        EXPECT_NE(replayed.tail, journal::TailStatus::Corrupt);
        EXPECT_NE(replayed.tail, journal::TailStatus::BadHeader);
        torn += replayed.tail == journal::TailStatus::Torn ? 1 : 0;
        for (const auto& [owner, messages] : seen) {
            const auto it = replayed.replies.find(owner);
            const std::size_t produced = it == replayed.replies.end() ? 0 : it->second.size();
            for (const auto& [sequence, bytes] : messages) {
                ASSERT_LE(sequence, produced) << "account " << owner << " saw message " << sequence
                                              << " that the journal does not hold";
                EXPECT_EQ(it->second[sequence - 1], bytes)
                    << "account " << owner << " message " << sequence;
            }
        }
    }

    // A last clean run, whose end state must equal a replay of the whole journal.
    GatewayProcess last(gateway_args(journal));
    auto clients = start_and_log_in(last, journal, false);
    EXPECT_EQ(last.stop(SIGTERM), 0);
    const std::string summary = last.line();
    const Replayed replayed = replay_file(journal);
    EXPECT_EQ(replayed.tail, journal::TailStatus::Clean);
    EXPECT_NE(summary.find("live orders " + std::to_string(replayed.live_orders)),
              std::string::npos)
        << summary;
    std::uint64_t total = 0;
    for (const auto& [owner, messages] : seen) {
        total += messages.size();
    }
    EXPECT_GT(total, 500U);
    std::printf("crash test: %d rounds, %llu messages checked, %d torn tails seen\n", kRounds,
                static_cast<unsigned long long>(total), torn);
}

}  // namespace
}  // namespace litbook::gateway
