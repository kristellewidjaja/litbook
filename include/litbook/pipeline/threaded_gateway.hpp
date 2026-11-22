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
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/setup.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/journal_worker.hpp"
#include "litbook/pipeline/matching_worker.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/pipeline/publisher.hpp"
#include "litbook/pipeline/queue_backend.hpp"
#include "litbook/pipeline/reply_pump.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <atomic>
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
};

enum class PipelineThread : std::uint8_t { Gateway, Matching, Publisher, Journal };

// Where the journal lives and how it is written. With a journal the pipeline gets a
// fourth thread, the journal thread, between the gateway and the matching thread.
struct JournalOptions {
    std::string path;
    journal::JournalWriterConfig writer{};
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
        joined_ = true;
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

    // Runs on the journal thread when the journal can no longer be written. Stopping the
    // gateway is only an atomic store and an eventfd write, so any thread may do it.
    void on_journal_failure() noexcept override { gateway_.stop(); }

    // Recovery and opening the journal. All of this runs on the thread that calls start(),
    // before any other thread exists.

    void open_journal() {
        const JournalOptions& options = journal_options();
        const protocol::Timestamp now = clock_();
        if (!std::filesystem::exists(options.path)) {
            journal::JournalWriter writer =
                journal::JournalWriter::create(options.path, options.writer);
            append_setup(writer, now, 0);
            require_written(writer.flush(), "while writing the setup records");
            begin_journal(std::move(writer));
            return;
        }

        RecoverySink sink(worker_.order_entry().config().max_owners, publisher_);
        journal::JournalScan scan;
        std::size_t setup_found = 0;
        {
            // The file stays mapped only while it is replayed. It must be unmapped before
            // the writer cuts a torn tail off.
            const journal::MappedFile file(options.path);
            scan = journal::scan_journal(file.bytes());
            refuse_unusable(scan);
            recovery_.records = scan.records;
            recovery_.torn_bytes = scan.file_length - scan.valid_length;
            setup_found = replay_into_worker(file.bytes(), scan, sink);
        }

        journal::JournalWriter writer =
            journal::JournalWriter::resume(options.path, scan, options.writer);
        // A journal that ends inside its setup records (a crash during the very first
        // start) gets the rest of them now. replay_into_worker checked that the ones it
        // has agree with this start.
        recovery_.setup_added = setup_.size() - setup_found;
        append_setup(writer, now, setup_found);
        if (scan.records > 0) {
            finish_recovery(writer, now, sink);
        } else {
            require_written(writer.flush(), "while writing the setup records");
        }
        worker_.end_redirect();
        begin_journal(std::move(writer));
    }

    void refuse_unusable(const journal::JournalScan& scan) const {
        const std::string& path = journal_options().path;
        if (scan.tail == journal::TailStatus::BadHeader) {
            std::ostringstream os;
            os << "the journal " << path << " has a bad file header (" << scan.header
               << "), so the exchange will not start from it";
            throw std::runtime_error(os.str());
        }
        if (scan.tail == journal::TailStatus::Corrupt) {
            std::ostringstream os;
            os << "the journal " << path << " is corrupt. Its good records stop at byte "
               << scan.valid_length << ", but another good record starts at byte "
               << scan.corrupt_at
               << ". Cutting it there would lose good records, so the exchange will not "
                  "start from it";
            throw std::runtime_error(os.str());
        }
    }

    // Replays every good record of the journal into the matching thread's order entry
    // layer, with its output going to `sink`. The setup records at the start are compared
    // with this start's. Returns how many setup records the journal has.
    std::size_t replay_into_worker(std::span<const std::byte> file,
                                   const journal::JournalScan& scan, RecoverySink& sink) {
        std::vector<journal::JournalRecord> found;
        bool requests_seen = false;
        worker_.redirect_output(sink, sink);
        const std::span<const std::byte> records = file.subspan(
            journal::kFileHeaderLength, scan.valid_length - journal::kFileHeaderLength);
        journal::RecordReader reader(records);
        journal::JournalRecord record;
        while (reader.next(record)) {
            if (journal::is_setup_record(record.type)) {
                if (requests_seen) {
                    throw std::runtime_error("the journal " + journal_options().path +
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
            throw std::runtime_error("the journal " + journal_options().path +
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
                return;  // replay_into_worker handles these.
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
        journal_.emplace(std::move(writer), requests_, journaled_, pipeline_.wait,
                         bells_for(pipeline_.wait).requests,
                         blocking() ? &journaled_bell_ : nullptr,
                         static_cast<JournalFailureListener*>(this));
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
    std::optional<Journal> journal_;
    RecoveryReport recovery_;
    std::thread publisher_thread_;
    std::thread matching_thread_;
    std::thread journal_thread_;
    std::thread gateway_thread_;
    // Each slot is written only by its own thread and read after the join.
    std::array<int, 4> pin_results_{};
    std::array<std::exception_ptr, 4> errors_{};
    bool started_ = false;
    bool joined_ = false;
    bool setup_failed_ = false;
};

// The pipeline without a market data feed, as in Phase 7.
template <OrderBook Book, template <concurrency::QueueElement> class Queue>
using ThreadedGateway = BasicThreadedGateway<Book, Queue, PublisherStub>;

}  // namespace litbook::pipeline
