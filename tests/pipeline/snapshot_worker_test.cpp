// Tests for the snapshot thread's worker. A journal directory is written by hand with
// segments, the worker is told when each segment closes, and every snapshot it writes
// must equal the state a full replay has after the same records.

#include "litbook/pipeline/snapshot_worker.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/journal/journal_directory.hpp"
#include "litbook/journal/journal_writer.hpp"
#include "litbook/journal/snapshot_store.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "journal/journal_builder.hpp"
#include "journal/replay_test_support.hpp"
#include "support/temp_dir.hpp"

namespace litbook::pipeline {
namespace {

using Book = SortedOrderBook;
using journal::Sequence;
using journal::Snapshot;

constexpr std::size_t kPerSegment = 400;

using journal::testing::encoded;
using journal::testing::JournalBuilder;

class SnapshotWorkerTest : public ::testing::Test {
protected:
    // Writes the startup snapshot right after the setup records, as the exchange does,
    // and builds the worker from it.
    void SetUp() override {
        const Snapshot start = builder_.snapshot();
        ASSERT_EQ(journal::write_snapshot_file(paths_, start.as_of, encoded(start), false), 0);
        worker_ = std::make_unique<SnapshotWorker<Book>>(
            SnapshotWorkerOptions{.directory = dir_.path(), .sync = false, .keep = 2}, start, 1,
            std::vector<Sequence>{start.as_of});
    }

    [[nodiscard]] journal::LoadedSnapshot read(Sequence as_of) {
        return journal::read_snapshot_file(
            journal::SnapshotFile{.as_of = as_of, .path = paths_.snapshot(as_of)});
    }

    litbook::testing::TempDir dir_;
    journal::PathBuffer paths_{dir_.path()};
    JournalBuilder builder_{dir_.path()};
    std::unique_ptr<SnapshotWorker<Book>> worker_;
};

TEST_F(SnapshotWorkerTest, NothingHappensUntilASegmentCloses) {
    builder_.append(50);
    EXPECT_FALSE(worker_->step());
    EXPECT_EQ(worker_->stats().snapshots, 0U);
}

TEST_F(SnapshotWorkerTest, EachClosedSegmentGivesASnapshotEqualToTheTrueState) {
    for (int round = 0; round < 3; ++round) {
        builder_.append(kPerSegment);
        const Snapshot truth = builder_.snapshot();
        const auto [first, next] = builder_.rotate();
        worker_->on_segment_closed(first, next);
        ASSERT_TRUE(worker_->step());
        EXPECT_FALSE(worker_->failed());
        EXPECT_EQ(worker_->as_of(), next - 1);
        const journal::LoadedSnapshot written = read(next - 1);
        ASSERT_EQ(written.status, journal::SnapshotStatus::Ok);
        EXPECT_EQ(written.snapshot, truth) << "round " << round;
    }
    EXPECT_EQ(worker_->stats().segments, 3U);
    EXPECT_EQ(worker_->stats().snapshots, 3U);
}

TEST_F(SnapshotWorkerTest, SeveralClosedSegmentsAreCaughtUpInOrder) {
    builder_.append(kPerSegment);
    builder_.rotate();
    builder_.append(kPerSegment);
    const Snapshot truth = builder_.snapshot();
    const auto [first, next] = builder_.rotate();
    worker_->on_segment_closed(first, next);  // Only the last report arrives.
    ASSERT_TRUE(worker_->step());
    EXPECT_EQ(worker_->stats().segments, 2U);
    EXPECT_EQ(read(next - 1).snapshot, truth);
}

TEST_F(SnapshotWorkerTest, OlderSnapshotsAndCoveredSegmentsAreRemoved) {
    const Sequence start_as_of = builder_.snapshot().as_of;
    std::vector<std::pair<Sequence, Sequence>> segments;
    for (int round = 0; round < 3; ++round) {
        builder_.append(kPerSegment);
        segments.push_back(builder_.rotate());
        worker_->on_segment_closed(segments.back().first, segments.back().second);
        ASSERT_TRUE(worker_->step());
    }
    // Snapshots as of the ends of segments 2 and 3 stay. The startup snapshot and the
    // snapshot after segment 1 went, and so did segments 1 and 2, which the snapshot after
    // segment 2 holds completely.
    EXPECT_FALSE(std::filesystem::exists(paths_.snapshot(start_as_of)));
    EXPECT_FALSE(std::filesystem::exists(paths_.snapshot(segments[0].second - 1)));
    EXPECT_TRUE(std::filesystem::exists(paths_.snapshot(segments[1].second - 1)));
    EXPECT_TRUE(std::filesystem::exists(paths_.snapshot(segments[2].second - 1)));
    EXPECT_FALSE(std::filesystem::exists(paths_.segment(segments[0].first)));
    EXPECT_FALSE(std::filesystem::exists(paths_.segment(segments[1].first)));
    EXPECT_TRUE(std::filesystem::exists(paths_.segment(segments[2].first)));
    EXPECT_TRUE(std::filesystem::exists(paths_.segment(segments[2].second)));
    EXPECT_EQ(worker_->stats().removed, 4U);
    EXPECT_EQ(worker_->kept().size(), 2U);
}

TEST_F(SnapshotWorkerTest, ADamagedSegmentStopsTheSnapshotsButNothingElse) {
    builder_.append(kPerSegment);
    const auto [first, next] = builder_.rotate();
    {
        std::fstream file(paths_.segment(first), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(200);
        file.put('\x7F');
    }
    worker_->on_segment_closed(first, next);
    EXPECT_TRUE(worker_->step());
    EXPECT_TRUE(worker_->failed());
    EXPECT_EQ(worker_->stats().failures, 1U);
    EXPECT_EQ(worker_->stats().last_error, EBADMSG);
    EXPECT_FALSE(std::filesystem::exists(paths_.snapshot(next - 1)));
    // Later segments are not looked at any more.
    builder_.append(kPerSegment);
    const auto later = builder_.rotate();
    worker_->on_segment_closed(later.first, later.second);
    EXPECT_FALSE(worker_->step());
}

TEST_F(SnapshotWorkerTest, AMissingSegmentFileIsAFailureWithItsErrno) {
    builder_.append(10);
    const auto [first, next] = builder_.rotate();
    std::filesystem::remove(paths_.segment(first));
    worker_->on_segment_closed(first, next);
    EXPECT_TRUE(worker_->step());
    EXPECT_TRUE(worker_->failed());
    EXPECT_EQ(worker_->stats().last_error, ENOENT);
}

TEST_F(SnapshotWorkerTest, OnItsOwnThreadItSleepsUntilASegmentClosesAndStopsWhenAsked) {
    std::thread thread([this] { worker_->run(); });
    builder_.append(kPerSegment);
    const Snapshot truth = builder_.snapshot();
    const auto [first, next] = builder_.rotate();
    worker_->on_segment_closed(first, next);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!std::filesystem::exists(paths_.snapshot(next - 1)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker_->request_stop();
    thread.join();
    ASSERT_TRUE(std::filesystem::exists(paths_.snapshot(next - 1)));
    EXPECT_EQ(read(next - 1).snapshot, truth);
}

TEST(SnapshotWorkerStartTest, AStartSnapshotThatDoesNotFitIsRefused) {
    const litbook::testing::TempDir dir;
    JournalBuilder builder(dir.path());
    builder.append(100);
    Snapshot start = builder.snapshot();
    ASSERT_FALSE(start.state.orders.empty());
    ++start.state.orders[0].open;
    EXPECT_THROW(SnapshotWorker<Book>(SnapshotWorkerOptions{.directory = dir.path()}, start, 1,
                                      {start.as_of}),
                 std::invalid_argument);
}

}  // namespace
}  // namespace litbook::pipeline
