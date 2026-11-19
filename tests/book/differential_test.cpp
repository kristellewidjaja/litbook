// This is a long differential test. It runs FastOrderBook against MapOrderBook (the trusted
// reference) on long random streams, without the slow naive oracle. The events from every
// operation must match exactly, and the invariants are checked periodically.
//
// Each workload runs 100,000 operations by default, which is 1.5 million across all 15
// workloads. Set LITBOOK_DIFF_OPS for longer runs, for example
// LITBOOK_DIFF_OPS=5000000 ctest --preset release.

#include "litbook/book/fast_order_book.hpp"
#include "litbook/book/map_order_book.hpp"
#include "litbook/book/recording_listener.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "book/book_test_support.hpp"
#include "book/workload.hpp"

namespace litbook {
namespace {

using testing::WorkloadParams;

std::uint64_t operations_per_workload() {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before any threads exist
    const char* env = std::getenv("LITBOOK_DIFF_OPS");
    return env == nullptr ? 100'000 : std::stoull(env);
}

class DifferentialTest : public ::testing::TestWithParam<WorkloadParams> {};

TEST_P(DifferentialTest, FastBookMatchesMapBook) {
    const WorkloadParams& p = GetParam();
    const BookConfig config = testing::book_config(p);
    const std::uint64_t operations = operations_per_workload();

    RecordingListener map_rec;
    RecordingListener fast_rec;
    MapOrderBook map_book(map_rec, config);
    FastOrderBook fast_book(fast_rec, config);
    testing::Generator gen(p);

    for (std::uint64_t op = 0; op < operations; ++op) {
        const testing::Operation next = gen.next();
        testing::apply(map_book, next);
        testing::apply(fast_book, next);
        ASSERT_EQ(fast_rec.events(), map_rec.events()) << "diverged at operation " << op;
        map_rec.clear();
        fast_rec.clear();
        if (op % 4096 == 0) {
            ASSERT_TRUE(fast_book.check_invariants())
                << "fast invariant broken at operation " << op;
            ASSERT_TRUE(map_book.check_invariants()) << "map invariant broken at operation " << op;
        }
    }
    EXPECT_TRUE(fast_book.check_invariants());
    EXPECT_EQ(fast_book.order_count(), map_book.order_count());
    RecordProperty("operations", std::to_string(operations));
}

INSTANTIATE_TEST_SUITE_P(Workloads, DifferentialTest,
                         ::testing::ValuesIn(testing::standard_workloads()),
                         [](const ::testing::TestParamInfo<WorkloadParams>& param_info) {
                             return param_info.param.name;
                         });

}  // namespace
}  // namespace litbook
