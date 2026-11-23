#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/mutex_queue.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/pipeline/feed_worker.hpp"
#include "litbook/pipeline/threaded_gateway.hpp"

#include <gtest/gtest.h>

#include <poll.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "book/test_limits.hpp"
#include "feed/feed_test_support.hpp"
#include "gateway/soup_test_support.hpp"
#include "gateway/test_client.hpp"
#include "journal/replay_test_support.hpp"
#include "pipeline/pipeline_test_support.hpp"
#include "pipeline/threaded_test_support.hpp"
#include "support/temp_dir.hpp"

// The threaded pipeline with a journal, over real loopback sockets, on the same four
// setups as ThreadedGatewayTest (SpscQueue and MutexQueue, spinning and blocking). These
// tests check the write-ahead property (every reply a client saw is explained by the
// journal), recovery at start, and what happens when the journal cannot be written.

namespace litbook::pipeline {
namespace {

using gateway::soup::PacketType;
using gateway::testing::Bytes;
using gateway::testing::login;
using gateway::testing::unsequenced;
using testing::limit;
using testing::Peer;
using testing::wait_until;

constexpr Price kTick = 100;
constexpr OwnerId kAlice = 1;  // Cancel on disconnect is on.
constexpr OwnerId kBob = 2;

protocol::Timestamp still_clock() {
    return 1'000'000;
}

// A write() that fails with ENOSPC from the Nth call on. The journal thread calls it, so
// the counters are atomic.
std::atomic<int> g_writes{0};
std::atomic<int> g_fail_from{0};

ssize_t failing_write(int fd, const void* data, std::size_t length) {
    const int n = ++g_writes;
    if (g_fail_from != 0 && n >= g_fail_from) {
        errno = ENOSPC;
        return -1;
    }
    return ::write(fd, data, length);
}

protocol::OrderEntryConfig entry_config() {
    return {.max_owners = 4, .max_live_orders = 4096, .max_symbols = 1};
}

BookConfig book_config() {
    return litbook::testing::with_test_limits(BookConfig{.tick_size = kTick});
}

const gateway::AccountConfig kAliceAccount{
    .username = "alice", .password = "secret", .cancel_on_disconnect = true};
const gateway::AccountConfig kBobAccount{.username = "bob", .password = "hunter2"};

// The setup records this exchange writes, for journals made by hand.
std::vector<journal::JournalRecord> expected_setup() {
    return {journal::session_start_record(entry_config()),
            journal::risk_limits_record(kAlice, kAliceAccount.limits),
            journal::risk_limits_record(kBob, kBobAccount.limits),
            journal::add_symbol_record(journal::SymbolListing{
                .id = 0, .ticker = testing::aapl(), .config = book_config()})};
}

// Writes a journal as an exchange that crashed would have left it. It holds the setup
// records and then `requests`, with no Stop.
void write_crashed_journal(const std::string& path, std::size_t setup_records,
                           const std::vector<Request>& requests) {
    journal::JournalWriter writer = journal::JournalWriter::create(path, {});
    const std::vector<journal::JournalRecord> setup = expected_setup();
    for (std::size_t i = 0; i < setup_records; ++i) {
        ASSERT_EQ(journal::append_setup_record(writer, 5, setup[i]), journal::JournalStatus::Ok);
    }
    for (const Request& r : requests) {
        ASSERT_EQ(writer.append(r), journal::JournalStatus::Ok);
    }
    ASSERT_EQ(writer.flush(), journal::JournalStatus::Ok);
}

std::vector<journal::JournalRecord> records_in(const std::string& path) {
    const journal::MappedFile file(path);
    const journal::JournalScan scan = journal::scan_journal(file.bytes());
    EXPECT_EQ(scan.tail, journal::TailStatus::Clean);
    std::vector<journal::JournalRecord> out;
    journal::RecordReader reader(file.bytes().subspan(
        journal::kFileHeaderLength, scan.valid_length - journal::kFileHeaderLength));
    journal::JournalRecord r;
    while (reader.next(r)) {
        out.push_back(r);
    }
    return out;
}

// Every reply that replaying the journal produces, per account.
std::map<OwnerId, std::vector<Bytes>> replayed_replies(const std::string& path) {
    journal::testing::RecordingReplies replies;
    journal::Replayer<SortedOrderBook> replayer(replies);
    const journal::MappedFile file(path);
    const auto outcome = journal::replay_journal(file.bytes(), replayer);
    EXPECT_EQ(outcome.status, journal::ReplayStatus::Ok);
    std::map<OwnerId, std::vector<Bytes>> out;
    for (const auto& r : replies.replies) {
        out[r.owner].push_back(r.bytes);
    }
    return out;
}

// The OUCH bytes of every Sequenced Data packet a peer received.
std::vector<Bytes> sequenced(const Peer& peer) {
    std::vector<Bytes> out;
    for (const auto& p : peer.packets) {
        if (p.type == PacketType::SequencedData) {
            out.push_back(p.payload);
        }
    }
    return out;
}

// True if `seen` is the start of `all`, which is how the write-ahead property looks from
// a client. A client may miss replies at the end (its connection closed), but it never
// sees one that the journal does not explain.
bool is_prefix(const std::vector<Bytes>& seen, const std::vector<Bytes>& all) {
    if (seen.size() > all.size()) {
        return false;
    }
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (seen[i] != all[i]) {
            return false;
        }
    }
    return true;
}

template <template <concurrency::QueueElement> class Q, WaitMode Mode>
struct Setup {
    static constexpr WaitMode kMode = Mode;
};

template <class S>
struct ExchangeOf;
template <template <concurrency::QueueElement> class Q, WaitMode Mode>
struct ExchangeOf<Setup<Q, Mode>> {
    using type = ThreadedGateway<SortedOrderBook, Q>;
};

template <class S>
class ThreadedJournalTest : public ::testing::Test {
public:  // The snapshot tests below use the fixture from free helper functions.
    using Exchange = typename ExchangeOf<S>::type;

    void SetUp() override {
        g_writes = 0;
        g_fail_from = 0;
        std::filesystem::create_directories(journal_dir_);
    }

    // Builds the exchange on the test's journal. `extra_account` gives it another setup.
    void make(bool extra_account = false, journal::FileOps ops = {}) {
        exchange_.reset();
        JournalOptions journal{.directory = journal_dir_,
                               .records_per_segment = records_per_segment_,
                               .keep_snapshots = keep_snapshots_};
        journal.writer.ops = ops;
        exchange_ = std::make_unique<Exchange>(
            gateway::SessionConfig{.max_connections = 8}, entry_config(),
            gateway::GatewayConfig{.clock = &still_clock}, PipelineConfig{.wait = S::kMode},
            typename Exchange::PublisherOptions{}, &journal);
        exchange_->add_account(kAliceAccount);
        exchange_->add_account(kBobAccount);
        if (extra_account) {
            exchange_->add_account({.username = "carol", .password = "pw"});
        }
        exchange_->add_symbol(testing::aapl(), book_config());
    }

    std::unique_ptr<Peer> log_in(std::string_view user, std::string_view password) {
        auto peer = std::make_unique<Peer>(exchange_->port());
        peer->send(login(user, password));
        EXPECT_TRUE(
            wait_until({peer.get()}, [&] { return peer->count(PacketType::LoginAccepted) == 1; }));
        return peer;
    }

    // The sequence number in the peer's Login Accepted.
    static std::uint64_t login_sequence(const Peer& peer) {
        for (const auto& p : peer.packets) {
            if (p.type == PacketType::LoginAccepted) {
                return litbook::testing::unwrap(gateway::soup::decode_login_accepted(p.payload))
                    .sequence;
            }
        }
        return 0;
    }

    // Bob rests a sell order of 50 at 50 ticks and the exchange stops cleanly.
    void run_with_a_resting_order() {
        make();
        exchange_->start();
        auto bob = log_in("bob", "hunter2");
        bob->send(unsequenced(limit(Side::Sell, 1, 50, 50 * kTick)));
        ASSERT_TRUE(wait_until({bob.get()}, [&] {
            return bob->template all<protocol::AcceptedMessage>().size() == 1;
        }));
        exchange_->stop();
    }

    litbook::testing::TempDir dir_;
    // The journal directory. The tests look at its first segment, which holds every
    // record because the segments are far larger than any test.
    std::string journal_dir_ = dir_.file("journal");
    std::string path_ = journal_dir_ + "/" + journal::segment_file_name(1);
    // The 9b-1 tests check recovery by replaying the whole journal, so they run without
    // snapshots. The snapshot tests below turn them on.
    std::size_t keep_snapshots_ = 0;
    std::uint64_t records_per_segment_ = 1'000'000;
    std::unique_ptr<Exchange> exchange_;
};

using Setups = ::testing::Types<Setup<concurrency::SpscQueue, WaitMode::Spin>,
                                Setup<concurrency::SpscQueue, WaitMode::Block>,
                                Setup<concurrency::MutexQueue, WaitMode::Spin>,
                                Setup<concurrency::MutexQueue, WaitMode::Block>>;
TYPED_TEST_SUITE(ThreadedJournalTest, Setups);

TYPED_TEST(ThreadedJournalTest, ANewJournalHoldsTheSetupThenEveryRequestAndExplainsEveryReply) {
    this->make();
    this->exchange_->start();
    EXPECT_FALSE(this->exchange_->recovery().recovered);
    auto alice = this->log_in("alice", "secret");
    auto bob = this->log_in("bob", "hunter2");
    bob->send(unsequenced(limit(Side::Sell, 1, 50, 50 * kTick)));
    ASSERT_TRUE(wait_until(
        {bob.get()}, [&] { return bob->template all<protocol::AcceptedMessage>().size() == 1; }));
    alice->send(unsequenced(limit(Side::Buy, 1, 20, 51 * kTick)));
    alice->send(unsequenced(limit(Side::Buy, 2, 5, 40 * kTick)));
    ASSERT_TRUE(wait_until({alice.get(), bob.get()}, [&] {
        return alice->template all<protocol::AcceptedMessage>().size() == 2 &&
               bob->template all<protocol::ExecutedMessage>().size() == 1;
    }));
    this->exchange_->stop();
    alice->collect();
    bob->collect();

    const auto* journal = this->exchange_->journal();
    ASSERT_NE(journal, nullptr);
    EXPECT_FALSE(journal->failed());
    EXPECT_EQ(journal->stats().forwarded, this->exchange_->matching().stats().requests);

    const std::vector<journal::JournalRecord> records = records_in(this->path_);
    const std::vector<journal::JournalRecord> setup = expected_setup();
    ASSERT_GT(records.size(), setup.size());
    EXPECT_EQ(journal::setup_difference(
                  std::span<const journal::JournalRecord>(records.data(), setup.size()), setup),
              "");
    EXPECT_EQ(records.back().type, journal::RecordType::Stop);
    // Three orders, then the cancel on disconnect of alice when the exchange stopped.
    std::size_t frames = 0;
    std::size_t cancels = 0;
    for (const auto& r : records) {
        frames += r.type == journal::RecordType::Frame ? 1 : 0;
        cancels += r.type == journal::RecordType::CancelAll ? 1 : 0;
    }
    EXPECT_EQ(frames, 3U);
    EXPECT_EQ(cancels, 1U);

    auto replayed = replayed_replies(this->path_);
    EXPECT_TRUE(is_prefix(sequenced(*alice), replayed[kAlice]));
    EXPECT_TRUE(is_prefix(sequenced(*bob), replayed[kBob]));
    EXPECT_EQ(sequenced(*bob).size(), replayed[kBob].size());
}

TYPED_TEST(ThreadedJournalTest, ARestartRebuildsTheBookAndCarriesOnTheSequenceNumbers) {
    this->run_with_a_resting_order();
    this->make();
    this->exchange_->start();
    const RecoveryReport& report = this->exchange_->recovery();
    EXPECT_TRUE(report.recovered);
    EXPECT_EQ(report.frames, 1U);
    EXPECT_EQ(report.stops, 1U);
    EXPECT_EQ(report.restarts, 0U);
    EXPECT_EQ(report.replies, 1U);
    EXPECT_EQ(report.torn_bytes, 0U);
    EXPECT_GT(report.restart_sequence, 4U);

    auto bob = this->log_in("bob", "hunter2");
    EXPECT_EQ(this->login_sequence(*bob), 2U);  // Order Accepted was message 1.
    auto alice = this->log_in("alice", "secret");
    EXPECT_EQ(this->login_sequence(*alice), 1U);
    // Bob's order from before the restart is still in the book and trades.
    alice->send(unsequenced(limit(Side::Buy, 1, 20, 51 * kTick)));
    ASSERT_TRUE(wait_until({alice.get(), bob.get()}, [&] {
        return alice->template all<protocol::ExecutedMessage>().size() == 1 &&
               bob->template all<protocol::ExecutedMessage>().size() == 1;
    }));
    EXPECT_EQ(bob->template all<protocol::ExecutedMessage>().front().qty, 20U);
    this->exchange_->stop();

    const std::vector<journal::JournalRecord> records = records_in(this->path_);
    std::size_t restarts = 0;
    for (const auto& r : records) {
        if (r.type == journal::RecordType::Restart) {
            ++restarts;
            EXPECT_EQ(r.sequence, report.restart_sequence);
        }
    }
    EXPECT_EQ(restarts, 1U);

    // A third start sees both earlier runs.
    this->make();
    this->exchange_->start();
    EXPECT_EQ(this->exchange_->recovery().stops, 2U);
    EXPECT_EQ(this->exchange_->recovery().restarts, 1U);
    this->exchange_->stop();
    EXPECT_EQ(this->exchange_->sessions().next_sequence(kBob), 3U);
}

TYPED_TEST(ThreadedJournalTest, OrdersOfACancelOnDisconnectAccountAreCanceledAtRestart) {
    // Alice's order rests when the exchange "crashes", so no cancel on disconnect ran.
    write_crashed_journal(this->path_, 4,
                          {testing::request_of(kAlice, limit(Side::Buy, 1, 10, 40 * kTick), 9),
                           testing::request_of(kBob, limit(Side::Buy, 1, 10, 39 * kTick), 9)});
    this->make();
    this->exchange_->start();
    EXPECT_EQ(this->exchange_->recovery().cancels_on_restart, 1U);
    auto alice = this->log_in("alice", "secret");
    EXPECT_EQ(this->login_sequence(*alice), 3U);  // Accepted, then Canceled at restart.
    auto bob = this->log_in("bob", "hunter2");
    EXPECT_EQ(this->login_sequence(*bob), 2U);
    this->exchange_->stop();
    EXPECT_EQ(this->exchange_->matching().order_entry().open_orders(kBob), 1U);
    EXPECT_EQ(this->exchange_->matching().order_entry().open_orders(kAlice), 0U);

    // The cancel is in the journal, so a replay agrees.
    auto replayed = replayed_replies(this->path_);
    ASSERT_EQ(replayed[kAlice].size(), 2U);
    EXPECT_EQ(replayed[kAlice][1][0], std::byte{'C'});
}

TYPED_TEST(ThreadedJournalTest, ATornTailIsCutOffAndTheGoodRecordsAreKept) {
    this->run_with_a_resting_order();
    const auto before = std::filesystem::file_size(this->path_);
    {
        std::ofstream out(this->path_, std::ios::binary | std::ios::app);
        const std::array<char, 7> half_a_record{40, 0, 'F', 0, 2, 0, 0};
        out.write(half_a_record.data(), half_a_record.size());
    }
    this->make();
    this->exchange_->start();
    EXPECT_EQ(this->exchange_->recovery().torn_bytes, 7U);
    EXPECT_EQ(this->exchange_->recovery().frames, 1U);
    auto bob = this->log_in("bob", "hunter2");
    EXPECT_EQ(this->login_sequence(*bob), 2U);
    this->exchange_->stop();
    EXPECT_GT(std::filesystem::file_size(this->path_), before);
    (void)records_in(this->path_);  // Clean again.
}

TYPED_TEST(ThreadedJournalTest, ACorruptJournalIsRefusedAndLeftAsItIs) {
    this->run_with_a_resting_order();
    const std::vector<journal::JournalRecord> records = records_in(this->path_);
    ASSERT_GE(records.size(), 6U);
    // Damage one byte of the first record, so good records follow the damage.
    {
        std::fstream file(this->path_, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(static_cast<std::streamoff>(journal::kFileHeaderLength + 10));
        file.put('\x55');
    }
    const auto size = std::filesystem::file_size(this->path_);
    this->make();
    try {
        this->exchange_->start();
        FAIL() << "a corrupt journal was accepted";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("corrupt"), std::string::npos) << e.what();
    }
    EXPECT_EQ(std::filesystem::file_size(this->path_), size);
    EXPECT_THROW(this->exchange_->start(), std::logic_error);  // It cannot start any more.
}

TYPED_TEST(ThreadedJournalTest, DamageAfterGoodRecordsIsRefusedToo) {
    this->run_with_a_resting_order();
    const std::vector<journal::JournalRecord> records = records_in(this->path_);
    ASSERT_GE(records.size(), 6U);
    // Damage the fifth record, Bob's order, so four good setup records come before it
    // and the Stop after it is still good.
    std::size_t offset = journal::kFileHeaderLength;
    {
        const journal::MappedFile file(this->path_);
        for (int i = 0; i < 4; ++i) {
            offset += journal::decode_record(file.bytes().subspan(offset)).length;
        }
    }
    {
        std::fstream file(this->path_, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(static_cast<std::streamoff>(offset + journal::kRecordHeaderLength + 2));
        file.put('\x55');
    }
    this->make();
    try {
        this->exchange_->start();
        FAIL() << "a journal damaged in the middle was accepted";
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("good records stop at byte " + std::to_string(offset)),
                  std::string::npos)
            << message;
    }
}

TYPED_TEST(ThreadedJournalTest, AFileThatIsNotAJournalIsRefused) {
    {
        std::ofstream out(this->path_);
        out << "this is not a journal at all, just some text";
    }
    this->make();
    try {
        this->exchange_->start();
        FAIL() << "a bad header was accepted";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("bad file header"), std::string::npos) << e.what();
    }
}

TYPED_TEST(ThreadedJournalTest, AJournalWrittenWithAnotherSetupIsRefused) {
    this->run_with_a_resting_order();
    this->make(/*extra_account=*/true);
    try {
        this->exchange_->start();
        FAIL() << "a journal with another setup was accepted";
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("another setup"), std::string::npos) << message;
        EXPECT_NE(message.find("Setup record 4"), std::string::npos) << message;
    }
}

TYPED_TEST(ThreadedJournalTest, AJournalThatEndsInsideItsSetupGetsTheRest) {
    write_crashed_journal(this->path_, 2, {});
    this->make();
    this->exchange_->start();
    EXPECT_EQ(this->exchange_->recovery().setup_added, 2U);
    EXPECT_TRUE(this->exchange_->recovery().recovered);
    auto bob = this->log_in("bob", "hunter2");
    bob->send(unsequenced(limit(Side::Sell, 1, 50, 50 * kTick)));
    ASSERT_TRUE(wait_until(
        {bob.get()}, [&] { return bob->template all<protocol::AcceptedMessage>().size() == 1; }));
    this->exchange_->stop();
    const auto replayed = replayed_replies(this->path_);
    EXPECT_EQ(replayed.at(kBob).size(), 1U);
}

TYPED_TEST(ThreadedJournalTest, AnEmptyJournalFileIsTreatedAsNew) {
    {
        const journal::JournalWriter writer = journal::JournalWriter::create(this->path_, {});
    }
    this->make();
    this->exchange_->start();
    EXPECT_FALSE(this->exchange_->recovery().recovered);
    EXPECT_EQ(this->exchange_->recovery().setup_added, 4U);
    this->exchange_->stop();
    EXPECT_EQ(records_in(this->path_).size(), 5U);  // The setup and Stop.
}

TYPED_TEST(ThreadedJournalTest, AFailedWriteStopsTheExchangeAndStopSaysWhy) {
    // Write 1 is the header and write 2 the setup, so the first batch of requests fails.
    g_fail_from = 3;
    this->make(false, journal::FileOps{.write = &failing_write, .sync = &::fdatasync});
    this->exchange_->start();
    auto bob = this->log_in("bob", "hunter2");
    bob->send(unsequenced(limit(Side::Sell, 1, 50, 50 * kTick)));
    // The exchange ends the session by itself.
    ASSERT_TRUE(wait_until({bob.get()}, [&] { return bob->count(PacketType::EndOfSession) == 1; }));
    pollfd finished{.fd = this->exchange_->finished_fd(), .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&finished, 1, 10'000), 1);
    try {
        this->exchange_->stop();
        FAIL() << "stop() did not report the failed journal";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("journal could not be written"), std::string::npos)
            << e.what();
    }
    EXPECT_TRUE(bob->template all<protocol::AcceptedMessage>().empty());
    EXPECT_EQ(this->exchange_->matching().stats().frames, 0U);
    ASSERT_NE(this->exchange_->journal(), nullptr);
    EXPECT_TRUE(this->exchange_->journal()->failed());
    EXPECT_GE(this->exchange_->journal()->stats().discarded, 1U);
    EXPECT_TRUE(this->exchange_->matching().stopped());
}

TYPED_TEST(ThreadedJournalTest, TheFinishedEventfdIsReadableOnlyAfterTheExchangeStopped) {
    this->make();
    this->exchange_->start();
    pollfd finished{.fd = this->exchange_->finished_fd(), .events = POLLIN, .revents = 0};
    EXPECT_EQ(::poll(&finished, 1, 0), 0);
    this->exchange_->stop();
    EXPECT_EQ(::poll(&finished, 1, 0), 1);
}

// Recovery with snapshots and segments.

// Bob rests `orders` sell orders at different prices, one at a time, and the exchange
// stops cleanly. Returns the UserRefNum of his last order.
template <class Test>
protocol::UserRefNum rest_bob_orders(Test& test, int orders) {
    test.make();
    test.exchange_->start();
    auto bob = test.log_in("bob", "hunter2");
    for (int i = 1; i <= orders; ++i) {
        const auto ref = static_cast<protocol::UserRefNum>(i);
        bob->send(unsequenced(limit(Side::Sell, ref, 10, (50 + i) * kTick)));
        EXPECT_TRUE(wait_until({bob.get()}, [&] {
            return bob->template all<protocol::AcceptedMessage>().size() ==
                   static_cast<std::size_t>(i);
        }));
    }
    test.exchange_->stop();
    return static_cast<protocol::UserRefNum>(orders);
}

// After a restart, Alice buys everything Bob has resting, which proves the book came
// back, and every login number carries on.
template <class Test>
void expect_book_back(Test& test, int orders) {
    auto bob = test.log_in("bob", "hunter2");
    EXPECT_EQ(test.login_sequence(*bob), static_cast<std::uint64_t>(orders) + 1);
    auto alice = test.log_in("alice", "secret");
    alice->send(unsequenced(limit(Side::Buy, 1, static_cast<Qty>(10 * orders), 1000 * kTick)));
    EXPECT_TRUE(wait_until({alice.get(), bob.get()}, [&] {
        return bob->template all<protocol::ExecutedMessage>().size() ==
               static_cast<std::size_t>(orders);
    }));
    test.exchange_->stop();
}

TYPED_TEST(ThreadedJournalTest, ACleanStopWritesASnapshotAndTheRestartStartsFromIt) {
    this->keep_snapshots_ = 2;
    rest_bob_orders(*this, 3);
    const journal::Sequence stop_snapshot = this->exchange_->stop_snapshot();
    EXPECT_GT(stop_snapshot, 0U);
    EXPECT_EQ(this->exchange_->stop_snapshot_error(), 0);

    this->make();
    this->exchange_->start();
    const RecoveryReport& report = this->exchange_->recovery();
    EXPECT_TRUE(report.recovered);
    EXPECT_EQ(report.snapshot_as_of, stop_snapshot);
    EXPECT_EQ(report.frames, 0U);  // Nothing after the snapshot to replay.
    EXPECT_EQ(report.replies, 3U);
    EXPECT_GT(report.start_snapshot, report.restart_sequence);
    expect_book_back(*this, 3);
}

TYPED_TEST(ThreadedJournalTest, ADamagedNewestSnapshotFallsBackToTheOneBefore) {
    this->keep_snapshots_ = 2;
    rest_bob_orders(*this, 2);
    const journal::Sequence newest = this->exchange_->stop_snapshot();
    const journal::DirectoryListing listing = journal::list_journal_directory(this->journal_dir_);
    ASSERT_EQ(listing.snapshots.size(), 2U);  // The start snapshot and the stop snapshot.
    {
        std::fstream file(listing.snapshots.back().path,
                          std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(60);
        file.put('\x55');
    }
    this->make();
    this->exchange_->start();
    const RecoveryReport& report = this->exchange_->recovery();
    EXPECT_EQ(report.snapshots_refused, 1U);
    EXPECT_LT(report.snapshot_as_of, newest);
    EXPECT_EQ(report.snapshot_as_of, listing.snapshots.front().as_of);
    EXPECT_EQ(report.frames, 2U);  // Bob's two orders came after the start snapshot.
    expect_book_back(*this, 2);
    // The damaged file is left for an operator to look at.
    EXPECT_TRUE(std::filesystem::exists(listing.snapshots.back().path));
}

TYPED_TEST(ThreadedJournalTest, SegmentsAndTheSnapshotThreadLetOldFilesGo) {
    this->keep_snapshots_ = 2;
    this->records_per_segment_ = 4;
    rest_bob_orders(*this, 12);
    const auto* snapshots = this->exchange_->snapshotter();
    ASSERT_NE(snapshots, nullptr);
    EXPECT_FALSE(snapshots->failed());
    EXPECT_GT(snapshots->stats().snapshots, 0U);
    const journal::DirectoryListing listing = journal::list_journal_directory(this->journal_dir_);
    EXPECT_LE(listing.snapshots.size(), 2U);
    ASSERT_FALSE(listing.segments.empty());
    EXPECT_GT(listing.segments.front().first, 1U);  // Segment 1 is gone.

    this->make();
    this->exchange_->start();
    EXPECT_GT(this->exchange_->recovery().snapshot_as_of, 0U);
    expect_book_back(*this, 12);
}

TYPED_TEST(ThreadedJournalTest,
           WithoutTheStopSnapshotTheSnapshotThreadsLatestAndTheLaterRecordsAreUsed) {
    this->keep_snapshots_ = 3;
    this->records_per_segment_ = 4;
    rest_bob_orders(*this, 10);
    const journal::Sequence stop_snapshot = this->exchange_->stop_snapshot();
    // As if the process had been killed before it could stop cleanly.
    journal::PathBuffer paths(this->journal_dir_);
    ASSERT_TRUE(std::filesystem::remove(paths.snapshot(stop_snapshot)));

    this->make();
    this->exchange_->start();
    const RecoveryReport& report = this->exchange_->recovery();
    EXPECT_GT(report.snapshot_as_of, 0U);
    EXPECT_LT(report.snapshot_as_of, stop_snapshot);
    EXPECT_EQ(report.stops, 1U);  // The Stop record came after the snapshot.
    expect_book_back(*this, 10);
}

TYPED_TEST(ThreadedJournalTest, ACutHeaderAtTheEndIsATornRotationAndIsRemoved) {
    this->keep_snapshots_ = 2;
    rest_bob_orders(*this, 1);
    const std::vector<journal::JournalRecord> records = records_in(this->path_);
    journal::PathBuffer paths(this->journal_dir_);
    const std::string torn = paths.segment(records.back().sequence + 1);
    {
        std::ofstream(torn, std::ios::binary) << "LIT";
    }  // The start of the magic.
    this->make();
    this->exchange_->start();
    EXPECT_TRUE(this->exchange_->recovery().torn_rotation);
    EXPECT_FALSE(std::filesystem::exists(torn));
    expect_book_back(*this, 1);
}

TYPED_TEST(ThreadedJournalTest, AMissingSegmentInTheMiddleIsRefused) {
    this->keep_snapshots_ = 0;  // Nothing is removed, so every segment stays.
    this->records_per_segment_ = 4;
    rest_bob_orders(*this, 8);
    const journal::DirectoryListing listing = journal::list_journal_directory(this->journal_dir_);
    ASSERT_GE(listing.segments.size(), 3U);
    std::filesystem::remove(listing.segments[1].path);
    this->make();
    try {
        this->exchange_->start();
        ADD_FAILURE() << "a journal with a gap must be refused";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("starts at record"), std::string::npos) << e.what();
    }
}

TYPED_TEST(ThreadedJournalTest, ASnapshotAndAFullReplayGiveTheSameExchange) {
    // The same orders on two journals, one with snapshots and one without. After a
    // restart both exchanges must hold the same state.
    this->keep_snapshots_ = 2;
    this->records_per_segment_ = 4;
    rest_bob_orders(*this, 9);
    this->make();
    this->exchange_->start();
    this->exchange_->stop();
    protocol::OrderEntryState with_snapshots;
    this->exchange_->matching().order_entry().capture(with_snapshots);

    this->journal_dir_ = this->dir_.file("plain");
    std::filesystem::create_directories(this->journal_dir_);
    this->keep_snapshots_ = 0;
    rest_bob_orders(*this, 9);
    this->make();
    this->exchange_->start();
    EXPECT_EQ(this->exchange_->recovery().snapshot_as_of, 0U);
    this->exchange_->stop();
    protocol::OrderEntryState plain;
    this->exchange_->matching().order_entry().capture(plain);
    EXPECT_EQ(with_snapshots, plain);
    EXPECT_EQ(plain.orders.size(), 9U);
}

// The market data feed after a restart. FeedWorker writes into a recording sink instead
// of sockets, which only the publisher thread touches until stop() returned.
TEST(ThreadedJournalFeedTest, AfterARestartTheFeedHasANewSessionAndTheRebuiltLevels) {
    using Exchange = BasicThreadedGateway<SortedOrderBook, concurrency::SpscQueue, FeedWorker>;
    const litbook::testing::TempDir dir;
    const JournalOptions journal{.directory = dir.file("feed")};
    feed::testing::RecordingSink sink;
    FeedWorkerOptions options;
    options.sink = &sink;
    options.feed.max_symbols = 1;

    auto run = [&](bool trade) {
        auto exchange =
            std::make_unique<Exchange>(gateway::SessionConfig{.max_connections = 8}, entry_config(),
                                       gateway::GatewayConfig{.clock = &still_clock},
                                       PipelineConfig{.wait = WaitMode::Spin}, options, &journal);
        exchange->add_account(kAliceAccount);
        exchange->add_account(kBobAccount);
        exchange->add_symbol(testing::aapl(), book_config());
        exchange->start();
        if (trade) {
            Peer bob(exchange->port());
            bob.send(login("bob", "hunter2"));
            bob.send(unsequenced(limit(Side::Sell, 1, 50, 50 * kTick)));
            EXPECT_TRUE(wait_until(
                {&bob}, [&] { return bob.all<protocol::AcceptedMessage>().size() == 1; }));
        }
        exchange->stop();
        return exchange;
    };

    const auto first = run(true);
    EXPECT_EQ(first->publisher().feed().session(), "1");
    const auto second = run(false);
    const journal::Sequence restart = second->recovery().restart_sequence;
    EXPECT_GT(restart, 0U);
    EXPECT_EQ(second->publisher().feed().session(), std::to_string(restart));
    const feed::LevelBook& copy = second->publisher().feed().copy(0);
    ASSERT_EQ(copy.level_count(Side::Sell), 1U);
    EXPECT_EQ(copy.level(Side::Sell, 0).price, 50 * kTick);
    EXPECT_EQ(copy.level(Side::Sell, 0).total_qty, 50U);
}

}  // namespace
}  // namespace litbook::pipeline
