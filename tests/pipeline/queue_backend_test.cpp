#include "litbook/pipeline/queue_backend.hpp"

#include "litbook/book/sorted_order_book.hpp"
#include "litbook/concurrency/spsc_queue.hpp"
#include "litbook/gateway/order_backend.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "book/test_limits.hpp"
#include "pipeline/pipeline_test_support.hpp"

namespace litbook::pipeline {
namespace {

using gateway::ForwardResult;
using testing::frame_of;
using testing::limit;

using Requests = concurrency::SpscQueue<Request>;
using Backend = QueueBackend<SortedOrderBook, Requests>;
static_assert(gateway::OrderBackend<Backend>);

// The order entry layer the backend makes its startup calls on. Its replies go nowhere.
class NullSink final : public protocol::OutboundSink {
public:
    void send(OwnerId /*owner*/, std::span<const std::byte> /*frame*/) override {}
};

struct BackendRig {
    explicit BackendRig(std::size_t capacity, Doorbell* bell = nullptr)
        : requests(capacity),
          orders(
              sink,
              protocol::OrderEntryConfig{.max_owners = 3, .max_live_orders = 64, .max_symbols = 1}),
          backend(orders, requests, bell) {}

    Requests requests;
    NullSink sink;
    protocol::OrderEntry<SortedOrderBook> orders;
    Backend backend;
};

TEST(QueueBackendTest, AValidFrameIsQueuedWithItsOwnerAndTime) {
    BackendRig rig(4);
    const auto frame = frame_of(limit(Side::Buy, 1, 100, 10'000));
    EXPECT_EQ(rig.backend.forward(2, frame, 55), ForwardResult::Taken);
    Request r;
    ASSERT_TRUE(rig.requests.try_pop(r));
    EXPECT_EQ(r.kind, RequestKind::Frame);
    EXPECT_EQ(r.owner, 2U);
    EXPECT_EQ(r.now, 55U);
    EXPECT_TRUE(std::equal(frame.begin(), frame.end(), r.bytes().begin(), r.bytes().end()));
}

TEST(QueueBackendTest, AMalformedFrameIsRefusedAndNotQueued) {
    BackendRig rig(4);
    auto frame = frame_of(limit(Side::Buy, 1, 100, 10'000));
    frame.pop_back();  // One byte short.
    EXPECT_EQ(rig.backend.forward(1, frame, 0), ForwardResult::Malformed);
    EXPECT_EQ(rig.backend.forward(1, {}, 0), ForwardResult::Malformed);
    EXPECT_EQ(rig.requests.size_approx(), 0U);
    EXPECT_EQ(rig.backend.malformed(), 2U);
}

TEST(QueueBackendTest, AFullQueueMakesItBusyAndChangesNothing) {
    BackendRig rig(2);
    const auto frame = frame_of(limit(Side::Sell, 1, 100, 10'000));
    EXPECT_EQ(rig.backend.forward(1, frame, 0), ForwardResult::Taken);
    EXPECT_EQ(rig.backend.forward(1, frame, 0), ForwardResult::Taken);
    EXPECT_EQ(rig.backend.forward(1, frame, 0), ForwardResult::Busy);
    EXPECT_FALSE(rig.backend.cancel_all(1, 0));
    EXPECT_EQ(rig.backend.busy(), 2U);
    EXPECT_EQ(rig.requests.size_approx(), 2U);
    Request r;
    ASSERT_TRUE(rig.requests.try_pop(r));
    EXPECT_TRUE(rig.backend.cancel_all(3, 9));
}

TEST(QueueBackendTest, CancelAllIsQueuedAsItsOwnRequest) {
    BackendRig rig(4);
    EXPECT_TRUE(rig.backend.cancel_all(3, 9));
    Request r;
    ASSERT_TRUE(rig.requests.try_pop(r));
    EXPECT_EQ(r.kind, RequestKind::CancelAll);
    EXPECT_EQ(r.owner, 3U);
    EXPECT_EQ(r.now, 9U);
}

TEST(QueueBackendTest, StartupCallsReachTheMatchingSidesOrderEntry) {
    BackendRig rig(4);
    EXPECT_EQ(rig.backend.max_owners(), 3U);
    const SymbolId id = rig.backend.add_symbol(
        testing::aapl(), litbook::testing::with_test_limits(BookConfig{.tick_size = 100}));
    EXPECT_EQ(id, 0U);
    EXPECT_EQ(rig.orders.engine().symbol_count(), 1U);
    rig.backend.set_risk_limits(2, protocol::RiskLimits{.max_order_qty = 500});
    EXPECT_EQ(rig.orders.risk_limits(2).max_order_qty, 500U);
}

TEST(QueueBackendTest, EachQueuedRequestRingsTheDoorbell) {
    Doorbell bell;
    BackendRig rig(4, &bell);
    ASSERT_TRUE(bell.prepare_to_sleep());  // The matching thread is about to sleep.
    EXPECT_EQ(rig.backend.forward(1, frame_of(limit(Side::Buy, 1, 1, 10'000)), 0),
              ForwardResult::Taken);
    EXPECT_EQ(bell.wakeups_sent(), 1U);
    bell.woke_up();
    ASSERT_TRUE(bell.prepare_to_sleep());
    EXPECT_TRUE(rig.backend.cancel_all(1, 0));
    EXPECT_EQ(bell.wakeups_sent(), 2U);
}

}  // namespace
}  // namespace litbook::pipeline
