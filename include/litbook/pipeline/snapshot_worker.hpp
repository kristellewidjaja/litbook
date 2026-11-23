#pragma once

#include "litbook/book/order_book.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_reader.hpp"
#include "litbook/journal/replay.hpp"
#include "litbook/journal/snapshot_format.hpp"
#include "litbook/journal/snapshot_store.hpp"
#include "litbook/pipeline/doorbell.hpp"
#include "litbook/pipeline/journal_worker.hpp"
#include "litbook/protocol/order_entry.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace litbook::pipeline {

struct SnapshotWorkerStats {
    std::uint64_t segments = 0;   // Closed segments replayed into the shadow copy.
    std::uint64_t records = 0;    // Records applied, setup and skipped ones included.
    std::uint64_t snapshots = 0;  // Snapshot files written.
    std::uint64_t removed = 0;    // Old segment and snapshot files removed.
    // Times listing or removing old files failed. They are tried again after the next
    // snapshot.
    std::uint64_t removal_failures = 0;
    std::uint64_t failures = 0;  // 1 once the worker gave up (see last_error).
    int last_error = 0;          // errno of the failure, or EBADMSG for a bad segment.
};

struct SnapshotWorkerOptions {
    std::string directory;  // The journal directory.
    bool sync = false;      // Sync each snapshot and the directory (the Sync policy).
    std::size_t keep = 2;   // Snapshots to keep. Older ones and covered segments go.
};

// The snapshot thread of the pipeline. It keeps a second copy of the exchange (the
// shadow), rebuilt from the snapshot the exchange wrote when it started. Each time the
// journal thread closes a segment, this thread reads that finished file, replays it into
// the shadow and writes a snapshot as of the segment's last record. Then older snapshots
// and the segments the oldest kept snapshot holds are removed. Replay is deterministic, so
// the shadow is exactly the state the matching thread had after the same records, and
// the matching thread never stops for a snapshot.
//
// Reading a segment, replaying it, encoding the snapshot and writing it use buffers
// reserved in the constructor and allocate nothing. Removing old files lists the
// directory, which allocates once per snapshot on this background thread.
//
// If anything goes wrong (a segment that is not a clean continuation, a snapshot that
// cannot be written), the worker counts one failure and takes no more snapshots. The
// exchange keeps running, since the journal itself is still complete. The next start
// recovers from the last good snapshot and the segments after it.
//
// Invariants. One thread calls step and run, any thread may call on_segment_closed and
// request_stop. The shadow has applied exactly the records up to as_of(), and every
// segment before next_segment() has been replayed.
template <OrderBook Book>
class SnapshotWorker final : public SegmentListener {
public:
    // `start` is the snapshot written at startup and `open_segment` the first sequence
    // number of the segment that was open then. `kept` lists the snapshots that remained
    // after startup (oldest first, `start` last). Throws std::invalid_argument if the
    // shadow cannot be built from `start`. A startup call that allocates.
    SnapshotWorker(SnapshotWorkerOptions options, const journal::Snapshot& start,
                   journal::Sequence open_segment, const std::vector<journal::Sequence>& kept)
        : options_(std::move(options)),
          paths_(options_.directory),
          replies_(start.replies.size()),
          shadow_(replies_),
          next_segment_(open_segment),
          closed_until_(open_segment) {
        if (shadow_.start_from(start) != journal::ReplayStatus::Ok) {
            throw std::invalid_argument("the snapshot thread could not start from the snapshot");
        }
        replies_.counts = start.replies;
        current_.setup = start.setup;
        current_.replies.assign(start.replies.size(), 0);
        shadow_.order_entry().reserve_state(current_.state);
        buffer_size_ = largest_snapshot(start);
        buffer_.resize(buffer_size_);
        kept_.reserve(options_.keep + 2);
        kept_ = kept;
    }

    SnapshotWorker(const SnapshotWorker&) = delete;
    SnapshotWorker& operator=(const SnapshotWorker&) = delete;
    SnapshotWorker(SnapshotWorker&&) = delete;
    SnapshotWorker& operator=(SnapshotWorker&&) = delete;
    ~SnapshotWorker() override = default;

    // Runs on the journal thread. It only stores a number and rings the doorbell.
    void on_segment_closed(journal::Sequence /*first*/, journal::Sequence next) noexcept override {
        closed_until_.store(next, std::memory_order_release);
        bell_.ring();
    }

    // Replays every closed segment not read yet and writes a snapshot after each one.
    // Returns true if it did any work.
    bool step() {
        bool worked = false;
        while (!failed_ && next_segment_ < closed_until_.load(std::memory_order_acquire)) {
            worked = true;
            if (!replay_segment() || !write_snapshot()) {
                break;
            }
            remove_old_files();
        }
        return worked;
    }

    // Takes snapshots until request_stop(), sleeping on the doorbell in between.
    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            if (step()) {
                continue;
            }
            if (!bell_.prepare_to_sleep()) {
                continue;
            }
            // A stop or a closed segment that came in before prepare_to_sleep has already
            // rung, so prepare_to_sleep returned false above. Anything later rings again.
            bell_.sleep(-1);
        }
    }

    void request_stop() noexcept {
        stop_.store(true, std::memory_order_release);
        bell_.ring();
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] journal::Sequence as_of() const noexcept { return current_as_of_; }
    [[nodiscard]] journal::Sequence next_segment() const noexcept { return next_segment_; }
    [[nodiscard]] const SnapshotWorkerStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::vector<journal::Sequence>& kept() const noexcept { return kept_; }
    // The shadow copy. Only read it after the thread has stopped.
    [[nodiscard]] journal::Replayer<Book>& shadow() noexcept { return shadow_; }

private:
    // Counts the replies the shadow produces, so a snapshot knows each account's next
    // sequence number. Sized at startup.
    class ReplyCounter final : public protocol::OutboundSink {
    public:
        explicit ReplyCounter(std::size_t accounts_plus_one) : counts(accounts_plus_one, 0) {}
        void send(OwnerId owner, std::span<const std::byte> /*frame*/) override {
            if (owner < counts.size()) {
                ++counts[owner];
            }
        }
        std::vector<std::uint64_t> counts;
    };

    // The most bytes a snapshot of this exchange can take, from the layout in
    // snapshot_format.hpp. Every live order is in both lists, so each costs both lengths.
    std::size_t largest_snapshot(const journal::Snapshot& start) {
        const protocol::OrderEntryConfig& config = shadow_.order_entry().config();
        std::size_t size = journal::kSnapshotHeaderLength + 4;
        for (const journal::JournalRecord& record : start.setup) {
            size += 6 + record.payload_length;
        }
        size += static_cast<std::size_t>(config.max_owners) * (8 + 4);
        size += 8 + 8 + 5 * 8;
        size += 4 + shadow_.order_entry().engine().symbol_count() * (8 + 4);
        size += 4 + static_cast<std::size_t>(config.max_live_orders) *
                        (journal::kSnapshotOrderLength + journal::kSnapshotRestingLength);
        size += 4 + 4;
        return size;
    }

    void fail(int error) noexcept {
        failed_ = true;
        stats_.failures = 1;
        stats_.last_error = error;
    }

    // Reads the segment that starts at next_segment_, which the journal thread has closed.
    bool replay_segment() {
        try {
            const journal::MappedFile file(paths_.segment(next_segment_));
            const journal::JournalScan scan = journal::scan_journal(file.bytes());
            if (scan.tail != journal::TailStatus::Clean || scan.first_sequence != next_segment_) {
                fail(EBADMSG);
                return false;
            }
            journal::RecordReader reader(file.bytes().subspan(
                journal::kFileHeaderLength, scan.valid_length - journal::kFileHeaderLength));
            journal::JournalRecord record;
            while (reader.next(record)) {
                ++stats_.records;
                if (shadow_.apply(record) != journal::ReplayStatus::Ok) {
                    fail(EBADMSG);
                    return false;
                }
            }
            next_segment_ = scan.next_sequence;
            ++stats_.segments;
            return true;
        } catch (const std::system_error& e) {
            fail(e.code().value());
            return false;
        }
    }

    bool write_snapshot() {
        current_.as_of = next_segment_ - 1;
        current_.as_of_time = shadow_.now();
        current_.replies = replies_.counts;  // Same size, so no allocation.
        shadow_.order_entry().capture(current_.state);
        const std::size_t length =
            journal::encode_snapshot(current_, std::span<std::byte>(buffer_));
        if (length == 0) {
            fail(ENOBUFS);
            return false;
        }
        const int error = journal::write_snapshot_file(
            paths_, current_.as_of, std::span<const std::byte>(buffer_.data(), length),
            options_.sync);
        if (error != 0) {
            fail(error);
            return false;
        }
        current_as_of_ = current_.as_of;
        ++stats_.snapshots;
        kept_.push_back(current_.as_of);
        return true;
    }

    // Removes what files_to_remove allows, given the snapshots this exchange wrote or
    // checked. A file that cannot be removed is left for the next start.
    void remove_old_files() {
        if (options_.keep == 0) {
            return;
        }
        try {
            const journal::DirectoryListing listing =
                journal::list_journal_directory(options_.directory);
            std::vector<journal::SnapshotFile> good;
            for (const journal::SnapshotFile& file : listing.snapshots) {
                if (std::ranges::find(kept_, file.as_of) != kept_.end()) {
                    good.push_back(file);
                }
            }
            for (const std::string& path : journal::files_to_remove(listing, good, options_.keep)) {
                std::error_code ignored;
                if (std::filesystem::remove(path, ignored)) {
                    ++stats_.removed;
                }
            }
            while (kept_.size() > options_.keep) {
                kept_.erase(kept_.begin());
            }
        } catch (const std::filesystem::filesystem_error&) {
            // The old files stay until a later snapshot removes them, and snapshots are
            // still written.
            ++stats_.removal_failures;
        }
    }

    SnapshotWorkerOptions options_;
    journal::PathBuffer paths_;
    ReplyCounter replies_;
    journal::Replayer<Book> shadow_;
    journal::Snapshot current_;
    // Sized once at startup for the largest snapshot this exchange can have.
    std::vector<std::byte> buffer_;
    std::size_t buffer_size_ = 0;
    std::vector<journal::Sequence> kept_;  // As of numbers of the good snapshots on disk.
    journal::Sequence next_segment_;
    journal::Sequence current_as_of_ = 0;
    std::atomic<journal::Sequence> closed_until_;
    std::atomic<bool> stop_{false};
    Doorbell bell_;
    SnapshotWorkerStats stats_;
    bool failed_ = false;
};

}  // namespace litbook::pipeline
