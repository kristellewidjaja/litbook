#include "litbook/metrics/latency_histogram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace litbook::metrics {
namespace {

using H = LatencyHistogram;

TEST(LatencyHistogramTest, EmptyHistogramReportsZeros) {
    const H h;
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.min(), 0U);
    EXPECT_EQ(h.max(), 0U);
    EXPECT_EQ(h.mean(), 0.0);
    EXPECT_EQ(h.percentile(50), 0U);
    EXPECT_EQ(h.percentile(100), 0U);
}

TEST(LatencyHistogramTest, SmallValuesAreExact) {
    H h;
    for (std::uint64_t v = 0; v < 64; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.count(), 64U);
    EXPECT_EQ(h.min(), 0U);
    EXPECT_EQ(h.max(), 63U);
    EXPECT_EQ(h.percentile(0), 0U);    // p0 is the smallest value
    EXPECT_EQ(h.percentile(50), 31U);  // the 32nd of 64 values
    EXPECT_EQ(h.percentile(100), 63U);
    EXPECT_DOUBLE_EQ(h.mean(), 31.5);
}

// Every bucket's range starts right after the previous one ends, the first starts at
// 0, and each value maps to the bucket whose range contains it.
TEST(LatencyHistogramTest, BucketsCoverEveryValueWithoutGaps) {
    EXPECT_EQ(H::bucket_low(0), 0U);
    for (std::size_t i = 1; i < H::kBucketCount; ++i) {
        ASSERT_EQ(H::bucket_low(i), H::bucket_high(i - 1) + 1) << "bucket " << i;
    }
    EXPECT_EQ(H::bucket_high(H::kBucketCount - 1), H::kMaxTrackable);
    EXPECT_EQ(H::bucket_index(H::kMaxTrackable), H::kBucketCount - 1);

    std::mt19937_64 rng(7);
    for (int n = 0; n < 100'000; ++n) {
        const std::uint64_t v = rng() >> (rng() % 64);  // spread over all magnitudes
        const std::size_t i = H::bucket_index(v);
        ASSERT_LT(i, H::kBucketCount);
        if (v <= H::kMaxTrackable) {
            ASSERT_LE(H::bucket_low(i), v) << v;
            ASSERT_GE(H::bucket_high(i), v) << v;
        }
    }
}

// Each bucket is at most 1/32 of its lowest value wide, so the error is about 3%.
TEST(LatencyHistogramTest, BucketWidthIsWithinThreePercent) {
    for (std::size_t i = H::kLinear; i < H::kBucketCount; ++i) {
        const auto width = static_cast<double>(H::bucket_high(i) - H::bucket_low(i) + 1);
        ASSERT_LE(width / static_cast<double>(H::bucket_low(i)), 1.0 / 32.0) << "bucket " << i;
    }
}

TEST(LatencyHistogramTest, PowerOfTwoBoundaries) {
    EXPECT_EQ(H::bucket_index(63), 63U);
    EXPECT_EQ(H::bucket_index(64), 64U);  // first bucket of width 2
    EXPECT_EQ(H::bucket_index(65), 64U);
    EXPECT_EQ(H::bucket_index(127), 95U);  // last bucket of 64..127
    EXPECT_EQ(H::bucket_index(128), 96U);  // first bucket of width 4
    EXPECT_EQ(H::bucket_low(96), 128U);
    EXPECT_EQ(H::bucket_high(96), 131U);
}

// Percentiles from the histogram never understate the exact ones and overstate them
// by at most one bucket width, compared against a sorted copy of the same samples.
TEST(LatencyHistogramTest, PercentilesMatchSortedSamples) {
    std::mt19937_64 rng(11);
    std::lognormal_distribution<double> latency(5.0, 1.5);  // a long tail, like real latency
    std::vector<std::uint64_t> samples;
    H h;
    for (int n = 0; n < 200'000; ++n) {
        const auto v = static_cast<std::uint64_t>(latency(rng));
        samples.push_back(v);
        h.record(v);
    }
    std::ranges::sort(samples);
    for (const double p : {0.0, 1.0, 50.0, 90.0, 99.0, 99.9, 99.99, 100.0}) {
        const auto rank = static_cast<std::size_t>(
            std::max(1.0, std::ceil(p / 100.0 * static_cast<double>(samples.size()))));
        const std::uint64_t exact = samples[rank - 1];
        const std::uint64_t got = h.percentile(p);
        EXPECT_GE(got, exact) << "p" << p;
        EXPECT_LE(static_cast<double>(got), static_cast<double>(exact) * (1.0 + 1.0 / 32.0) + 1)
            << "p" << p;
    }
    EXPECT_EQ(h.max(), samples.back());
    EXPECT_EQ(h.min(), samples.front());
    EXPECT_EQ(h.percentile(100), samples.back());  // capped at the true maximum
}

// Regression test. The rank used to be computed as ceil(99.9 / 100 * 1000) in floating
// point, which is ceil(999.0000000000001) = 1000. With 999 fast values and one slow one,
// p99.9 then reported the slow value. The rank is now computed with integers.
TEST(LatencyHistogramTest, DecimalPercentilesUseTheExactRank) {
    H h;
    h.record_n(10, 999);
    h.record(1'000);
    EXPECT_EQ(h.percentile(99.9), 10U);
    EXPECT_EQ(h.percentile(99.91), 1'000U);

    H hundred;
    for (std::uint64_t v = 0; v < 100; ++v) {
        hundred.record(v);
    }
    EXPECT_EQ(hundred.percentile(7), 6U);    // 7 / 100 * 100 is 7.000000000000001 as a double
    EXPECT_EQ(hundred.percentile(55), 54U);  // and 55 / 100 * 100 is 55.00000000000001
    EXPECT_EQ(hundred.percentile(7.5), 7U);  // 7.5 values round up to the 8th
}

// Counts near 2^64 must not overflow while computing the rank.
TEST(LatencyHistogramTest, RankDoesNotOverflowForHugeCounts) {
    H h;
    h.record_n(1, std::uint64_t{1} << 62);
    h.record_n(2, std::uint64_t{1} << 62);
    EXPECT_EQ(h.percentile(50), 1U);
    EXPECT_EQ(h.percentile(50.001), 2U);
}

TEST(LatencyHistogramTest, PercentileArgumentIsClamped) {
    H h;
    h.record(10);
    h.record(20);
    EXPECT_EQ(h.percentile(-5), 10U);
    EXPECT_EQ(h.percentile(250), 20U);
}

TEST(LatencyHistogramTest, HugeValuesLandInTheLastBucketButMaxIsExact) {
    H h;
    const std::uint64_t huge = std::uint64_t{1} << 50;
    h.record(huge);
    EXPECT_EQ(h.max(), huge);
    EXPECT_EQ(h.percentile(100), H::kMaxTrackable);  // the bucket top is below max here
}

TEST(LatencyHistogramTest, RecordNCountsRepeatedValues) {
    H h;
    h.record_n(100, 5);
    h.record_n(200, 0);  // recording zero times changes nothing
    EXPECT_EQ(h.count(), 5U);
    EXPECT_EQ(h.max(), 100U);
    EXPECT_DOUBLE_EQ(h.mean(), 100.0);
}

TEST(LatencyHistogramTest, MergeAddsEverything) {
    H a;
    H b;
    a.record(5);
    a.record(1'000);
    b.record(2);
    b.record(70'000);
    a.merge(b);
    EXPECT_EQ(a.count(), 4U);
    EXPECT_EQ(a.min(), 2U);
    EXPECT_EQ(a.max(), 70'000U);
    EXPECT_DOUBLE_EQ(a.mean(), (5.0 + 1'000 + 2 + 70'000) / 4);
    const H empty;
    a.merge(empty);  // merging an empty histogram must not disturb min or max
    EXPECT_EQ(a.min(), 2U);
    EXPECT_EQ(a.max(), 70'000U);
}

TEST(LatencyHistogramTest, ResetEmptiesTheHistogram) {
    H h;
    h.record(42);
    h.reset();
    EXPECT_EQ(h.count(), 0U);
    EXPECT_EQ(h.min(), 0U);
    EXPECT_EQ(h.max(), 0U);
}

// One reply that took 10 intervals hid 9 requests that should have been sent while we
// waited. The corrected recording adds them back at 9, 8, ..., 1 intervals.
TEST(LatencyHistogramTest, CorrectedRecordingAddsTheMissedSamples) {
    H h;
    h.record_corrected(1'000, 100);
    EXPECT_EQ(h.count(), 10U);
    EXPECT_EQ(h.min(), 100U);
    EXPECT_EQ(h.max(), 1'000U);
    EXPECT_DOUBLE_EQ(h.mean(), 550.0);  // 100 + 200 + ... + 1,000, divided by 10

    H fast;
    fast.record_corrected(50, 100);  // no slower than the interval, so nothing was missed
    fast.record_corrected(50, 0);    // an interval of 0 disables the correction
    EXPECT_EQ(fast.count(), 2U);
}

}  // namespace
}  // namespace litbook::metrics
