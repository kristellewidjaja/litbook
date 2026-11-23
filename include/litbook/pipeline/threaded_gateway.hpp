#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/events.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/concurrency/cpu_relax.hpp"
#include "litbook/concurrency/message_queue.hpp"
#include "litbook/concurrency/thread_affinity.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/gateway/socket.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/journal/snapshot_format.hpp"
#include "litbook/journal/snapshot_store.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/journal_worker.hpp"
#include "litbook/pipeline/matching_worker.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/pipeline/publisher.hpp"
#include "litbook/pipeline/queue_backend.hpp"
#include "litbook/pipeline/reply_pump.hpp"
#include "litbook/pipeline/snapshot_worker.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace litbook::pipeline {

struct PipelineConfig {
    WaitMode wait = WaitMode::Spin;
    // Queue sizes, each a power of two. With a journal, the queue from the journal thread
    // to the matching thread has request_capacity slots too.
    std::size_t request_capacity = 1024;
    std::size_t reply_capacity = 4096;
    std::size_t event_capacity = 4096;
    // The most replies the gateway hands to the session layer per round.
    std::size_t max_replies_per_drain = 1024;
    // The CPU each thread pins itself to, or -1 to leave it to the scheduler.
    int gateway_cpu = -1;
    int matching_cpu = -1;
    int publisher_cpu = -1;
    int journal_cpu = -1;
    int snapshot_cpu = -1;
};

enum class PipelineThread : std::uint8_t { Gateway, Matching, Publisher, Journal, Snapshot };
inline constexpr std::size_t kPipelineThreads = 5;

// Where the journal lives and how it is written. With a journal the pipeline gets a
// fourth thread, the journal thread, between the gateway and the matching thread, and
// with snapshots a fifth, the snapshot thread (see docs/JOURNAL.md).
struct JournalOptions {
    // The journal directory, with its segment and snapshot files. It is created if it
    // does not exist.
    std::string directory;
    journal::JournalWriterConfig writer{};
    // The journal thread starts a new segment after this many records. 0 keeps the
    // journal in one segment.
    std::uint64_t records_per_segment = 1'000'000;
    // How many snapshots to keep. Snapshots are written at every start and at a clean
    // stop, and older snapshots and the segments they make unnecessary are removed. 0
    // turns snapshots off, so nothing is ever removed.
    std::size_t keep_snapshots = 2;
    // Also take a snapshot each time a segment closes, on a snapshot thread that keeps a
    // second copy of the exchange. It needs snapshots and segments.
    bool snapshot_thread = true;
};

// What start() found in the journal and did with it.
struct RecoveryReport {
    // True if the journal already had records, so the exchange was rebuilt from it.
    bool recovered = false;
    std::uint64_t records = 0;    // Good records found in the journal.
    std::size_t torn_bytes = 0;   // Bytes of a torn last record that were cut off.
    std::size_t setup_added = 0;  // Setup records a journal without requests was missing.
    std::uint64_t frames = 0;     // Requests replayed.
    std::uint64_t cancel_alls = 0;
    std::uint64_t stops = 0;     // Clean shutdowns found in the journal.
    std::uint64_t restarts = 0;  // Earlier recoveries found in the journal.
    std::uint64_t replies = 0;   // Replies the replay produced, every account together.
    // The sequence number of the Restart record this start wrote, which also names the
    // market data feed's session. 0 when the journal was new.
    journal::Sequence restart_sequence = 0;
    // Cancel All records written for the accounts with cancel on disconnect, whose
    // connections the crash took away.
    std::uint32_t cancels_on_restart = 0;
    // The snapshot recovery started from (its as_of), or 0 if it replayed from record 1.
    journal::Sequence snapshot_as_of = 0;
    // Snapshot files that could not be used (damaged, or not continued by the segments).
    std::uint32_t snapshots_refused = 0;
    std::uint32_t segments = 0;  // Segment files in the journal.
    // True if the last segment's header was cut short (a crash while it was created), so
    // the file was removed.
    bool torn_rotation = false;
    // The snapshot this start wrote (its as_of), or 0 without snapshots.
    journal::Sequence start_snapshot = 0;
    std::uint32_t files_removed = 0;  // Old snapshots and segments removed at start.
};

// The exchange on three threads, or four with a journal. The gateway thread runs the
// epoll loop and the session layer, the matching thread runs the order entry layer and
// every book, the publisher thread consumes the market events, and the journal thread
// writes every request to the journal before the matching thread may see it. They are
// connected by queues of type Queue (SpscQueue, or MutexQueue for the comparison), each
// with one producer and one consumer.
//
//   gateway thread pushes Request to the journal thread (or straight to matching)
//   journal thread pushes the journaled Request to the matching thread
//   matching thread pushes Reply to the gateway thread
//   matching thread pushes MarketEvent to the publisher thread
//
// Setup (add_account, add_symbol) happens before start(). start() opens the journal if
// there is one, launches the threads, and stop() shuts them down in order. The gateway
// sends End of Session, closes every connection (so cancel on disconnect runs), sends Stop
// down the request queue, and keeps draining replies until the matching thread answers
// Stopped. Stop travels as a message, so it is handled after everything that was queued
// before it. The statistics and the order entry layer may be read once stop() has
// returned.
//
// Recovery. If the journal file already exists, start() rebuilds the exchange from it
// before any thread runs. It refuses a journal with a bad header or corruption in the
// middle, and one whose setup records differ from this start's accounts and stocks. It
// replays every request into the matching thread's order entry layer, cuts off a torn
// last record, writes a Restart record, and cancels the orders of every account with
// cancel on disconnect (journaled first, as every request is). Each account's next
// sequence number becomes 1 plus the replies it had, and the market data feed starts a
// new session named after the Restart record, so receivers fill their books again from
// snapshots.
//
// Invariants. Before start() and after stop() only the calling thread touches anything.
// In between each queue has exactly one producer thread and one consumer thread, and the
// gateway thread never waits for the matching or journal thread. With a journal, every
// request the matching thread handles was committed to the journal first.
//
// The publisher thread runs Publisher<Queue<MarketEvent>>. ThreadedGateway (below) uses
// PublisherStub, which only counts the events, and FeedWorker publishes the market data
// feed. Each stock is listed with the publisher right after the engine, so the feed's
// locates are the engine's symbol ids.
//
// Publisher is a template, not a finished type, so that callers never have to write
// Queue<MarketEvent> themselves. Clang 21 crashes when that is written inside a partial
// specialization whose Queue is a constrained template template parameter, which is how
// the typed tests pick their queue.
template <OrderBook Book, template <concurrency::QueueElement> class Queue,
          template <concurrency::MessageQueue> class PublisherTemplate>
class BasicThreadedGateway final : private JournalFailureListener {
public:
    using Requests = Queue<Request>;
    using Replies = Queue<Reply>;
    using Events = Queue<MarketEvent>;
    using Backend = QueueBackend<Book, Requests>;
    using Sessions = gateway::BasicSessionManager<Backend>;
    using Pump = ReplyPump<Sessions, Replies>;
    using Gateway = gateway::BasicEpollGateway<Sessions, Pump>;
    using Worker = MatchingWorker<Book, Requests, Replies, Events>;
    using Journal = JournalWorker<Requests, Requests>;
    using Snapshotter = SnapshotWorker<Book>;
    using Publisher = PublisherTemplate<Events>;
    using PublisherOptions = typename Publisher::Options;

    // Opens the listening socket. Throws std::invalid_argument for a bad configuration
    // or queue capacity, and std::system_error if a socket or eventfd cannot be made.
    // The publisher reads the same clock as the gateway (network.clock). With `journal`
    // the exchange journals every request (see the class comment). The journal file is
    // only opened in start().
    BasicThreadedGateway(const gateway::SessionConfig& session,
                         const protocol::OrderEntryConfig& orders,
                         const gateway::GatewayConfig& network, const PipelineConfig& pipeline,
                         const PublisherOptions& publisher_options = {},
                         const JournalOptions* journal = nullptr)
        : requests_(pipeline.request_capacity),
          journaled_(journal != nullptr ? pipeline.request_capacity : 1),
          replies_(pipeline.reply_capacity),
          events_(pipeline.event_capacity),
          worker_(orders, journal != nullptr ? journaled_ : requests_, replies_, events_,
                  pipeline.wait, matching_bells_for(pipeline.wait, journal != nullptr)),
          publisher_(events_, pipeline.wait, bells_for(pipeline.wait).events, network.clock,
                     publisher_options),
          sessions_(session, std::in_place, worker_.order_entry(), requests_,
                    bells_for(pipeline.wait).requests),
          pump_(sessions_, replies_, bells_for(pipeline.wait).replies,
                pipeline.max_replies_per_drain),
          gateway_(sessions_, network, pump_),
          pipeline_(pipeline),
          clock_(network.clock),
          finished_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
        if (!finished_.valid()) {
            throw std::system_error(errno, std::generic_category(), "eventfd");
        }
        if (journal != nullptr) {
            journal_options_ = *journal;
            setup_.push_back(journal::session_start_record(orders));
        }
    }

    BasicThreadedGateway(const BasicThreadedGateway&) = delete;
    BasicThreadedGateway& operator=(const BasicThreadedGateway&) = delete;
    BasicThreadedGateway(BasicThreadedGateway&&) = delete;
    BasicThreadedGateway& operator=(BasicThreadedGateway&&) = delete;

    // Stops the threads if they still run. An error from a thread is lost here, so call
    // stop() first to see it.
    ~BasicThreadedGateway() override {
        try {
            stop();
        } catch (...) {  // NOLINT(bugprone-empty-catch) A destructor must not throw.
        }
    }

    // Startup calls. Both throw std::logic_error after start(), because the threads then
    // own the state they change. If the publisher refuses a stock that the engine already
    // listed, add_symbol throws and the pipeline can no longer start, because the feed and
    // the engine would disagree about the stocks.
    OwnerId add_account(const gateway::AccountConfig& account) {
        require_not_started();
        const OwnerId owner = sessions_.add_account(account);
        if (journal_options_) {
            setup_.push_back(journal::risk_limits_record(owner, account.limits));
        }
        return owner;
    }
    SymbolId add_symbol(Ticker ticker, const BookConfig& config) {
        require_not_started();
        const SymbolId id = sessions_.add_symbol(ticker, config);
        try {
            publisher_.add_symbol(id, ticker, config);
            if (journal_options_) {
                setup_.push_back(journal::add_symbol_record(
                    journal::SymbolListing{.id = id, .ticker = ticker, .config = config}));
            }
        } catch (...) {
            setup_failed_ = true;
            throw;
        }
        return id;
    }

    [[nodiscard]] std::uint16_t port() const { return gateway_.port(); }

    // Opens the journal (and recovers from it, see the class comment), then launches the
    // threads. Throws std::logic_error if called twice or after a failed add_symbol,
    // std::runtime_error if the journal cannot be used (bad header, corruption, another
    // setup, or a failed write), and std::system_error if a file or thread cannot be
    // made. If the journal is refused, nothing runs and the pipeline cannot start again.
    void start() {
        require_not_started();
        if (setup_failed_) {
            throw std::logic_error("a stock could not be listed with the publisher");
        }
        if (journal_options_) {
            try {
                open_journal();
            } catch (...) {
                worker_.end_redirect();
                setup_failed_ = true;
                throw;
            }
        }
        started_ = true;
        publisher_thread_ = std::thread([this] { publisher_main(); });
        matching_thread_ = std::thread([this] { matching_main(); });
        if (journal_) {
            journal_thread_ = std::thread([this] { journal_main(); });
        }
        if (snapshotter_) {
            snapshot_thread_ = std::thread([this] { snapshot_main(); });
        }
        gateway_thread_ = std::thread([this] { gateway_main(); });
    }

    // Shuts the pipeline down and joins the threads. Calling it again does nothing. If a
    // thread failed with an exception, or the journal could not be written, the first such
    // error is rethrown here.
    void stop() {
        if (!started_ || joined_) {
            return;
        }
        gateway_.stop();
        join(gateway_thread_);
        join(journal_thread_);
        join(matching_thread_);
        join(publisher_thread_);
        if (snapshotter_) {
            snapshotter_->request_stop();
        }
        join(snapshot_thread_);
        joined_ = true;
        write_stop_snapshot();
        for (const std::exception_ptr& error : errors_) {
            if (error) {
                std::rethrow_exception(error);
            }
        }
    }

    // Read these only after stop().
    [[nodiscard]] const Sessions& sessions() const noexcept { return sessions_; }
    [[nodiscard]] const Worker& matching() const noexcept { return worker_; }
    [[nodiscard]] const Publisher& publisher() const noexcept { return publisher_; }
    [[nodiscard]] const Pump& pump() const noexcept { return pump_; }
    // The journal thread's worker, or nullptr without a journal or before start().
    [[nodiscard]] const Journal* journal() const noexcept {
        return journal_ ? &*journal_ : nullptr;
    }
    // The snapshot thread's worker, or nullptr without one.
    [[nodiscard]] const Snapshotter* snapshotter() const noexcept {
        return snapshotter_ ? &*snapshotter_ : nullptr;
    }
    // The snapshot stop() wrote (its as_of), 0 if none, and the errno if writing it failed.
    // A failed stop snapshot loses nothing, because the journal holds every record.
    [[nodiscard]] journal::Sequence stop_snapshot() const noexcept { return stop_snapshot_; }
    [[nodiscard]] int stop_snapshot_error() const noexcept { return stop_snapshot_error_; }
    // 0 if the thread was pinned or not asked to be, otherwise the error number.
    [[nodiscard]] int pin_result(PipelineThread thread) const noexcept {
        return pin_results_[static_cast<std::size_t>(thread)];
    }
    [[nodiscard]] const PipelineConfig& config() const noexcept { return pipeline_; }

    // These may be read any time after start() returned.
    [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
    // An eventfd that becomes readable once the gateway thread has finished shutting the
    // exchange down, whether stop() asked for it or the journal failed. A program can
    // wait for it next to its signals and then call stop().
    [[nodiscard]] int finished_fd() const noexcept { return finished_.get(); }

private:
    // Counts the replies and passes on the level updates that replaying the journal
    // produces. Nothing is sent, because the clients that got those replies already have
    // them (or lost them in the crash).
    class RecoverySink final : public protocol::OutboundSink, public protocol::MarketDataSink {
    public:
        RecoverySink(OwnerId max_owners, Publisher& publisher)
            : replies_(static_cast<std::size_t>(max_owners) + 1, 0), publisher_(&publisher) {}

        // The time of the record being replayed, for the level updates it causes.
        void set_now(Timestamp now) noexcept { now_ = now; }

        void send(OwnerId owner, std::span<const std::byte> /*frame*/) override {
            ++total_;
            if (owner < replies_.size()) {
                ++replies_[owner];
            }
        }
        void on_trade(SymbolId /*symbol*/, const Trade& /*trade*/) override {}
        void on_level_update(SymbolId symbol, const LevelUpdate& update) override {
            MarketEvent e;
            e.now = now_;
            e.kind = MarketEventKind::LevelUpdate;
            e.side = update.side;
            e.symbol = symbol;
            e.order_count = update.order_count;
            e.price = update.price;
            e.qty = update.total_qty;
            publisher_->restore(e);
        }

        // Starts the counts from a snapshot's reply counts.
        void restore_replies(const std::vector<std::uint64_t>& replies) {
            for (std::size_t owner = 0; owner < replies.size() && owner < replies_.size();
                 ++owner) {
                replies_[owner] = replies[owner];
                total_ += replies[owner];
            }
        }

        [[nodiscard]] std::uint64_t replies_of(OwnerId owner) const { return replies_.at(owner); }
        [[nodiscard]] std::uint64_t total() const noexcept { return total_; }

    private:
        std::vector<std::uint64_t> replies_;  // Indexed by OwnerId.
        Publisher* publisher_;
        Timestamp now_ = 0;
        std::uint64_t total_ = 0;
    };

    [[nodiscard]] bool blocking() const noexcept { return pipeline_.wait == WaitMode::Block; }

    // The doorbells exist in both modes, but only a blocking pipeline uses them. This
    // takes the mode as an argument because it is called while pipeline_ is not built yet.
    // The request doorbell wakes whoever reads the gateway's queue, the journal thread if
    // there is one and the matching thread otherwise.
    [[nodiscard]] Doorbells bells_for(WaitMode mode) noexcept {
        if (mode != WaitMode::Block) {
            return Doorbells{};
        }
        return Doorbells{
            .requests = &request_bell_, .replies = &reply_bell_, .events = &event_bell_};
    }

    // With a journal, the matching thread reads the journal thread's queue, so it sleeps
    // on that queue's doorbell instead.
    [[nodiscard]] Doorbells matching_bells_for(WaitMode mode, bool journaled) noexcept {
        Doorbells bells = bells_for(mode);
        if (journaled && bells.requests != nullptr) {
            bells.requests = &journaled_bell_;
        }
        return bells;
    }

    void require_not_started() const {
        if (started_) {
            throw std::logic_error("the pipeline has already started");
        }
    }

    // The journal's options and worker. Recovery only runs with a journal, and the journal
    // thread only once the worker exists, so these throw std::logic_error only after a
    // mistake in this class.
    [[nodiscard]] const JournalOptions& journal_options() const {
        if (!journal_options_) {
            throw std::logic_error("the pipeline has no journal");
        }
        return *journal_options_;
    }
    [[nodiscard]] Journal& journal_worker() {
        if (!journal_) {
            throw std::logic_error("the journal thread has no worker");
        }
        return *journal_;
    }
    [[nodiscard]] const Journal& journal_worker() const {
        if (!journal_) {
            throw std::logic_error("the journal thread has no worker");
        }
        return *journal_;
    }
    // The names of the journal's files, which exist once open_journal() has started.
    [[nodiscard]] journal::PathBuffer& paths() {
        if (!paths_) {
            throw std::logic_error("the journal directory was not opened");
        }
        return *paths_;
    }
    // The snapshot thread's worker, which only runs once it exists.
    [[nodiscard]] Snapshotter& snapshot_worker() {
        if (!snapshotter_) {
            throw std::logic_error("the snapshot thread has no worker");
        }
        return *snapshotter_;
    }

    // Runs on the journal thread when the journal can no longer be written. Stopping the
    // gateway is only an atomic store and an eventfd write, so any thread may do it.
    void on_journal_failure() noexcept override { gateway_.stop(); }

    // Recovery and opening the journal. All of this runs on the thread that calls start(),
    // before any other thread exists.

    void open_journal() {
        const JournalOptions& options = journal_options();
        const protocol::Timestamp now = clock_();
        std::filesystem::create_directories(options.directory);
        paths_.emplace(options.directory);

        const journal::DirectoryListing listing =
            journal::list_journal_directory(options.directory);
        // A snapshot a crash left half written is never used, so it can go.
        for (const std::string& path : listing.temporary) {
            std::filesystem::remove(path);
        }
        const journal::ChainScan chain = journal::scan_segment_chain(listing);
        refuse_unusable(chain);
        recovery_.segments = static_cast<std::uint32_t>(chain.segments.size());
        if (!chain.torn_rotation.empty()) {
            std::filesystem::remove(chain.torn_rotation);
            recovery_.torn_rotation = true;
        }
        std::optional<journal::Snapshot> snapshot = choose_snapshot(listing, chain);

        if (chain.status == journal::ChainStatus::Empty) {
            if (!listing.snapshots.empty()) {
                throw std::runtime_error("the journal " + options.directory +
                                         " has snapshots but no segment, so the records "
                                         "after them are missing");
            }
            journal::JournalWriter writer =
                journal::JournalWriter::create(paths().segment(1), options.writer);
            append_setup(writer, now, 0);
            require_written(writer.flush(), "while writing the setup records");
            start_snapshots(writer, now);
            begin_journal(std::move(writer));
            return;
        }
        const journal::Sequence first_kept = chain.segments.front().scan.first_sequence;
        if (!snapshot && first_kept != 1) {
            throw std::runtime_error("the journal " + options.directory + " starts at record " +
                                     std::to_string(first_kept) +
                                     ", and no usable snapshot holds the records before it");
        }

        RecoverySink sink(worker_.order_entry().config().max_owners, publisher_);
        worker_.redirect_output(sink, sink);
        const journal::Sequence as_of = snapshot ? snapshot->as_of : 0;
        if (snapshot) {
            restore_from_snapshot(*snapshot, sink);
        }
        const std::size_t setup_found = replay_segments(chain, as_of, sink);

        const journal::SegmentScan& last = chain.segments.back();
        recovery_.records = chain.records;
        recovery_.torn_bytes = last.scan.file_length - last.scan.valid_length;
        journal::JournalWriter writer =
            journal::JournalWriter::resume(last.file.path, last.scan, options.writer);
        if (!snapshot) {
            // A journal that ends inside its setup records (a crash during the very first
            // start) gets the rest of them now. replay_segments checked that the ones it
            // has agree with this start.
            recovery_.setup_added = setup_.size() - setup_found;
            append_setup(writer, now, setup_found);
        }
        if (snapshot || chain.records > 0) {
            finish_recovery(writer, now, sink);
        } else {
            require_written(writer.flush(), "while writing the setup records");
        }
        worker_.end_redirect();
        start_snapshots(writer, now);
        begin_journal(std::move(writer));
    }

    void refuse_unusable(const journal::ChainScan& chain) const {
        if (chain.status == journal::ChainStatus::Ok ||
            chain.status == journal::ChainStatus::Empty) {
            return;
        }
        const std::string& path = chain.problem.file.path;
        const journal::JournalScan& scan = chain.problem.scan;
        std::ostringstream os;
        os << "the journal segment " << path << " ";
        switch (chain.status) {
            case journal::ChainStatus::BadSegment:
                if (scan.tail == journal::TailStatus::BadHeader) {
                    os << "has a bad file header (" << scan.header << ")";
                } else {
                    os << "is corrupt. Its good records stop at byte " << scan.valid_length
                       << ", but another good record starts at byte " << scan.corrupt_at
                       << ". Cutting it there would lose good records";
                }
                break;
            case journal::ChainStatus::NameMismatch:
                os << "has a name that does not match the first record in its header ("
                   << scan.first_sequence << ")";
                break;
            case journal::ChainStatus::Gap:
                os << "starts at record " << scan.first_sequence
                   << ", but the segment before it ended before record " << chain.next_sequence;
                break;
            case journal::ChainStatus::TornInside:
                os << "ends with a torn record although later segments follow it";
                break;
            case journal::ChainStatus::Ok:
            case journal::ChainStatus::Empty:
                break;
        }
        os << ", so the exchange will not start from it";
        throw std::runtime_error(os.str());
    }

    // Reads every snapshot, newest first. Each one that decodes, has this start's setup
    // and is continued by the segments is good and kept for retention. The newest good one
    // is returned. A snapshot written by an exchange with another setup refuses the start.
    std::optional<journal::Snapshot> choose_snapshot(const journal::DirectoryListing& listing,
                                                     const journal::ChainScan& chain) {
        good_snapshots_.clear();
        std::optional<journal::Snapshot> newest;
        for (std::size_t k = listing.snapshots.size(); k > 0; --k) {
            const journal::SnapshotFile& file = listing.snapshots[k - 1];
            journal::LoadedSnapshot loaded = journal::read_snapshot_file(file);
            if (loaded.status != journal::SnapshotStatus::Ok) {
                ++recovery_.snapshots_refused;
                continue;
            }
            require_same_setup(loaded.snapshot.setup, setup_);
            // The records after the snapshot must be in the segments.
            if (!journal::snapshot_is_continued(chain, file.as_of)) {
                ++recovery_.snapshots_refused;
                continue;
            }
            good_snapshots_.insert(good_snapshots_.begin(), file);
            if (!newest) {
                newest = std::move(loaded.snapshot);
            }
        }
        return newest;
    }

    // Puts the matching thread's order entry layer, the reply counts and the feed's level
    // copy in the state of the snapshot. Nothing is sent.
    void restore_from_snapshot(const journal::Snapshot& snapshot, RecoverySink& sink) {
        recovery_.snapshot_as_of = snapshot.as_of;
        protocol::OrderEntry<Book>& orders = worker_.order_entry();
        try {
            orders.restore(snapshot.state);
        } catch (const std::invalid_argument& e) {
            throw std::runtime_error("the snapshot as of record " + std::to_string(snapshot.as_of) +
                                     " in " + journal_options().directory +
                                     " does not fit this exchange. " + e.what());
        }
        sink.restore_replies(snapshot.replies);
        // The feed's copy of the books is rebuilt from the restored books, level by level,
        // as if each level had just been updated.
        sink.set_now(snapshot.as_of_time);
        const auto& engine = orders.engine();
        for (std::size_t s = 0; s < engine.symbol_count(); ++s) {
            const auto symbol = static_cast<SymbolId>(s);
            const Book& book = *engine.book(symbol);
            for (const Side side : {Side::Buy, Side::Sell}) {
                for (const LevelSummary& level : book.depth(side, book.level_count(side))) {
                    sink.on_level_update(symbol, LevelUpdate{.side = side,
                                                             .price = level.price,
                                                             .total_qty = level.total_qty,
                                                             .order_count = level.order_count});
                }
            }
        }
    }

    // Replays every record after `as_of` of every segment into the matching thread's order
    // entry layer, with its output going to `sink`. Without a snapshot (as_of 0) the setup
    // records at the start are compared with this start's. Returns how many setup records
    // the journal has (0 with a snapshot, which holds the setup itself).
    std::size_t replay_segments(const journal::ChainScan& chain, journal::Sequence as_of,
                                RecoverySink& sink) {
        std::vector<journal::JournalRecord> found;
        bool requests_seen = as_of != 0;
        for (const journal::SegmentScan& segment : chain.segments) {
            // Each file stays mapped only while it is replayed. The last one must be
            // unmapped before the writer cuts a torn tail off.
            const journal::MappedFile file(segment.file.path);
            const std::span<const std::byte> records = file.bytes().subspan(
                journal::kFileHeaderLength, segment.scan.valid_length - journal::kFileHeaderLength);
            journal::RecordReader reader(records);
            journal::JournalRecord record;
            while (reader.next(record)) {
                if (record.sequence <= as_of) {
                    continue;
                }
                if (journal::is_setup_record(record.type)) {
                    if (requests_seen) {
                        throw std::runtime_error("the journal " + journal_options().directory +
                                                 " lists setup records after its first request");
                    }
                    found.push_back(record);
                    continue;
                }
                if (!requests_seen) {
                    requests_seen = true;
                    require_same_setup(found, setup_);
                }
                replay_record(record, sink);
            }
        }
        if (!requests_seen) {
            // Without requests the journal may hold only the first part of the setup.
            // A journal with more setup records than this start is compared in full, so the
            // message names the first extra record.
            const std::size_t compared =
                found.size() > setup_.size() ? setup_.size() : found.size();
            const std::span<const journal::JournalRecord> expected(setup_.data(), compared);
            if (found.size() > setup_.size()) {
                require_same_setup(found, setup_);
            } else {
                require_same_setup(found, expected);
            }
        }
        return found.size();
    }

    void require_same_setup(std::span<const journal::JournalRecord> found,
                            std::span<const journal::JournalRecord> expected) const {
        const std::string difference = journal::setup_difference(found, expected);
        if (!difference.empty()) {
            throw std::runtime_error("the journal " + journal_options().directory +
                                     " was written by an exchange with another setup, so "
                                     "this one will not start from it. " +
                                     difference);
        }
    }

    // The same calls MatchingWorker::handle makes, with the record's own time.
    void replay_record(const journal::JournalRecord& record, RecoverySink& sink) {
        sink.set_now(record.now);
        switch (record.type) {
            case journal::RecordType::Frame:
                ++recovery_.frames;
                worker_.order_entry().on_frame(record.owner, record.payload_bytes(), record.now);
                return;
            case journal::RecordType::CancelAll:
                ++recovery_.cancel_alls;
                worker_.order_entry().cancel_all(record.owner, record.now);
                return;
            case journal::RecordType::Stop:
                ++recovery_.stops;
                return;
            case journal::RecordType::Restart:
                ++recovery_.restarts;
                return;
            case journal::RecordType::SessionStart:
            case journal::RecordType::AddSymbol:
            case journal::RecordType::RiskLimits:
                return;  // replay_segments handles these.
        }
    }

    // A snapshot of the exchange as it is now, as of the last record the writer wrote.
    // Every account's replies are its next sequence number minus 1.
    [[nodiscard]] journal::Snapshot snapshot_now(journal::Sequence as_of,
                                                 protocol::Timestamp now) const {
        journal::Snapshot s;
        s.as_of = as_of;
        s.as_of_time = now;
        s.setup = setup_;
        s.replies.assign(static_cast<std::size_t>(worker_.order_entry().config().max_owners) + 1,
                         0);
        for (std::size_t i = 1; i <= sessions_.account_count(); ++i) {
            s.replies[i] = sessions_.next_sequence(static_cast<OwnerId>(i)) - 1;
        }
        worker_.order_entry().capture(s.state);
        return s;
    }

    // Writes a snapshot file and returns 0 or the errno.
    int write_snapshot(const journal::Snapshot& s) {
        std::vector<std::byte> bytes(journal::snapshot_size(s));
        if (bytes.empty() || journal::encode_snapshot(s, bytes) != bytes.size()) {
            return ENOBUFS;
        }
        return journal::write_snapshot_file(
            paths(), s.as_of, bytes, journal_options().writer.sync == journal::SyncPolicy::Sync);
    }

    // Removes the snapshots and segments the good snapshots make unnecessary. Returns how
    // many files went.
    std::uint32_t remove_old_files() {
        const JournalOptions& options = journal_options();
        const journal::DirectoryListing listing =
            journal::list_journal_directory(options.directory);
        std::uint32_t removed = 0;
        for (const std::string& path :
             journal::files_to_remove(listing, good_snapshots_, options.keep_snapshots)) {
            std::error_code ignored;
            if (std::filesystem::remove(path, ignored)) {
                ++removed;
            }
        }
        // Keep only the good snapshots that are still on disk.
        std::erase_if(good_snapshots_, [](const journal::SnapshotFile& file) {
            return !std::filesystem::exists(file.path);
        });
        return removed;
    }

    // At every start, after recovery and before any thread runs, the state is written as a
    // snapshot, old files are removed, and the snapshot thread is built from it.
    void start_snapshots(const journal::JournalWriter& writer, protocol::Timestamp now) {
        const JournalOptions& options = journal_options();
        if (options.keep_snapshots == 0) {
            return;
        }
        const journal::Snapshot start = snapshot_now(writer.next_sequence() - 1, now);
        const int error = write_snapshot(start);
        if (error != 0) {
            throw std::system_error(error, std::generic_category(),
                                    "write the start snapshot in " + options.directory);
        }
        recovery_.start_snapshot = start.as_of;
        good_snapshots_.push_back(
            journal::SnapshotFile{.as_of = start.as_of, .path = paths().snapshot(start.as_of)});
        recovery_.files_removed = remove_old_files();
        if (options.snapshot_thread && options.records_per_segment > 0) {
            std::vector<journal::Sequence> kept;
            kept.reserve(good_snapshots_.size());
            for (const journal::SnapshotFile& file : good_snapshots_) {
                kept.push_back(file.as_of);
            }
            snapshotter_.emplace(
                SnapshotWorkerOptions{.directory = options.directory,
                                      .sync = options.writer.sync == journal::SyncPolicy::Sync,
                                      .keep = options.keep_snapshots},
                start, writer.file_first_sequence(), kept);
        }
    }

    // After a clean stop the live state is written as one more snapshot, so the next start
    // has nothing to replay. Nothing is written if the journal failed, since the state may
    // then hold requests the journal does not.
    void write_stop_snapshot() noexcept {
        if (!journal_ || !journal_options_ || journal_options_->keep_snapshots == 0 ||
            journal_->failed() || !journal_->stopped()) {
            return;
        }
        try {
            if (snapshotter_) {
                for (const journal::Sequence as_of : snapshotter_->kept()) {
                    if (std::ranges::none_of(good_snapshots_, [as_of](const auto& file) {
                            return file.as_of == as_of;
                        })) {
                        good_snapshots_.push_back(
                            journal::SnapshotFile{.as_of = as_of, .path = paths().snapshot(as_of)});
                    }
                }
                std::ranges::sort(good_snapshots_, {}, &journal::SnapshotFile::as_of);
            }
            const journal::Sequence as_of = journal_worker().writer().next_sequence() - 1;
            const journal::Snapshot last = snapshot_now(as_of, clock_());
            stop_snapshot_error_ = write_snapshot(last);
            if (stop_snapshot_error_ != 0) {
                return;
            }
            stop_snapshot_ = last.as_of;
            good_snapshots_.push_back(
                journal::SnapshotFile{.as_of = last.as_of, .path = paths().snapshot(last.as_of)});
            remove_old_files();
        } catch (const std::system_error& e) {
            stop_snapshot_error_ = e.code().value();
        } catch (const std::exception&) {
            stop_snapshot_error_ = EIO;
        }
    }

    // Writes the Restart record and the cancels of the cancel on disconnect accounts, then
    // applies the cancels, the same order the journal thread uses. Then every account's
    // next sequence number and the feed's session are set.
    void finish_recovery(journal::JournalWriter& writer, protocol::Timestamp now,
                         RecoverySink& sink) {
        recovery_.recovered = true;
        recovery_.restart_sequence = writer.next_sequence();
        require_written(writer.append_restart(now), "while writing the Restart record");
        std::vector<OwnerId> to_cancel;
        for (std::size_t i = 1; i <= sessions_.account_count(); ++i) {
            const auto owner = static_cast<OwnerId>(i);
            if (sessions_.cancels_on_disconnect(owner)) {
                require_written(writer.append(make_cancel_all_request(owner, now)),
                                "while writing a cancel on restart");
                to_cancel.push_back(owner);
            }
        }
        require_written(writer.flush(), "while writing the Restart record");

        sink.set_now(now);
        for (const OwnerId owner : to_cancel) {
            worker_.order_entry().cancel_all(owner, now);
            ++recovery_.cancels_on_restart;
        }
        for (std::size_t i = 1; i <= sessions_.account_count(); ++i) {
            const auto owner = static_cast<OwnerId>(i);
            sessions_.restore_next_sequence(owner, 1 + sink.replies_of(owner));
        }
        recovery_.replies = sink.total();
        publisher_.begin_new_session(recovery_.restart_sequence);
    }

    // Appends this start's setup records from index `first` on.
    void append_setup(journal::JournalWriter& writer, protocol::Timestamp now, std::size_t first) {
        for (std::size_t i = first; i < setup_.size(); ++i) {
            require_written(journal::append_setup_record(writer, now, setup_[i]),
                            "while writing the setup records");
        }
    }

    static void require_written(journal::JournalStatus status, const char* when) {
        if (status != journal::JournalStatus::Ok) {
            std::ostringstream os;
            os << "the journal could not be written " << when << " (" << status << ")";
            throw std::runtime_error(os.str());
        }
    }

    void begin_journal(journal::JournalWriter writer) {
        const SegmentOptions segments{
            .records_per_segment = journal_options().records_per_segment,
            .paths = &paths(),
            .listener = snapshotter_ ? static_cast<SegmentListener*>(&*snapshotter_) : nullptr};
        journal_.emplace(std::move(writer), requests_, journaled_, pipeline_.wait,
                         bells_for(pipeline_.wait).requests,
                         blocking() ? &journaled_bell_ : nullptr,
                         static_cast<JournalFailureListener*>(this), segments);
    }

    void pin(PipelineThread thread, int cpu) noexcept {
        if (cpu >= 0) {
            pin_results_[static_cast<std::size_t>(thread)] = concurrency::pin_current_thread(cpu);
        }
    }

    static void join(std::thread& thread) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    void gateway_main() {
        try {
            pin(PipelineThread::Gateway, pipeline_.gateway_cpu);
            if (blocking()) {
                gateway_.run();  // Sleeps in epoll_wait for up to one tick at a time.
            } else {
                gateway_.run(0);
            }
            shut_down();
        } catch (...) {
            errors_[static_cast<std::size_t>(PipelineThread::Gateway)] = std::current_exception();
        }
        signal_finished();
    }

    // Runs on the gateway thread after its loop has ended.
    void shut_down() {
        gateway_.end_session();
        gateway_.close_all(clock_());
        // A cancel on disconnect that met a full queue waits in the session layer.
        // Draining replies makes room for it.
        while (sessions_.waiting_for_backend()) {
            pump_.drain(clock_());
            concurrency::cpu_relax();
        }
        while (!requests_.try_push(make_stop_request(clock_()))) {
            pump_.drain(clock_());
            concurrency::cpu_relax();
        }
        request_bell_.ring();
        while (!pump_.stopped()) {
            pump_.drain(clock_());
            concurrency::cpu_relax();
        }
    }

    void signal_finished() noexcept {
        const std::uint64_t one = 1;
        // Only a counter near its limit can make this fail, and then the eventfd is
        // readable anyway. GCC's fortify checks refuse a (void) cast, so it is checked.
        if (::write(finished_.get(), &one, sizeof(one)) < 0) {
            return;
        }
    }

    void matching_main() {
        try {
            pin(PipelineThread::Matching, pipeline_.matching_cpu);
            worker_.run();
        } catch (...) {
            errors_[static_cast<std::size_t>(PipelineThread::Matching)] = std::current_exception();
            // The other two threads wait for Stop and Stopped, so send them anyway.
            // There is nothing left to do if that fails too.
            try {
                MarketEvent stop;
                stop.kind = MarketEventKind::Stop;
                while (!events_.try_push(stop)) {
                    concurrency::cpu_relax();
                }
                while (!replies_.try_push(make_stopped_reply())) {
                    concurrency::cpu_relax();
                }
                event_bell_.ring();
                reply_bell_.ring();
            } catch (...) {  // NOLINT(bugprone-empty-catch) The first error is kept.
            }
        }
    }

    void journal_main() {
        try {
            pin(PipelineThread::Journal, pipeline_.journal_cpu);
            journal_worker().run();
            if (journal_worker().failed()) {
                errors_[static_cast<std::size_t>(PipelineThread::Journal)] =
                    std::make_exception_ptr(std::runtime_error(journal_failure_message()));
            }
        } catch (...) {
            errors_[static_cast<std::size_t>(PipelineThread::Journal)] = std::current_exception();
            gateway_.stop();
            // The matching thread still waits for Stop, so it is passed on by hand.
            try {
                pass_stop_after_error();
            } catch (...) {  // NOLINT(bugprone-empty-catch) The first error is kept.
            }
        }
    }

    [[nodiscard]] std::string journal_failure_message() const {
        const int error = journal_worker().writer().stats().last_error;
        std::ostringstream os;
        os << "the journal could not be written (" << journal_worker().failure();
        if (error != 0) {
            os << ", " << std::error_code(error, std::generic_category()).message();
        }
        os << "), so the exchange stopped";
        return os.str();
    }

    // Throws away requests until Stop and passes Stop on to the matching thread.
    void pass_stop_after_error() {
        for (;;) {
            const Request* request = requests_.front();
            if (request == nullptr) {
                concurrency::cpu_relax();
                continue;
            }
            const bool is_stop = request->kind == RequestKind::Stop;
            requests_.pop();
            if (is_stop) {
                while (!journaled_.try_push(make_stop_request(0))) {
                    concurrency::cpu_relax();
                }
                journaled_bell_.ring();
                return;
            }
        }
    }

    void snapshot_main() {
        try {
            pin(PipelineThread::Snapshot, pipeline_.snapshot_cpu);
            snapshot_worker().run();
        } catch (...) {
            errors_[static_cast<std::size_t>(PipelineThread::Snapshot)] = std::current_exception();
        }
    }

    void publisher_main() {
        try {
            pin(PipelineThread::Publisher, pipeline_.publisher_cpu);
            publisher_.run();
        } catch (...) {
            errors_[static_cast<std::size_t>(PipelineThread::Publisher)] = std::current_exception();
        }
    }

    // The members are built in this order, and each one only uses those above it. The
    // large cache-line aligned objects come first so less padding is needed between them.
    Doorbell request_bell_;
    Doorbell journaled_bell_;
    Doorbell reply_bell_;
    Doorbell event_bell_;
    Requests requests_;
    // From the journal thread to the matching thread. Without a journal it is a one slot
    // queue that nobody uses.
    Requests journaled_;
    Replies replies_;
    Events events_;
    Worker worker_;
    Publisher publisher_;
    Sessions sessions_;
    Pump pump_;
    Gateway gateway_;
    PipelineConfig pipeline_;
    protocol::Timestamp (*clock_)();
    gateway::Fd finished_;
    std::optional<JournalOptions> journal_options_;
    // This start's setup records, in the order of the startup calls. Only with a journal.
    std::vector<journal::JournalRecord> setup_;
    std::optional<journal::PathBuffer> paths_;
    // The snapshots known to be good, oldest first, used to decide what can be removed.
    std::vector<journal::SnapshotFile> good_snapshots_;
    std::optional<Snapshotter> snapshotter_;
    std::optional<Journal> journal_;
    RecoveryReport recovery_;
    journal::Sequence stop_snapshot_ = 0;
    int stop_snapshot_error_ = 0;
    std::thread publisher_thread_;
    std::thread matching_thread_;
    std::thread journal_thread_;
    std::thread snapshot_thread_;
    std::thread gateway_thread_;
    // Each slot is written only by its own thread and read after the join.
    std::array<int, kPipelineThreads> pin_results_{};
    std::array<std::exception_ptr, kPipelineThreads> errors_{};
    bool started_ = false;
    bool joined_ = false;
    bool setup_failed_ = false;
};

// The pipeline without a market data feed, as in Phase 7.
template <OrderBook Book, template <concurrency::QueueElement> class Queue>
using ThreadedGateway = BasicThreadedGateway<Book, Queue, PublisherStub>;

}  // namespace litbook::pipeline
