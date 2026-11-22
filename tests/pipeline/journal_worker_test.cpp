#include "litbook/pipeline/journal_worker.hpp"

#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/journal/journal_reader.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "pipeline/pipeline_test_support.hpp"
#include "support/temp_dir.hpp"

// The journal thread's worker, driven by hand on one thread. A fake write() looks at the
// output queue each time it is called, which shows that no request reaches the matching
// thread before the write that holds it.

namespace litbook::pipeline {
namespace {

using Queue = concurrency::SpscQueue<Request>;
using Worker = JournalWorker<Queue, Queue>;
using testing::limit;
using testing::request_of;

// What the fake system calls see and do. Plain function pointers cannot capture
// anything, so they read this one object, which every test resets.
struct Fakes {
    const Queue* out = nullptr;
    std::vector<std::size_t> out_size_at_write;  // The output queue's size at each write.
    std::size_t writes = 0;
    std::size_t fail_write_number = 0;  // The write that fails with ENOSPC, 0 for none.
    bool fail_sync = false;
    std::size_t syncs = 0;
};

Fakes fakes;

ssize_t fake_write(int fd, const void* data, std::size_t length) {
    ++fakes.writes;
    if (fakes.out != nullptr) {
        fakes.out_size_at_write.push_back(fakes.out->size_approx());
    }
    if (fakes.writes == fakes.fail_write_number) {
        errno = ENOSPC;
        return -1;
    }
    return ::write(fd, data, length);
}

int fake_sync(int fd) {
    ++fakes.syncs;
    if (fakes.fail_sync) {
        errno = EIO;
        return -1;
    }
    return ::fdatasync(fd);
}

class Listener final : public JournalFailureListener {
public:
    void on_journal_failure() noexcept override { ++calls; }
    int calls = 0;
};

class JournalWorkerTest : public ::testing::Test {
protected:
    void SetUp() override { fakes = Fakes{}; }

    // Builds the worker on a new journal. The header write happens here, before the fake
    // starts watching the output queue.
    void make(journal::SyncPolicy policy = journal::SyncPolicy::Write,
              std::size_t buffer = std::size_t{1} << 16U) {
        journal::JournalWriterConfig config;
        config.buffer_capacity = buffer;
        config.sync = policy;
        config.ops = journal::FileOps{.write = &fake_write, .sync = &fake_sync};
        worker_ = std::make_unique<Worker>(journal::JournalWriter::create(path_, config), in_, out_,
                                           WaitMode::Spin, nullptr, nullptr, &listener_);
        fakes.out = &out_;
        fakes.writes = 0;
        fakes.syncs = 0;
    }

    void push(const Request& request) { ASSERT_TRUE(in_.try_push(request)); }

    void push_orders(std::size_t count) {
        for (std::size_t i = 1; i <= count; ++i) {
            const auto ref = static_cast<protocol::UserRefNum>(i);
            push(request_of(1, limit(Side::Buy, ref, 10, 100), 1000 + i));
        }
    }

    std::vector<Request> drain_out() {
        std::vector<Request> out;
        Request r;
        while (out_.try_pop(r)) {
            out.push_back(r);
        }
        return out;
    }

    // The good records in the journal file, read back by the scanner.
    journal::JournalScan scan() const {
        const journal::MappedFile file(path_);
        return journal::scan_journal(file.bytes());
    }

    litbook::testing::TempDir dir_;
    std::string path_ = dir_.file("journal");
    Queue in_{1024};
    Queue out_{1024};
    Listener listener_;
    std::unique_ptr<Worker> worker_;
};

TEST_F(JournalWorkerTest, AnEmptyQueueIsNoWork) {
    make();
    EXPECT_FALSE(worker_->step());
    EXPECT_EQ(fakes.writes, 0U);
    EXPECT_EQ(worker_->stats().batches, 0U);
}

TEST_F(JournalWorkerTest, EveryWaitingRequestGoesInOneCommitAndThenOnInOrder) {
    make();
    push_orders(5);
    ASSERT_TRUE(worker_->step());
    EXPECT_EQ(fakes.writes, 1U);  // Group commit.
    ASSERT_EQ(fakes.out_size_at_write.size(), 1U);
    EXPECT_EQ(fakes.out_size_at_write[0], 0U);  // Nothing went on before the write.
    const std::vector<Request> out = drain_out();
    ASSERT_EQ(out.size(), 5U);
    for (std::size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i].now, 1001 + i);
    }
    EXPECT_EQ(worker_->stats().batches, 1U);
    EXPECT_EQ(worker_->stats().largest_batch, 5U);
    EXPECT_EQ(worker_->stats().forwarded, 5U);
    EXPECT_EQ(worker_->writer().next_sequence(), 6U);
    EXPECT_EQ(scan().records, 5U);
}

TEST_F(JournalWorkerTest, ABatchStopsAtItsLimit) {
    make();
    push_orders(kMaxJournalBatch + 10);
    ASSERT_TRUE(worker_->step());
    EXPECT_EQ(drain_out().size(), kMaxJournalBatch);
    ASSERT_TRUE(worker_->step());
    EXPECT_EQ(drain_out().size(), 10U);
    EXPECT_EQ(fakes.writes, 2U);
    EXPECT_EQ(fakes.out_size_at_write, (std::vector<std::size_t>{0, 0}));
}

TEST_F(JournalWorkerTest, ASmallBufferMakesSmallerBatchesSoAnAppendNeverFindsItFull) {
    make(journal::SyncPolicy::Write, 3 * journal::kMaxRecordLength);
    EXPECT_EQ(worker_->batch_limit(), 3U);
    push_orders(7);
    while (worker_->step()) {
    }
    EXPECT_EQ(drain_out().size(), 7U);
    EXPECT_FALSE(worker_->failed());
    EXPECT_EQ(worker_->stats().batches, 3U);
    EXPECT_EQ(scan().records, 7U);
}

TEST_F(JournalWorkerTest, SyncPolicySyncsOncePerBatch) {
    make(journal::SyncPolicy::Sync);
    push_orders(4);
    ASSERT_TRUE(worker_->step());
    EXPECT_EQ(fakes.syncs, 1U);
    EXPECT_EQ(drain_out().size(), 4U);
}

TEST_F(JournalWorkerTest, StopIsJournaledFlushedAndPassedOnLast) {
    // Buffered would keep a small batch in memory, but a batch with Stop is flushed.
    make(journal::SyncPolicy::Buffered);
    push_orders(2);
    push(make_stop_request(5000));
    push_orders(1);  // Never taken, because Stop is the last request.
    ASSERT_TRUE(worker_->step());
    EXPECT_TRUE(worker_->stopped());
    EXPECT_EQ(fakes.writes, 1U);
    const std::vector<Request> out = drain_out();
    ASSERT_EQ(out.size(), 3U);
    EXPECT_EQ(out.back().kind, RequestKind::Stop);
    EXPECT_FALSE(worker_->step());
    EXPECT_EQ(in_.size_approx(), 1U);
    const journal::JournalScan s = scan();
    EXPECT_EQ(s.records, 3U);
    EXPECT_EQ(s.tail, journal::TailStatus::Clean);
}

TEST_F(JournalWorkerTest, AFailedWriteForwardsNothingAndTellsTheListenerOnce) {
    make();
    fakes.fail_write_number = 2;
    push_orders(3);
    ASSERT_TRUE(worker_->step());  // Write 1 works.
    EXPECT_EQ(drain_out().size(), 3U);
    push_orders(4);
    ASSERT_TRUE(worker_->step());  // Write 2 fails.
    EXPECT_TRUE(worker_->failed());
    EXPECT_EQ(worker_->failure(), journal::JournalStatus::WriteFailed);
    EXPECT_EQ(worker_->writer().stats().last_error, ENOSPC);
    EXPECT_TRUE(drain_out().empty());
    EXPECT_EQ(listener_.calls, 1);
    EXPECT_EQ(worker_->stats().discarded, 4U);
    EXPECT_FALSE(worker_->stopped());

    // Later requests are thrown away without another write, and Stop still goes on.
    push_orders(2);
    push(make_stop_request(9000));
    ASSERT_TRUE(worker_->step());
    EXPECT_TRUE(worker_->stopped());
    EXPECT_EQ(fakes.writes, 2U);
    EXPECT_EQ(listener_.calls, 1);
    EXPECT_EQ(worker_->stats().discarded, 6U);
    const std::vector<Request> out = drain_out();
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].kind, RequestKind::Stop);
    EXPECT_EQ(scan().records, 3U);
}

TEST_F(JournalWorkerTest, AFailedSyncStopsForwardingToo) {
    make(journal::SyncPolicy::Sync);
    fakes.fail_sync = true;
    push_orders(2);
    ASSERT_TRUE(worker_->step());
    EXPECT_TRUE(worker_->failed());
    EXPECT_EQ(worker_->failure(), journal::JournalStatus::SyncFailed);
    EXPECT_TRUE(drain_out().empty());
    EXPECT_EQ(listener_.calls, 1);
}

TEST_F(JournalWorkerTest, AFailureInTheBatchThatHoldsStopStillPassesStopOn) {
    make();
    fakes.fail_write_number = 1;
    push_orders(2);
    push(make_stop_request(7000));
    ASSERT_TRUE(worker_->step());
    EXPECT_TRUE(worker_->failed());
    EXPECT_TRUE(worker_->stopped());
    EXPECT_EQ(worker_->stats().discarded, 2U);
    const std::vector<Request> out = drain_out();
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].kind, RequestKind::Stop);
}

TEST_F(JournalWorkerTest, AFullOutputQueueIsWaitedForAndNothingIsDropped) {
    Queue small_out{2};
    journal::JournalWriterConfig config;
    config.ops = journal::FileOps{.write = &fake_write, .sync = &fake_sync};
    Worker worker(journal::JournalWriter::create(path_, config), in_, small_out, WaitMode::Spin,
                  nullptr, nullptr, nullptr);
    push_orders(20);
    push(make_stop_request(1));
    std::vector<Request> received;
    std::thread consumer([&] {
        Request r;
        for (;;) {
            if (small_out.try_pop(r)) {
                received.push_back(r);
                if (r.kind == RequestKind::Stop) {
                    return;
                }
            }
        }
    });
    worker.run();
    consumer.join();
    ASSERT_EQ(received.size(), 21U);
    for (std::size_t i = 0; i < 20; ++i) {
        EXPECT_EQ(received[i].now, 1001 + i);
    }
    EXPECT_GT(worker.stats().forward_waits, 0U);
}

TEST_F(JournalWorkerTest, ABlockingWorkerSleepsUntilItsDoorbellRings) {
    Doorbell in_bell;
    Doorbell out_bell;
    const journal::JournalWriterConfig config;
    Worker worker(journal::JournalWriter::create(path_, config), in_, out_, WaitMode::Block,
                  &in_bell, &out_bell, nullptr);
    std::thread thread([&] { worker.run(); });
    // The worker sleeps with no timeout, so a lost wakeup would hang this test.
    push_orders(3);
    in_bell.ring();
    push(make_stop_request(1));
    in_bell.ring();
    thread.join();
    EXPECT_EQ(drain_out().size(), 4U);
    EXPECT_TRUE(worker.stopped());
    EXPECT_EQ(scan().records, 4U);
}

}  // namespace
}  // namespace litbook::pipeline
