#pragma once

#include "litbook/book/book_config.hpp"
#include "litbook/book/order_book.hpp"
#include "litbook/concurrency/cpu_relax.hpp"
#include "litbook/concurrency/message_queue.hpp"
#include "litbook/concurrency/thread_affinity.hpp"
#include "litbook/engine/engine_listener.hpp"
#include "litbook/engine/ticker.hpp"
#include "litbook/gateway/epoll_gateway.hpp"
#include "litbook/gateway/session_manager.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/matching_worker.hpp"
#include "litbook/pipeline/messages.hpp"
#include "litbook/pipeline/publisher.hpp"
#include "litbook/pipeline/queue_backend.hpp"
#include "litbook/pipeline/reply_pump.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace litbook::pipeline {

struct PipelineConfig {
    WaitMode wait = WaitMode::Spin;
    // Queue sizes, each a power of two.
    std::size_t request_capacity = 1024;
    std::size_t reply_capacity = 4096;
    std::size_t event_capacity = 4096;
    // The most replies the gateway hands to the session layer per round.
    std::size_t max_replies_per_drain = 1024;
    // The CPU each thread pins itself to, or -1 to leave it to the scheduler.
    int gateway_cpu = -1;
    int matching_cpu = -1;
    int publisher_cpu = -1;
};

enum class PipelineThread : std::uint8_t { Gateway, Matching, Publisher };

// The exchange on three threads. The gateway thread runs the epoll loop and the session
// layer, the matching thread runs the order entry layer and every book, and the publisher
// thread consumes the market events. They are connected by three queues of type Queue
// (SpscQueue, or MutexQueue for the comparison), each with one producer and one consumer.
//
//   gateway thread pushes Request to the matching thread
//   matching thread pushes Reply to the gateway thread
//   matching thread pushes MarketEvent to the publisher thread
//
// Setup (add_account, add_symbol) happens before start(). start() launches the threads,
// and stop() shuts them down in order. The gateway sends End of Session, closes every
// connection (so cancel on disconnect runs), sends Stop down the request queue, and keeps
// draining replies until the matching thread answers Stopped. Stop travels as a message,
// so it is handled after everything that was queued before it. The statistics and the
// order entry layer may be read once stop() has returned.
//
// Invariants. Before start() and after stop() only the calling thread touches anything.
// In between each queue has exactly one producer thread and one consumer thread, and the
// gateway thread never waits for the matching thread.
template <OrderBook Book, template <concurrency::QueueElement> class Queue>
class ThreadedGateway {
public:
    using Requests = Queue<Request>;
    using Replies = Queue<Reply>;
    using Events = Queue<MarketEvent>;
    using Backend = QueueBackend<Book, Requests>;
    using Sessions = gateway::BasicSessionManager<Backend>;
    using Pump = ReplyPump<Sessions, Replies>;
    using Gateway = gateway::BasicEpollGateway<Sessions, Pump>;
    using Worker = MatchingWorker<Book, Requests, Replies, Events>;
    using Publisher = PublisherStub<Events>;

    // Opens the listening socket. Throws std::invalid_argument for a bad configuration
    // or queue capacity, and std::system_error if a socket or eventfd cannot be made.
    ThreadedGateway(const gateway::SessionConfig& session, const protocol::OrderEntryConfig& orders,
                    const gateway::GatewayConfig& network, const PipelineConfig& pipeline)
        : requests_(pipeline.request_capacity),
          replies_(pipeline.reply_capacity),
          events_(pipeline.event_capacity),
          worker_(orders, requests_, replies_, events_, pipeline.wait, bells_for(pipeline.wait)),
          publisher_(events_, pipeline.wait, bells_for(pipeline.wait).events),
          sessions_(session, std::in_place, worker_.order_entry(), requests_,
                    bells_for(pipeline.wait).requests),
          pump_(sessions_, replies_, bells_for(pipeline.wait).replies,
                pipeline.max_replies_per_drain),
          gateway_(sessions_, network, pump_),
          pipeline_(pipeline),
          clock_(network.clock) {}

    ThreadedGateway(const ThreadedGateway&) = delete;
    ThreadedGateway& operator=(const ThreadedGateway&) = delete;
    ThreadedGateway(ThreadedGateway&&) = delete;
    ThreadedGateway& operator=(ThreadedGateway&&) = delete;

    // Stops the threads if they still run. An error from a thread is lost here, so call
    // stop() first to see it.
    ~ThreadedGateway() {
        try {
            stop();
        } catch (...) {  // NOLINT(bugprone-empty-catch) A destructor must not throw.
        }
    }

    // Startup calls. Both throw std::logic_error after start(), because the threads then
    // own the state they change.
    OwnerId add_account(const gateway::AccountConfig& account) {
        require_not_started();
        return sessions_.add_account(account);
    }
    SymbolId add_symbol(Ticker ticker, const BookConfig& config) {
        require_not_started();
        return sessions_.add_symbol(ticker, config);
    }

    [[nodiscard]] std::uint16_t port() const { return gateway_.port(); }

    // Launches the publisher, matching and gateway threads. Throws std::logic_error if
    // called twice, and std::system_error if a thread cannot be created.
    void start() {
        require_not_started();
        started_ = true;
        publisher_thread_ = std::thread([this] { publisher_main(); });
        matching_thread_ = std::thread([this] { matching_main(); });
        gateway_thread_ = std::thread([this] { gateway_main(); });
    }

    // Shuts the pipeline down and joins the threads. Calling it again does nothing. If a
    // thread failed with an exception, the first one is rethrown here.
    void stop() {
        if (!started_ || joined_) {
            return;
        }
        gateway_.stop();
        join(gateway_thread_);
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
    // 0 if the thread was pinned or not asked to be, otherwise the error number.
    [[nodiscard]] int pin_result(PipelineThread thread) const noexcept {
        return pin_results_[static_cast<std::size_t>(thread)];
    }
    [[nodiscard]] const PipelineConfig& config() const noexcept { return pipeline_; }

private:
    [[nodiscard]] bool blocking() const noexcept { return pipeline_.wait == WaitMode::Block; }

    // The doorbells exist in both modes, but only a blocking pipeline uses them. This
    // takes the mode as an argument because it is called while pipeline_ is not built yet.
    [[nodiscard]] Doorbells bells_for(WaitMode mode) noexcept {
        if (mode != WaitMode::Block) {
            return Doorbells{};
        }
        return Doorbells{
            .requests = &request_bell_, .replies = &reply_bell_, .events = &event_bell_};
    }

    void require_not_started() const {
        if (started_) {
            throw std::logic_error("the pipeline has already started");
        }
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
    Doorbell reply_bell_;
    Doorbell event_bell_;
    Requests requests_;
    Replies replies_;
    Events events_;
    Worker worker_;
    Publisher publisher_;
    Sessions sessions_;
    Pump pump_;
    Gateway gateway_;
    PipelineConfig pipeline_;
    protocol::Timestamp (*clock_)();
    std::thread publisher_thread_;
    std::thread matching_thread_;
    std::thread gateway_thread_;
    // Each slot is written only by its own thread and read after the join.
    std::array<int, 3> pin_results_{};
    std::array<std::exception_ptr, 3> errors_{};
    bool started_ = false;
    bool joined_ = false;
};

}  // namespace litbook::pipeline
