#include "litbook/journal/replay.hpp"

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/pipeline/matching_worker.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "book/book_test_support.hpp"
#include "journal/journal_test_support.hpp"
#include "journal/replay_test_support.hpp"
#include "support/temp_dir.hpp"

namespace litbook::journal {
namespace {

using litbook::testing::TempDir;
using testing::apply_request;
using testing::expect_same_state;
using testing::RecordingEvents;
using testing::RecordingReplies;
using testing::replay_order_entry_config;
using testing::replay_stream;
using testing::set_up_exchange;
using testing::unwrap;

JournalWriterConfig plain_config() {
    JournalWriterConfig config;
    config.buffer_capacity = std::size_t{64} * 1024;
    config.sync = SyncPolicy::Write;
    return config;
}

// Writes the setup records for the exchange that set_up_exchange builds.
void journal_setup(JournalWriter& writer) {
    ASSERT_EQ(writer.append_session_start(1, replay_order_entry_config()), JournalStatus::Ok);
    SymbolId id = 0;
    for (const char* name : testing::kReplayStocks) {
        ASSERT_EQ(
            writer.append_add_symbol(1, SymbolListing{.id = id,
                                                      .ticker = unwrap(make_ticker(name)),
                                                      .config = testing::replay_book_config()}),
            JournalStatus::Ok);
        ++id;
    }
    ASSERT_EQ(writer.append_risk_limits(1, testing::kLimitedOwner, testing::kLimitedRisk),
              JournalStatus::Ok);
}

// The live side of a replay test. An order entry layer handles the requests one by one,
// as the matching thread would, and every request is journaled first, with commits at
// random batch sizes.
template <OrderBook Book>
struct LiveRun {
    RecordingReplies replies;
    pipeline::Timestamp now = 0;
    RecordingEvents events{&now};
    protocol::OrderEntry<Book> orders{replies, replay_order_entry_config()};

    LiveRun() {
        orders.set_market_data_sink(&events);
        set_up_exchange(orders);
    }

    void run(const std::vector<pipeline::Request>& requests, JournalWriter& writer,
             std::uint64_t seed) {
        std::mt19937_64 rng(seed);
        std::size_t until_commit = 1 + rng() % 16;
        for (const pipeline::Request& r : requests) {
            ASSERT_EQ(writer.append(r), JournalStatus::Ok);
            // Write-ahead. The request is in the journal before it is handled.
            if (--until_commit == 0) {
                ASSERT_EQ(writer.commit(), JournalStatus::Ok);
                until_commit = 1 + rng() % 16;
            }
            now = r.now;
            apply_request(orders, r);
        }
    }
};

// The replay side, over any book type.
template <OrderBook Book>
struct ReplayRun {
    RecordingReplies replies;
    pipeline::Timestamp now = 0;
    RecordingEvents events{&now};
    Replayer<Book> replayer{replies, &events};

    // Replays a journal file and keeps the events stamped with each record's time.
    ReplayOutcome replay(const std::string& path) {
        const MappedFile file(path);
        ReplayOutcome outcome;
        outcome.scan = scan_journal(file.bytes());
        RecordReader reader(
            file.bytes().subspan(kFileHeaderLength, outcome.scan.valid_length - kFileHeaderLength));
        JournalRecord record;
        while (reader.next(record)) {
            now = record.now;
            outcome.status = replayer.apply(record);
            if (outcome.status != ReplayStatus::Ok) {
                break;
            }
        }
        return outcome;
    }
};

template <class Book>
class ReplayTest : public ::testing::Test {};

TYPED_TEST_SUITE(ReplayTest, litbook::testing::BookTypes, litbook::testing::BookTypeNames);

TYPED_TEST(ReplayTest, ReplayGivesTheSameRepliesEventsAndState) {
    using Book = TypeParam;
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        SCOPED_TRACE(::testing::Message() << "seed " << seed);
        const TempDir dir;
        const std::string path = dir.file("day.journal");
        LiveRun<Book> live;
        {
            JournalWriter writer = JournalWriter::create(path, plain_config());
            journal_setup(writer);
            live.run(replay_stream(seed, 6'000), writer, seed);
            ASSERT_EQ(writer.append(pipeline::make_stop_request(live.now + 1)), JournalStatus::Ok);
            ASSERT_EQ(writer.flush(), JournalStatus::Ok);
        }

        ReplayRun<Book> replayed;
        const ReplayOutcome outcome = replayed.replay(path);
        ASSERT_EQ(outcome.status, ReplayStatus::Ok);
        EXPECT_EQ(outcome.scan.tail, TailStatus::Clean);
        EXPECT_EQ(replayed.replayer.stats().frames + replayed.replayer.stats().cancel_alls, 6'000U);
        EXPECT_EQ(replayed.replayer.stats().stops, 1U);
        EXPECT_EQ(replayed.replayer.stats().setup_records, 5U);

        // The stream must really have covered the interesting paths, or equality proves
        // little.
        protocol::OrderEntry<Book>& orders = replayed.replayer.order_entry();
        EXPECT_GT(orders.engine().last_match_id(), 200U);
        EXPECT_GT(orders.stats().malformed, 0U);
        EXPECT_GT(orders.stats().bad_owner, 0U);
        EXPECT_GT(orders.stats().unknown_order, 0U);
        EXPECT_GT(replayed.replayer.stats().cancel_alls, 50U);

        // Byte for byte the same replies, and the same market events at the same times.
        ASSERT_EQ(replayed.replies.replies.size(), live.replies.replies.size());
        EXPECT_TRUE(replayed.replies.replies == live.replies.replies);
        ASSERT_EQ(replayed.events.events.size(), live.events.events.size());
        EXPECT_TRUE(replayed.events.events == live.events.events);
        expect_same_state(live.orders, orders);

        // And the same future. Both handle new requests the same way from here on.
        const std::size_t replies_before = live.replies.replies.size();
        for (const pipeline::Request& r : replay_stream(seed + 100, 400, live.now + 10)) {
            live.now = r.now;
            replayed.now = r.now;
            apply_request(live.orders, r);
            apply_request(orders, r);
        }
        EXPECT_GT(live.replies.replies.size(), replies_before);
        EXPECT_TRUE(replayed.replies.replies == live.replies.replies);
        EXPECT_TRUE(replayed.events.events == live.events.events);
        expect_same_state(live.orders, orders);
    }
}

TYPED_TEST(ReplayTest, EachAccountsNextSequenceNumberCanBeRebuilt) {
    // The session layer gives every reply of an account the next sequence number, so an
    // account's next number is 1 plus its replies. Recovery rebuilds it this way.
    using Book = TypeParam;
    const TempDir dir;
    const std::string path = dir.file("day.journal");
    LiveRun<Book> live;
    {
        JournalWriter writer = JournalWriter::create(path, plain_config());
        journal_setup(writer);
        live.run(replay_stream(7, 2'000), writer, 7);
    }
    ReplayRun<Book> replayed;
    ASSERT_EQ(replayed.replay(path).status, ReplayStatus::Ok);
    std::map<OwnerId, std::uint64_t> live_next;
    std::map<OwnerId, std::uint64_t> replayed_next;
    for (const auto& r : live.replies.replies) {
        ++live_next[r.owner];
    }
    for (const auto& r : replayed.replies.replies) {
        ++replayed_next[r.owner];
    }
    EXPECT_EQ(live_next, replayed_next);
    EXPECT_EQ(live_next.size(), testing::kReplayOwners);
}

TYPED_TEST(ReplayTest, ATornTailIsLeftOutAndTheRestStillMatches) {
    using Book = TypeParam;
    const TempDir dir;
    const std::string path = dir.file("day.journal");
    const std::vector<pipeline::Request> requests = replay_stream(11, 500);
    {
        LiveRun<Book> unused;
        JournalWriter writer = JournalWriter::create(path, plain_config());
        journal_setup(writer);
        unused.run(requests, writer, 11);
    }
    // A crash cut the last record short.
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3);

    // What the exchange did before the crash, for every request but the last one.
    LiveRun<Book> reference;
    for (std::size_t i = 0; i + 1 < requests.size(); ++i) {
        reference.now = requests[i].now;
        apply_request(reference.orders, requests[i]);
    }
    ReplayRun<Book> replayed;
    const MappedFile file(path);
    const ReplayOutcome outcome = replay_journal(file.bytes(), replayed.replayer);
    ASSERT_EQ(outcome.status, ReplayStatus::Ok);
    EXPECT_EQ(outcome.scan.tail, TailStatus::Torn);
    EXPECT_TRUE(replayed.replies.replies == reference.replies.replies);
    expect_same_state(reference.orders, replayed.replayer.order_entry());
}

// Journals written over one book type replay into the other two with the same replies,
// which is the differential test of Phase 3 carried through the journal.
TEST(ReplayAcrossBooksTest, AJournalReplaysTheSameIntoEveryBook) {
    const TempDir dir;
    const std::string path = dir.file("day.journal");
    LiveRun<MapOrderBook> live;
    {
        JournalWriter writer = JournalWriter::create(path, plain_config());
        journal_setup(writer);
        live.run(replay_stream(21, 5'000), writer, 21);
    }
    ReplayRun<FastOrderBook> fast;
    ReplayRun<SortedOrderBook> sorted;
    ASSERT_EQ(fast.replay(path).status, ReplayStatus::Ok);
    ASSERT_EQ(sorted.replay(path).status, ReplayStatus::Ok);
    EXPECT_TRUE(fast.replies.replies == live.replies.replies);
    EXPECT_TRUE(sorted.replies.replies == live.replies.replies);
    EXPECT_TRUE(fast.events.events == live.events.events);
    EXPECT_TRUE(sorted.events.events == live.events.events);
    expect_same_state(live.orders, fast.replayer.order_entry());
    expect_same_state(live.orders, sorted.replayer.order_entry());
}

// The matching thread's worker, driven by hand, gives the same replies and events as a
// replay of the same requests. This ties the journal to the pipeline without threads.
TEST(ReplayMatchesTheWorkerTest, ReplayEqualsWhatTheMatchingThreadSent) {
    using Requests = concurrency::SpscQueue<pipeline::Request>;
    using Replies = concurrency::SpscQueue<pipeline::Reply>;
    using Events = concurrency::SpscQueue<pipeline::MarketEvent>;
    Requests requests(4);
    // One CancelAll can cause hundreds of replies and events, and the worker waits for
    // room forever, so the queues are far larger than any one request needs.
    Replies replies(1U << 16U);
    Events events(1U << 16U);
    pipeline::MatchingWorker<SortedOrderBook, Requests, Replies, Events> worker(
        replay_order_entry_config(), requests, replies, events, pipeline::WaitMode::Spin, {});
    set_up_exchange(worker.order_entry());

    const TempDir dir;
    const std::string path = dir.file("day.journal");
    std::vector<testing::RecordedReply> worker_replies;
    std::vector<pipeline::MarketEvent> worker_events;
    {
        JournalWriter writer = JournalWriter::create(path, plain_config());
        journal_setup(writer);
        for (const pipeline::Request& r : replay_stream(31, 3'000)) {
            ASSERT_EQ(writer.append(r), JournalStatus::Ok);
            ASSERT_EQ(writer.commit(), JournalStatus::Ok);
            ASSERT_TRUE(requests.try_push(r));
            ASSERT_TRUE(worker.step());
            while (const pipeline::Reply* reply = replies.front()) {
                if (reply->kind == pipeline::ReplyKind::Message) {
                    worker_replies.push_back(testing::RecordedReply{
                        .owner = reply->owner,
                        .bytes = {reply->bytes().begin(), reply->bytes().end()}});
                }
                replies.pop();
            }
            while (const pipeline::MarketEvent* e = events.front()) {
                worker_events.push_back(*e);
                events.pop();
            }
        }
    }

    ReplayRun<SortedOrderBook> replayed;
    ASSERT_EQ(replayed.replay(path).status, ReplayStatus::Ok);
    EXPECT_TRUE(replayed.replies.replies == worker_replies);
    ASSERT_EQ(replayed.events.events.size(), worker_events.size());
    for (std::size_t i = 0; i < worker_events.size(); ++i) {
        const pipeline::MarketEvent& w = worker_events[i];
        const testing::RecordedEvent& r = replayed.events.events[i];
        ASSERT_EQ(r.trade, w.kind == pipeline::MarketEventKind::Trade) << i;
        EXPECT_EQ(r.now, w.now) << i;
        EXPECT_EQ(r.symbol, w.symbol) << i;
        EXPECT_EQ(r.side, w.side) << i;
        EXPECT_EQ(r.price, w.price) << i;
        EXPECT_EQ(r.qty, w.qty) << i;
        EXPECT_EQ(r.order_count, w.order_count) << i;
        EXPECT_EQ(r.match_id, w.match_id) << i;
    }
    expect_same_state(worker.order_entry(), replayed.replayer.order_entry());
}

// Journals that do not make sense are refused with a reason.

std::vector<JournalRecord> records(std::initializer_list<std::vector<std::byte>> encoded) {
    std::vector<JournalRecord> out;
    for (const auto& e : encoded) {
        const DecodedRecord d = decode_record(e);
        EXPECT_EQ(d.status, RecordStatus::Ok);
        out.push_back(d.record);
    }
    return out;
}

std::vector<std::byte> start_record(Sequence seq) {
    return testing::encoded(
        [&](auto o) { return encode_session_start(o, seq, 0, replay_order_entry_config()); });
}

std::vector<std::byte> symbol_record(Sequence seq, SymbolId id, const char* ticker) {
    return testing::encoded(
        [&](auto o) { return encode_add_symbol(o, seq, 0, testing::sample_listing(id, ticker)); });
}

std::vector<std::byte> cancel_all_record(Sequence seq) {
    return testing::encoded(
        [&](auto o) { return encode_request(o, seq, pipeline::make_cancel_all_request(1, 0)); });
}

ReplayStatus replay_records(const std::vector<JournalRecord>& list) {
    RecordingReplies replies;
    Replayer<SortedOrderBook> replayer(replies);
    ReplayStatus status = ReplayStatus::Ok;
    for (const JournalRecord& r : list) {
        status = replayer.apply(r);
    }
    return status;
}

TEST(ReplayStatusTest, TheFirstRecordMustStartTheSession) {
    EXPECT_EQ(replay_records(records({cancel_all_record(1)})), ReplayStatus::MissingSessionStart);
    EXPECT_EQ(replay_records(records({symbol_record(1, 0, "AAPL")})),
              ReplayStatus::MissingSessionStart);
}

TEST(ReplayStatusTest, ThereIsOnlyOneSessionStart) {
    EXPECT_EQ(replay_records(records({start_record(1), start_record(2)})),
              ReplayStatus::SecondSessionStart);
}

TEST(ReplayStatusTest, SetupComesBeforeTheFirstRequest) {
    EXPECT_EQ(replay_records(
                  records({start_record(1), cancel_all_record(2), symbol_record(3, 0, "AAPL")})),
              ReplayStatus::SetupAfterRequests);
    const auto limits = testing::encoded(
        [&](auto o) { return encode_risk_limits(o, 3, 0, 1, protocol::RiskLimits{}); });
    EXPECT_EQ(replay_records(records({start_record(1), cancel_all_record(2), limits})),
              ReplayStatus::SetupAfterRequests);
}

TEST(ReplayStatusTest, AStockMustGetTheSameIdAgain) {
    // The engine numbers stocks 0, 1, 2 in order, so a journal that says the first stock
    // was 5 does not fit.
    EXPECT_EQ(replay_records(records({start_record(1), symbol_record(2, 5, "AAPL")})),
              ReplayStatus::SymbolRefused);
    EXPECT_EQ(replay_records(records(
                  {start_record(1), symbol_record(2, 0, "AAPL"), symbol_record(3, 1, "AAPL")})),
              ReplayStatus::SymbolRefused);
    EXPECT_EQ(replay_records(records(
                  {start_record(1), symbol_record(2, 0, "AAPL"), symbol_record(3, 1, "MSFT")})),
              ReplayStatus::Ok);
}

TEST(ReplayStatusTest, RiskLimitsNeedAnAccountThatExists) {
    const auto limits = testing::encoded([&](auto o) {
        return encode_risk_limits(o, 2, 0, testing::kReplayOwners + 1, protocol::RiskLimits{});
    });
    EXPECT_EQ(replay_records(records({start_record(1), limits})), ReplayStatus::RiskLimitsRefused);
}

TEST(ReplayStatusTest, AFailureSticksAndChangesNothingMore) {
    RecordingReplies replies;
    Replayer<SortedOrderBook> replayer(replies);
    EXPECT_FALSE(replayer.started());
    EXPECT_THROW(static_cast<void>(replayer.order_entry()), std::logic_error);
    const auto list = records({start_record(1), symbol_record(2, 3, "AAPL"), cancel_all_record(3)});
    EXPECT_EQ(replayer.apply(list[0]), ReplayStatus::Ok);
    EXPECT_EQ(replayer.apply(list[1]), ReplayStatus::SymbolRefused);
    EXPECT_EQ(replayer.apply(list[2]), ReplayStatus::SymbolRefused);
    EXPECT_EQ(replayer.status(), ReplayStatus::SymbolRefused);
    EXPECT_EQ(replayer.stats().records, 1U);
    EXPECT_EQ(replayer.stats().cancel_alls, 0U);
}

TEST(ReplayStatusTest, AStopInTheMiddleIsOnlyCounted) {
    // An exchange that stopped cleanly and was started again from the same journal has a
    // Stop record in the middle, and replay goes on past it.
    const auto stop = testing::encoded(
        [&](auto o) { return encode_request(o, 3, pipeline::make_stop_request(0)); });
    RecordingReplies replies;
    Replayer<SortedOrderBook> replayer(replies);
    for (const JournalRecord& r :
         records({start_record(1), symbol_record(2, 0, "AAPL"), stop, cancel_all_record(4)})) {
        ASSERT_EQ(replayer.apply(r), ReplayStatus::Ok);
    }
    EXPECT_EQ(replayer.stats().stops, 1U);
    EXPECT_EQ(replayer.stats().cancel_alls, 1U);
    EXPECT_EQ(replayer.stats().last_sequence, 4U);
}

TEST(ReplayStatusTest, ACorruptOrUnreadableJournalIsNotReplayedAtAll) {
    std::vector<std::byte> file(kFileHeaderLength);
    encode_file_header(file, 1);
    for (const auto& r : {start_record(1), cancel_all_record(2), cancel_all_record(3)}) {
        file.insert(file.end(), r.begin(), r.end());
    }
    std::vector<std::byte> corrupt = file;
    corrupt[kFileHeaderLength + start_record(1).size() + 9] ^= std::byte{1};
    RecordingReplies replies;
    Replayer<SortedOrderBook> replayer(replies);
    EXPECT_EQ(replay_journal(std::span<const std::byte>(corrupt), replayer).status,
              ReplayStatus::JournalCorrupt);
    EXPECT_FALSE(replayer.started());
    std::vector<std::byte> unreadable = file;
    unreadable[0] = std::byte{0};
    EXPECT_EQ(replay_journal(std::span<const std::byte>(unreadable), replayer).status,
              ReplayStatus::JournalUnreadable);
    EXPECT_FALSE(replayer.started());
    EXPECT_EQ(replay_journal(std::span<const std::byte>(file), replayer).status, ReplayStatus::Ok);
    EXPECT_EQ(replayer.stats().cancel_alls, 2U);
}

}  // namespace
}  // namespace litbook::journal
