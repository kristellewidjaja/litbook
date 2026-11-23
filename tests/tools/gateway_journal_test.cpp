// End-to-end tests of litbook_gateway with --journal. The smoke tests start the real
// program, trade a little, stop it and start it again on the same journal directory. The
// crash tests kill it with SIGKILL again and again at random points and check after every
// kill that the journal explains every reply any client saw, and after every restart that
// the accounts carry on with the right sequence numbers. The second crash test uses tiny
// segments, so kills also land while segments rotate and snapshots are written.

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/journal/directory_replay.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/snapshot_store.hpp"

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
#include <set>
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

std::vector<std::string> gateway_args(const std::string& journal,
                                      const std::vector<std::string>& extra = {}) {
    std::vector<std::string> args{
        "--port",           "0",         "--journal",   journal,     "--account",
        "alice:secret:cod", "--account", "bob:hunter2", "--account", "carol:pw",
        "--symbol",         "AAPL",      "--symbol",    "MSFT"};
    args.insert(args.end(), extra.begin(), extra.end());
    return args;
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

// What replaying the journal directory gives. A replay that starts from a snapshot only
// produces the replies after it, and the snapshot says how many came before.
struct Replayed {
    journal::DirectoryStatus status = journal::DirectoryStatus::Empty;
    // The replies the replay produced, per account, in order.
    std::map<OwnerId, std::vector<Bytes>> replies;
    // Each account's replies already counted in the snapshot the replay started from.
    std::map<OwnerId, std::uint64_t> earlier;
    std::uint32_t live_orders = 0;
    protocol::OrderEntryState state;  // The end state of the replay.
    journal::TailStatus tail = journal::TailStatus::BadHeader;
    journal::Sequence snapshot_as_of = 0;
    journal::Sequence next_sequence = 0;

    // Every reply the account ever got, the snapshot's included.
    [[nodiscard]] std::uint64_t total(OwnerId owner) const {
        std::uint64_t n = 0;
        if (const auto it = earlier.find(owner); it != earlier.end()) {
            n += it->second;
        }
        if (const auto it = replies.find(owner); it != replies.end()) {
            n += it->second.size();
        }
        return n;
    }
};

Replayed replay_journal_directory(const std::string& directory, journal::StartFrom start) {
    Replayed out;
    journal::testing::RecordingReplies replies;
    journal::Replayer<SortedOrderBook> replayer(replies);
    const journal::DirectoryReplay result = journal::replay_directory(directory, replayer, start);
    out.status = result.status;
    EXPECT_EQ(result.status, journal::DirectoryStatus::Ok) << directory << " " << result.replay;
    if (result.status != journal::DirectoryStatus::Ok) {
        return out;
    }
    out.tail = result.chain.segments.back().scan.tail;
    out.snapshot_as_of = result.snapshot_as_of;
    out.next_sequence = result.chain.next_sequence;
    for (std::size_t owner = 1; owner < result.snapshot_replies.size(); ++owner) {
        out.earlier[static_cast<OwnerId>(owner)] = result.snapshot_replies[owner];
    }
    for (const auto& r : replies.replies) {
        out.replies[r.owner].push_back(r.bytes);
    }
    if (replayer.started()) {
        out.live_orders = replayer.order_entry().live_orders();
        replayer.order_entry().capture(out.state);
    }
    return out;
}

// Recovery's view of the journal, from the newest usable snapshot.
Replayed replay_like_recovery(const std::string& directory) {
    return replay_journal_directory(directory, journal::StartFrom::NewestSnapshot);
}

// The whole journal from record 1, which needs every segment.
Replayed replay_everything(const std::string& directory) {
    return replay_journal_directory(directory, journal::StartFrom::FirstRecord);
}

bool has_journal(const std::string& directory) {
    return std::filesystem::exists(directory) &&
           !journal::list_journal_directory(directory).segments.empty();
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
    const Replayed before = replay_like_recovery(journal);
    std::vector<std::unique_ptr<Client>> clients;
    for (const Account& a : kAccounts) {
        auto c = std::make_unique<Client>(port, a);
        EXPECT_TRUE(c->log_in()) << a.username;
        EXPECT_EQ(c->first_sequence, before.total(a.owner) + 1) << a.username;
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
    using Args = std::vector<std::string>;
    for (const Args& args : {
             Args{"--sync", "sync"},
             Args{"--journal", "/tmp/x", "--sync", "sometimes"},
             Args{"--journal", ""},
             // The segment and snapshot flags need --journal too.
             Args{"--segment-records", "10"},
             Args{"--keep-snapshots", "3"},
             Args{"--snapshot-thread", "off"},
             Args{"--journal", "/tmp/x", "--segment-records", "many"},
             Args{"--journal", "/tmp/x", "--keep-snapshots", "-1"},
             Args{"--journal", "/tmp/x", "--snapshot-thread", "maybe"},
             // The snapshot thread needs segments and snapshots.
             Args{"--journal", "/tmp/x", "--snapshot-thread", "on", "--keep-snapshots", "0"},
             Args{"--journal", "/tmp/x", "--snapshot-thread", "on", "--segment-records", "0"},
         }) {
        GatewayProcess gateway(args);
        EXPECT_EQ(gateway.stop(0), 2) << args[0] << " " << args.back();
    }
}

// Sends `count` resting sells from bob and waits until each one was answered.
void rest_sells(Client& bob, std::uint64_t count, std::map<std::uint64_t, Bytes>& seen) {
    std::mt19937_64 rng(7);
    const std::uint64_t before = bob.received;
    for (std::uint64_t i = 0; i < count; ++i) {
        protocol::EnterOrder sell = random_order(rng, static_cast<protocol::UserRefNum>(i + 1));
        sell.side = Side::Sell;
        sell.price = 2'000'000;  // Far above every buy, so nothing trades.
        (void)bob.tcp.send_some(testing::unsequenced(sell));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (bob.received < before + count && std::chrono::steady_clock::now() < deadline) {
        bob.collect(seen);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    ASSERT_EQ(bob.received, before + count);
}

TEST(GatewayJournalProgram, SmallSegmentsRotateAndARestartStartsFromTheStopSnapshot) {
    const litbook::testing::TempDir dir;
    const std::string journal = dir.file("segments");
    const std::vector<std::string> extra{"--segment-records", "8", "--keep-snapshots", "2"};
    std::map<std::uint64_t, Bytes> seen;
    std::string stop_snapshot;
    {
        GatewayProcess gateway(gateway_args(journal, extra));
        auto clients = start_and_log_in(gateway, journal, true);
        rest_sells(*clients[1], 40, seen);
        EXPECT_EQ(gateway.stop(SIGTERM), 0);
        (void)gateway.line();  // stopped: ...
        const std::string journal_line = gateway.line();
        EXPECT_NE(journal_line.find("journal: records"), std::string::npos) << journal_line;
        EXPECT_EQ(journal_line.find("rotations 0;"), std::string::npos) << journal_line;
        EXPECT_NE(journal_line.find("; snapshot thread snapshots "), std::string::npos)
            << journal_line;
        const std::size_t at = journal_line.find("; stop snapshot as of ");
        ASSERT_NE(at, std::string::npos) << journal_line;
        stop_snapshot = journal_line.substr(at + std::string("; stop snapshot as of ").size());
    }
    // A snapshot a crash left half written, which the next start must delete.
    const std::string half_written = journal + "/" + journal::snapshot_file_name(5) + ".tmp";
    {
        std::ofstream(half_written) << "half";
    }
    ASSERT_EQ(journal::list_journal_directory(journal).temporary.size(), 1U);

    GatewayProcess again(gateway_args(journal, extra));
    const std::uint16_t port = litbook::testing::gateway_port(again);
    const std::string recovery = again.line();
    EXPECT_NE(recovery.find("from snapshot as of " + stop_snapshot + ","), std::string::npos)
        << recovery;
    EXPECT_NE(recovery.find("snapshots refused 0"), std::string::npos) << recovery;
    EXPECT_NE(recovery.find("; start snapshot as of "), std::string::npos) << recovery;
    Client bob(port, kAccounts[1]);
    ASSERT_TRUE(bob.log_in());
    EXPECT_EQ(bob.first_sequence, 41U);
    EXPECT_EQ(again.stop(SIGTERM), 0);
    EXPECT_NE(again.line().find("live orders 40"), std::string::npos);

    // Retention kept two snapshots, and the segments before the older one are gone.
    const journal::DirectoryListing listing = journal::list_journal_directory(journal);
    EXPECT_EQ(listing.snapshots.size(), 2U);
    EXPECT_TRUE(listing.temporary.empty());
    EXPECT_FALSE(std::filesystem::exists(half_written));
    ASSERT_FALSE(listing.segments.empty());
    EXPECT_GT(listing.segments.front().first, 1U);
    EXPECT_EQ(replay_like_recovery(journal).live_orders, 40U);
}

TEST(GatewayJournalProgram, WithoutSnapshotsNothingIsRemoved) {
    const litbook::testing::TempDir dir;
    const std::string journal = dir.file("kept");
    const std::vector<std::string> extra{"--segment-records", "8", "--keep-snapshots", "0"};
    std::map<std::uint64_t, Bytes> seen;
    {
        GatewayProcess gateway(gateway_args(journal, extra));
        auto clients = start_and_log_in(gateway, journal, true);
        rest_sells(*clients[1], 30, seen);
        EXPECT_EQ(gateway.stop(SIGTERM), 0);
        (void)gateway.line();
        const std::string journal_line = gateway.line();
        // No snapshot thread and no stop snapshot.
        EXPECT_EQ(journal_line.find("snapshot"), std::string::npos) << journal_line;
    }
    GatewayProcess again(gateway_args(journal, extra));
    (void)litbook::testing::gateway_port(again);
    const std::string recovery = again.line();
    EXPECT_NE(recovery.find("from record 1"), std::string::npos) << recovery;
    EXPECT_NE(recovery.find("; snapshots off"), std::string::npos) << recovery;
    EXPECT_EQ(again.stop(SIGTERM), 0);
    const journal::DirectoryListing listing = journal::list_journal_directory(journal);
    EXPECT_TRUE(listing.snapshots.empty());
    // The journal thread rotates after a batch that filled the segment, and a batch can
    // hold several requests, so a segment can hold more than 8 records.
    EXPECT_GT(listing.segments.size(), 1U);
    EXPECT_EQ(listing.segments.front().first, 1U);
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

// What the crash rounds carry from one round to the next.
struct CrashState {
    explicit CrashState(std::uint64_t seed) : rng(seed) {}

    std::mt19937_64 rng;
    // Every message each account ever received, by sequence number, over all rounds.
    std::map<OwnerId, std::map<std::uint64_t, Bytes>> seen;
    std::map<OwnerId, protocol::UserRefNum> next_ref{{1, 1}, {2, 1}, {3, 1}};
    bool started = false;  // False until a round got as far as creating the journal.
    int torn = 0;
    int from_snapshot = 0;  // Rounds whose recovery started from a snapshot.
    // The snapshots already checked. A snapshot file never changes once it has its name,
    // and replay is deterministic, so checking each one once is enough.
    std::set<journal::Sequence> snapshots_checked;
};

// Sends random orders and cancels from every client until `target` replies arrived in
// total (or a deadline passed), keeping every message the clients got.
void trade(std::vector<std::unique_ptr<Client>>& clients, CrashState& state, std::uint64_t target) {
    std::uint64_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (got < target && std::chrono::steady_clock::now() < deadline) {
        for (auto& c : clients) {
            protocol::UserRefNum& ref = state.next_ref[c->account.owner];
            if (state.rng() % 5 == 0 && ref > 1) {
                const protocol::CancelOrder cancel{
                    .user_ref = static_cast<protocol::UserRefNum>(1 + state.rng() % (ref - 1)),
                    .qty = 0};
                (void)c->tcp.send_some(testing::unsequenced(cancel));
            } else {
                (void)c->tcp.send_some(testing::unsequenced(random_order(state.rng, ref++)));
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        got = 0;
        for (auto& c : clients) {
            c->collect(state.seen[c->account.owner]);
            got += c->received;
        }
    }
    EXPECT_GE(got, target);
}

// The write-ahead check. Every message a client ever saw must be explained by the
// journal. A message the replay produced must have the same sequence number and bytes.
// A message the starting snapshot already counted can only be checked by its number.
void check_seen_messages(const CrashState& state, const Replayed& replayed) {
    for (const auto& [owner, messages] : state.seen) {
        const std::uint64_t earlier =
            replayed.earlier.contains(owner) ? replayed.earlier.at(owner) : 0;
        const auto it = replayed.replies.find(owner);
        for (const auto& [sequence, bytes] : messages) {
            ASSERT_LE(sequence, replayed.total(owner))
                << "account " << owner << " saw message " << sequence
                << " that the journal does not hold";
            if (sequence > earlier) {
                EXPECT_EQ(it->second.at(sequence - earlier - 1), bytes)
                    << "account " << owner << " message " << sequence;
            }
        }
    }
}

// Every snapshot file must be whole (each one is renamed into place only when it is
// complete) and continued by the segments (retention removes old snapshots before the
// segments they need). Replaying the later records from each one must end in exactly the
// state, and with exactly the reply counts, of `truth`. Snapshots checked in an earlier
// round are only checked to still be whole and continued.
void check_every_snapshot(const std::string& directory, const Replayed& truth, CrashState& state) {
    const journal::DirectoryListing listing = journal::list_journal_directory(directory);
    const journal::ChainScan chain = journal::scan_segment_chain(listing);
    ASSERT_EQ(chain.status, journal::ChainStatus::Ok);
    for (const journal::SnapshotFile& file : listing.snapshots) {
        SCOPED_TRACE("snapshot as of " + std::to_string(file.as_of));
        const journal::LoadedSnapshot loaded = journal::read_snapshot_file(file);
        ASSERT_EQ(loaded.status, journal::SnapshotStatus::Ok);
        ASSERT_TRUE(journal::snapshot_is_continued(chain, file.as_of));
        if (state.snapshots_checked.contains(file.as_of)) {
            continue;
        }

        journal::testing::RecordingReplies replies;
        journal::Replayer<SortedOrderBook> replayer(replies);
        ASSERT_EQ(replayer.start_from(loaded.snapshot), journal::ReplayStatus::Ok);
        for (const journal::SegmentScan& segment : chain.segments) {
            const journal::MappedFile mapped(segment.file.path);
            journal::RecordReader reader(
                mapped.bytes().subspan(journal::kFileHeaderLength,
                                       segment.scan.valid_length - journal::kFileHeaderLength));
            journal::JournalRecord record;
            while (reader.next(record)) {
                ASSERT_EQ(replayer.apply(record), journal::ReplayStatus::Ok);
            }
        }
        protocol::OrderEntryState end;
        replayer.order_entry().capture(end);
        EXPECT_EQ(end, truth.state);
        std::vector<std::uint64_t> counts = loaded.snapshot.replies;
        for (const journal::testing::RecordedReply& reply : replies.replies) {
            ++counts.at(reply.owner);
        }
        for (const Account& a : kAccounts) {
            EXPECT_EQ(counts.at(a.owner), truth.total(a.owner)) << a.username;
        }
        state.snapshots_checked.insert(file.as_of);
    }
}

// One round. Start, log in, trade a random amount and SIGKILL, or every fourth round
// SIGKILL right after the spawn, so sometimes during recovery itself. Then check the
// journal. With `everything_kept`, no file was removed, so the whole journal replays from
// record 1 and every message can be checked by its bytes.
void crash_round(const std::string& journal, const std::vector<std::string>& extra, int round,
                 CrashState& state, bool everything_kept) {
    GatewayProcess gateway(gateway_args(journal, extra));
    if (round % 4 == 3) {
        EXPECT_EQ(gateway.stop(SIGKILL), -1);
    } else {
        auto clients = start_and_log_in(gateway, journal, !state.started);
        state.started = true;
        trade(clients, state, 5 + state.rng() % 200);
        EXPECT_EQ(gateway.stop(SIGKILL), -1);
        // Whatever was still on its way to a client is lost, as in a real crash.
    }
    if (!has_journal(journal)) {
        return;  // Killed before the journal was even created.
    }
    state.started = true;
    const Replayed recovery = replay_like_recovery(journal);
    ASSERT_EQ(recovery.status, journal::DirectoryStatus::Ok);
    EXPECT_NE(recovery.tail, journal::TailStatus::Corrupt);
    EXPECT_NE(recovery.tail, journal::TailStatus::BadHeader);
    state.torn += recovery.tail == journal::TailStatus::Torn ? 1 : 0;
    state.from_snapshot += recovery.snapshot_as_of > 0 ? 1 : 0;
    if (everything_kept) {
        const Replayed full = replay_everything(journal);
        ASSERT_EQ(full.status, journal::DirectoryStatus::Ok);
        check_seen_messages(state, full);
        EXPECT_EQ(recovery.state, full.state);
        check_every_snapshot(journal, full, state);
    } else {
        check_seen_messages(state, recovery);
        check_every_snapshot(journal, recovery, state);
    }
}

// A last clean run, whose end state must equal recovery's replay of the journal.
void last_clean_run(const std::string& journal, const std::vector<std::string>& extra) {
    GatewayProcess last(gateway_args(journal, extra));
    auto clients = start_and_log_in(last, journal, false);
    EXPECT_EQ(last.stop(SIGTERM), 0);
    const std::string summary = last.line();
    const Replayed replayed = replay_like_recovery(journal);
    EXPECT_EQ(replayed.tail, journal::TailStatus::Clean);
    EXPECT_NE(summary.find("live orders " + std::to_string(replayed.live_orders)),
              std::string::npos)
        << summary;
}

std::uint64_t messages_seen(const CrashState& state) {
    std::uint64_t total = 0;
    for (const auto& [owner, messages] : state.seen) {
        total += messages.size();
    }
    return total;
}

// 20 rounds on the default segments (a million records, so one segment). All waits have
// deadlines, and nothing waits a fixed time.
TEST(GatewayJournalCrashTest, EveryReplyAClientSawSurvivesRandomKills) {
    constexpr int kRounds = 20;
    constexpr std::uint64_t kSeed = 20261122;
    CrashState state(kSeed);
    const litbook::testing::TempDir dir;
    const std::string journal = dir.file("crash.journal");
    for (int round = 0; round < kRounds; ++round) {
        SCOPED_TRACE("seed " + std::to_string(kSeed) + " round " + std::to_string(round));
        crash_round(journal, {}, round, state, true);
    }
    last_clean_run(journal, {});
    EXPECT_GT(messages_seen(state), 500U);
    std::printf("crash test: %d rounds, %llu messages checked, %d torn tails seen\n", kRounds,
                static_cast<unsigned long long>(messages_seen(state)), state.torn);
}

// Segments of 16 records, so every round rotates many times and the snapshot thread
// writes a snapshot for each closed segment, and the kills land among those. For 20
// rounds every snapshot is kept (so nothing is removed and the whole journal can be
// checked), then 6 more rounds keep only 2, so old files are removed under the kills too.
TEST(GatewayJournalCrashTest, SmallSegmentsAndSnapshotsSurviveRandomKills) {
    constexpr int kKeepAllRounds = 20;
    constexpr int kRemoveRounds = 6;
    constexpr std::uint64_t kSeed = 20261123;
    CrashState state(kSeed);
    const litbook::testing::TempDir dir;
    const std::string journal = dir.file("crash.journal");
    const std::vector<std::string> keep_all{"--segment-records", "16", "--keep-snapshots",
                                            "100000"};
    const std::vector<std::string> keep_two{"--segment-records", "16", "--keep-snapshots", "2"};
    for (int round = 0; round < kKeepAllRounds; ++round) {
        SCOPED_TRACE("seed " + std::to_string(kSeed) + " round " + std::to_string(round));
        crash_round(journal, keep_all, round, state, true);
    }
    const std::size_t segments_before = journal::list_journal_directory(journal).segments.size();
    for (int round = 0; round < kRemoveRounds; ++round) {
        SCOPED_TRACE("seed " + std::to_string(kSeed) + " removing round " + std::to_string(round));
        crash_round(journal, keep_two, round, state, false);
    }
    last_clean_run(journal, keep_two);

    const journal::DirectoryListing after = journal::list_journal_directory(journal);
    EXPECT_GT(segments_before, 20U);  // The kills really happened among many segments.
    EXPECT_LT(after.segments.size(), segments_before);  // And old ones were removed.
    EXPECT_LE(after.snapshots.size(), 3U);  // 2 kept, plus a thread snapshot not yet trimmed.
    EXPECT_GT(state.from_snapshot, kKeepAllRounds / 2);
    EXPECT_GT(messages_seen(state), 500U);
    std::printf(
        "crash test with small segments: %d rounds, %llu messages checked, %llu snapshots "
        "checked, %d recoveries from a snapshot, %d torn tails seen, segments %zu then %zu\n",
        kKeepAllRounds + kRemoveRounds, static_cast<unsigned long long>(messages_seen(state)),
        static_cast<unsigned long long>(state.snapshots_checked.size()), state.from_snapshot,
        state.torn, segments_before, after.segments.size());
}

}  // namespace
}  // namespace litbook::gateway
