#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace litbook::metrics {

// A fixed-size histogram of latencies in nanoseconds, used to report percentiles such
// as p50, p99 and p99.9. The design follows HdrHistogram, the standard tool for this.
//
// Values below 64 are counted exactly, one bucket per value. Above that, every range
// between two powers of two (64..127, 128..255, and so on) is split into 32 equal
// buckets. So a recorded value is off by at most 1/32 of itself, about 3%, which is
// plenty for latency, and the whole histogram is a fixed array of about 1,200 counters.
// Recording a value costs a few instructions and never allocates, so it is safe to call
// on the hot path.
//
// Values above kMaxTrackable (about 18 minutes) are counted in the last bucket. The
// minimum, maximum and sum are tracked exactly, so max() is always the true maximum.
//
// These invariants always hold. count() equals the sum of all bucket counts, and when
// count() > 0, min() <= every recorded value <= max().
class LatencyHistogram {
public:
    static constexpr std::uint64_t kMaxTrackable = (std::uint64_t{1} << 40) - 1;

    void record(std::uint64_t value_ns) noexcept { record_n(value_ns, 1); }

    void record_n(std::uint64_t value_ns, std::uint64_t times) noexcept {
        if (times == 0) {
            return;
        }
        counts_[bucket_index(value_ns)] += times;
        count_ += times;
        sum_ += value_ns * times;
        min_ = value_ns < min_ ? value_ns : min_;
        max_ = value_ns > max_ ? value_ns : max_;
    }

    // Records a value measured by a loop that sends one request every
    // expected_interval_ns and waits for each reply before sending the next one.
    // When one reply is slow, that loop also delays every request that should have
    // been sent meanwhile, and those delays would never be recorded. This is called
    // coordinated omission, and it hides exactly the slow cases we want to see.
    // Following HdrHistogram, we add the missing samples back. A value of 10 times the
    // interval also records values of 9, 8, ..., 1 times the interval.
    void record_corrected(std::uint64_t value_ns, std::uint64_t expected_interval_ns) noexcept {
        record(value_ns);
        if (expected_interval_ns == 0) {
            return;
        }
        for (std::uint64_t missed = value_ns; missed > expected_interval_ns;) {
            missed -= expected_interval_ns;
            record(missed);
        }
    }

    // Adds every value recorded in another histogram to this one.
    void merge(const LatencyHistogram& other) noexcept {
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            counts_[i] += other.counts_[i];
        }
        count_ += other.count_;
        sum_ += other.sum_;
        min_ = other.min_ < min_ ? other.min_ : min_;
        max_ = other.max_ > max_ ? other.max_ : max_;
    }

    void reset() noexcept { *this = LatencyHistogram{}; }

    // The smallest value v such that at least `percent` percent of the recorded values
    // are <= v, rounded up to the top of v's bucket (and never above max()). So the
    // answer can overstate the true percentile by up to about 3%, but never understates
    // it. percent is clamped to [0, 100]. Returns 0 for an empty histogram.
    [[nodiscard]] std::uint64_t percentile(double percent) const noexcept {
        if (count_ == 0) {
            return 0;
        }
        const std::uint64_t rank = rank_of(percent, count_);
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBucketCount; ++i) {
            seen += counts_[i];
            if (seen >= rank) {
                const std::uint64_t top = bucket_high(i);
                return top < max_ ? top : max_;
            }
        }
        return max_;  // not reached, because seen ends at count_
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_);
    }

    // Bucket layout, public so the tests can check it directly.
    static constexpr unsigned kLinearBits = 6;                   // values 0..63 are exact
    static constexpr std::uint64_t kLinear = 1U << kLinearBits;  // 64
    static constexpr std::uint64_t kSubBuckets = kLinear / 2;    // 32 per power of two
    static constexpr std::size_t kBucketCount =
        kLinear + (std::bit_width(kMaxTrackable) - kLinearBits) * kSubBuckets;

    static constexpr std::size_t bucket_index(std::uint64_t value) noexcept {
        if (value < kLinear) {
            return static_cast<std::size_t>(value);
        }
        if (value > kMaxTrackable) {
            value = kMaxTrackable;
        }
        // shift is how far value must be shifted right to fit in 6 bits. After the
        // shift, its top bit is set, so `top` is in 32..63.
        const auto shift = static_cast<unsigned>(std::bit_width(value)) - kLinearBits;
        const std::uint64_t top = value >> shift;
        return static_cast<std::size_t>(kLinear + (shift - 1) * kSubBuckets + (top - kSubBuckets));
    }

    // The smallest and largest value that land in bucket i.
    static constexpr std::uint64_t bucket_low(std::size_t i) noexcept {
        if (i < kLinear) {
            return i;
        }
        const std::uint64_t shift = (i - kLinear) / kSubBuckets + 1;
        const std::uint64_t top = (i - kLinear) % kSubBuckets + kSubBuckets;
        return top << shift;
    }
    static constexpr std::uint64_t bucket_high(std::size_t i) noexcept {
        return i + 1 < kBucketCount ? bucket_low(i + 1) - 1 : kMaxTrackable;
    }

private:
    // Which recorded value (1st, 2nd, ...) in sorted order is the percentile, which is
    // ceil(percent / 100 * count), but at least 1 so that p0 means the smallest value.
    //
    // Doing this in floating point gives wrong answers. For example 7 / 100.0 * 100 is
    // 7.000000000000001, whose ceil is 8. So the percent is first rounded to a whole
    // number of parts per billion, and the rest is integer arithmetic. The count is
    // split as q * 1e9 + r so that no product can overflow 64 bits.
    static std::uint64_t rank_of(double percent, std::uint64_t count) noexcept {
        constexpr std::uint64_t kBillion = 1'000'000'000;
        const double p = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        const auto ppb = static_cast<std::uint64_t>(std::llround(p * 1e7));  // <= 1e9
        const std::uint64_t q = count / kBillion;
        const std::uint64_t r = count % kBillion;
        const std::uint64_t rank = q * ppb + (r * ppb + kBillion - 1) / kBillion;
        return rank == 0 ? 1 : rank;
    }

    std::array<std::uint64_t, kBucketCount> counts_{};
    std::uint64_t count_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_ = 0;
};

}  // namespace litbook::metrics
