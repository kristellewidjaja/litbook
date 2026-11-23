#pragma once

#include "litbook/concurrency/cpu_relax.hpp"
#include "litbook/concurrency/message_queue.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_format.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/messages.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace litbook::pipeline {

// Hears once, on the journal thread, that the journal can no longer be written. The
// threaded gateway uses it to shut the exchange down.
class JournalFailureListener {
public:
    virtual ~JournalFailureListener() = default;
    virtual void on_journal_failure() noexcept = 0;

protected:
    JournalFailureListener() = default;
    JournalFailureListener(const JournalFailureListener&) = default;
    JournalFailureListener(JournalFailureListener&&) = default;
    JournalFailureListener& operator=(const JournalFailureListener&) = default;
    JournalFailureListener& operator=(JournalFailureListener&&) = default;
};

// Hears, on the journal thread, that a journal segment was closed. `first` is the first
// sequence number of the closed segment and `next` the first of the new one, so the
// closed segment holds the records first to next - 1. The snapshot thread uses it to
// learn that there is a finished file to read. It must not block.
class SegmentListener {
public:
    virtual ~SegmentListener() = default;
    virtual void on_segment_closed(journal::Sequence first, journal::Sequence next) noexcept = 0;

protected:
    SegmentListener() = default;
    SegmentListener(const SegmentListener&) = default;
    SegmentListener(SegmentListener&&) = default;
    SegmentListener& operator=(const SegmentListener&) = default;
    SegmentListener& operator=(SegmentListener&&) = default;
};

// How the journal thread splits the journal into segments. With records_per_segment 0
// (the default) it never rotates and the journal stays one file.
struct SegmentOptions {
    std::uint64_t records_per_segment = 0;
    journal::PathBuffer* paths = nullptr;  // Names the new segments. Needed to rotate.
    SegmentListener* listener = nullptr;   // May be nullptr.
};

// The most requests the journal thread takes for one commit. A batch is copied into an
// array that lives inside the worker, so this also bounds the worker's size.
inline constexpr std::size_t kMaxJournalBatch = 64;

struct JournalWorkerStats {
    std::uint64_t requests = 0;       // Requests popped, Stop included.
    std::uint64_t batches = 0;        // Batches the worker tried to commit.
    std::uint64_t largest_batch = 0;  // The most requests in one batch.
    std::uint64_t forwarded = 0;      // Requests pushed on to the matching thread.
    std::uint64_t discarded = 0;      // Requests thrown away because the journal failed.
    // How often a push found the matching thread's queue full and had to wait.
    std::uint64_t forward_waits = 0;
};

// The journal thread of the pipeline. It sits between the gateway thread and the matching
// thread. It takes every request that is waiting (up to a batch limit), appends them all
// to the journal, commits them with one write (group commit), and only then pushes them
// on to the matching thread. So the matching thread never sees a request that is not in
// the journal yet, and no reply or market data can leave the exchange before its cause
// was written. That is what makes the journal a write-ahead log.
//
// Stop is journaled and flushed like any request and then passed on, and the worker
// stops after it.
//
// If an append, a write or a sync fails, the writer stops for good (D-060), and so the
// batch that failed is never passed on, because it might not be on disk. The worker
// tells its listener once, which shuts the exchange down. From then on it keeps popping
// requests and throws them away, so the gateway is never stuck on a full queue, until
// Stop arrives. Stop is still passed on, so the matching thread answers Stopped and the
// pipeline shuts down normally.
//
// With segments, the worker rotates to a new segment file once the current one holds at
// least records_per_segment records. That happens after a batch was passed on, so it
// never delays the batch, and only between batches, so a batch is never split across two
// files. A failed rotation stops the journal like a failed write.
//
// step() handles one batch and can be called by hand, which is how the single-threaded
// tests drive it. run() loops until it has handled Stop.
//
// Invariants. Exactly one thread calls step or run. It is the only consumer of the input
// queue and the only producer of the output queue. Every request in the output queue was
// committed to the journal before it was pushed, and requests leave in the order they
// arrived. Nothing allocates after the constructor.
template <concurrency::MessageQueue InQueue, concurrency::MessageQueue OutQueue>
    requires std::same_as<typename InQueue::value_type, Request> &&
             std::same_as<typename OutQueue::value_type, Request>
class JournalWorker {
public:
    // `in_bell` wakes this thread in WaitMode::Block and `out_bell` wakes the matching
    // thread. Both are nullptr when the threads spin. `listener` may be nullptr.
    // Throws std::invalid_argument if segments are asked for without a PathBuffer.
    JournalWorker(journal::JournalWriter writer, InQueue& in, OutQueue& out, WaitMode mode,
                  Doorbell* in_bell, Doorbell* out_bell, JournalFailureListener* listener,
                  const SegmentOptions& segments = {})
        : writer_(std::move(writer)),
          in_(&in),
          out_(&out),
          mode_(mode),
          in_bell_(in_bell),
          out_bell_(out_bell),
          listener_(listener),
          segments_(checked(segments)),
          batch_limit_(batch_limit_for(writer_.capacity())) {}

    JournalWorker(const JournalWorker&) = delete;
    JournalWorker& operator=(const JournalWorker&) = delete;
    JournalWorker(JournalWorker&&) = delete;
    JournalWorker& operator=(JournalWorker&&) = delete;
    ~JournalWorker() = default;

    // Takes one batch of waiting requests, journals it and passes it on. Returns false if
    // the queue was empty or the worker has already stopped.
    bool step() {
        if (stopped_) {
            return false;
        }
        if (failed_) {
            return discard_waiting();
        }
        const std::size_t count = take_batch();
        if (count == 0) {
            return false;
        }
        const bool ends_with_stop = batch_[count - 1].kind == RequestKind::Stop;
        const journal::JournalStatus status = journal_batch(count, ends_with_stop);
        if (status != journal::JournalStatus::Ok) {
            fail(status);
            // Only the Stop of this batch goes on. Everything before it may be lost.
            if (ends_with_stop) {
                stats_.discarded += count - 1;
                forward(batch_[count - 1]);
                stopped_ = true;
            } else {
                stats_.discarded += count;
            }
            ring(out_bell_);
            return true;
        }
        for (std::size_t i = 0; i < count; ++i) {
            forward(batch_[i]);
        }
        ring(out_bell_);
        stopped_ = ends_with_stop;
        if (!stopped_) {
            rotate_if_full();
        }
        return true;
    }

    // Handles batches until Stop, spinning or sleeping on the input doorbell when the queue
    // is empty.
    void run() {
        while (!stopped_) {
            if (step()) {
                continue;
            }
            wait_for_requests();
        }
    }

    [[nodiscard]] bool stopped() const noexcept { return stopped_; }
    // True once the journal could not be written. failure() says how it failed.
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] journal::JournalStatus failure() const noexcept { return failure_; }
    [[nodiscard]] std::size_t batch_limit() const noexcept { return batch_limit_; }
    [[nodiscard]] const JournalWorkerStats& stats() const noexcept { return stats_; }
    // The writer. Only read it after the thread has stopped.
    [[nodiscard]] const journal::JournalWriter& writer() const noexcept { return writer_; }

private:
    static SegmentOptions checked(SegmentOptions segments) {
        if (segments.records_per_segment > 0 && segments.paths == nullptr) {
            throw std::invalid_argument("journal segments need a PathBuffer to name them");
        }
        return segments;
    }

    // Starts a new segment once the current one is full, and tells the listener.
    void rotate_if_full() {
        if (segments_.records_per_segment == 0) {
            return;
        }
        const journal::Sequence first = writer_.file_first_sequence();
        const journal::Sequence next = writer_.next_sequence();
        if (next - first < segments_.records_per_segment) {
            return;
        }
        const journal::JournalStatus status =
            writer_.rotate(segments_.paths->segment(next), segments_.paths->directory());
        if (status != journal::JournalStatus::Ok) {
            fail(status);
            return;
        }
        if (segments_.listener != nullptr) {
            segments_.listener->on_segment_closed(first, next);
        }
    }

    // A batch must always fit in the writer's buffer, so an append never finds it full.
    static std::size_t batch_limit_for(std::size_t capacity) noexcept {
        return std::clamp<std::size_t>(capacity / journal::kMaxRecordLength, 1, kMaxJournalBatch);
    }

    // Copies up to batch_limit_ requests out of the input queue, stopping after a Stop.
    // They are popped right away, which gives the gateway room again sooner.
    std::size_t take_batch() {
        std::size_t count = 0;
        while (count < batch_limit_) {
            const Request* request = in_->front();
            if (request == nullptr) {
                break;
            }
            batch_[count] = *request;
            in_->pop();
            ++stats_.requests;
            ++count;
            if (batch_[count - 1].kind == RequestKind::Stop) {
                break;
            }
        }
        return count;
    }

    // Appends every request of the batch and writes them with one commit. A batch that
    // ends with Stop is flushed, so even the Buffered policy writes it out.
    journal::JournalStatus journal_batch(std::size_t count, bool ends_with_stop) {
        ++stats_.batches;
        stats_.largest_batch = std::max<std::uint64_t>(stats_.largest_batch, count);
        for (std::size_t i = 0; i < count; ++i) {
            const journal::JournalStatus status = writer_.append(batch_[i]);
            if (status != journal::JournalStatus::Ok) {
                return status;
            }
        }
        if (ends_with_stop) {
            return writer_.flush();
        }
        return writer_.commit();
    }

    // After a failure, pops whatever waits and throws it away, except Stop.
    bool discard_waiting() {
        bool popped = false;
        while (!stopped_) {
            const Request* request = in_->front();
            if (request == nullptr) {
                break;
            }
            const Request copy = *request;
            in_->pop();
            ++stats_.requests;
            popped = true;
            if (copy.kind == RequestKind::Stop) {
                forward(copy);
                ring(out_bell_);
                stopped_ = true;
            } else {
                ++stats_.discarded;
            }
        }
        return popped;
    }

    void fail(journal::JournalStatus status) noexcept {
        failed_ = true;
        failure_ = status;
        if (listener_ != nullptr) {
            listener_->on_journal_failure();
        }
    }

    // A full output queue is never a reason to drop a request. The matching thread never
    // waits for this thread, so it keeps draining and this cannot deadlock.
    void forward(const Request& request) {
        ++stats_.forwarded;
        if (out_->try_push(request)) {
            return;
        }
        ++stats_.forward_waits;
        while (!out_->try_push(request)) {
            // The matching thread may be asleep with a full queue in front of it.
            ring(out_bell_);
            concurrency::cpu_relax();
        }
    }

    void wait_for_requests() {
        if (mode_ == WaitMode::Spin || in_bell_ == nullptr) {
            concurrency::cpu_relax();
            return;
        }
        if (in_bell_->prepare_to_sleep()) {
            in_bell_->sleep(-1);
        }
    }

    static void ring(Doorbell* bell) noexcept {
        if (bell != nullptr) {
            bell->ring();
        }
    }

    journal::JournalWriter writer_;
    InQueue* in_;
    OutQueue* out_;
    WaitMode mode_;
    Doorbell* in_bell_;
    Doorbell* out_bell_;
    JournalFailureListener* listener_;
    SegmentOptions segments_;
    std::size_t batch_limit_;
    std::array<Request, kMaxJournalBatch> batch_{};
    JournalWorkerStats stats_;
    journal::JournalStatus failure_ = journal::JournalStatus::Ok;
    bool failed_ = false;
    bool stopped_ = false;
};

}  // namespace litbook::pipeline
