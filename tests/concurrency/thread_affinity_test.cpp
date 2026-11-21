#include "litbook/concurrency/thread_affinity.hpp"

#include <gtest/gtest.h>

#include <sched.h>

#include <algorithm>
#include <cerrno>
#include <thread>
#include <vector>

namespace litbook::concurrency {
namespace {

TEST(ThreadAffinityTest, AllowedCpusAreSortedAndIncludeTheCurrentOne) {
    const std::vector<int> cpus = allowed_cpus();
    ASSERT_FALSE(cpus.empty());
    for (std::size_t i = 1; i < cpus.size(); ++i) {
        EXPECT_LT(cpus[i - 1], cpus[i]);
    }
    const int now = current_cpu();
    ASSERT_GE(now, 0);
    EXPECT_NE(std::ranges::find(cpus, now), cpus.end());
}

// Pinning happens on a new thread so the test runner's own thread keeps every CPU.
TEST(ThreadAffinityTest, PinnedThreadRunsOnlyOnThatCpu) {
    const std::vector<int> cpus = allowed_cpus();
    ASSERT_FALSE(cpus.empty());
    const int target = cpus.back();

    int result = -1;
    std::vector<int> seen_on;
    std::vector<int> allowed_after;
    std::thread worker([&] {
        result = pin_current_thread(target);
        for (int i = 0; i < 100; ++i) {
            seen_on.push_back(current_cpu());
            std::this_thread::yield();
        }
        allowed_after = allowed_cpus();
    });
    worker.join();

    ASSERT_EQ(result, 0);
    for (const int cpu : seen_on) {
        EXPECT_EQ(cpu, target);
    }
    EXPECT_EQ(allowed_after, std::vector<int>{target});
}

TEST(ThreadAffinityTest, RefusesCpuNumbersOutOfRange) {
    std::thread worker([] {
        EXPECT_EQ(pin_current_thread(-1), EINVAL);
        EXPECT_EQ(pin_current_thread(CPU_SETSIZE), EINVAL);
        EXPECT_EQ(pin_current_thread(1'000'000), EINVAL);
    });
    worker.join();
}

// A CPU number inside the set's range that this machine does not have is refused by
// the kernel itself.
TEST(ThreadAffinityTest, RefusesACpuThisProcessCannotUse) {
    const std::vector<int> cpus = allowed_cpus();
    ASSERT_FALSE(cpus.empty());
    const int missing = CPU_SETSIZE - 1;
    if (cpus.back() == missing) {
        GTEST_SKIP() << "this machine really has CPU " << missing;
    }
    int result = 0;
    std::thread worker([&] { result = pin_current_thread(missing); });
    worker.join();
    EXPECT_EQ(result, EINVAL);
}

}  // namespace
}  // namespace litbook::concurrency
