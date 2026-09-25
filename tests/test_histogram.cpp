#include "itch/histogram.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>

namespace {

using itch::LatencyHistogram;

TEST(Histogram, BucketBoundsContainValueWithinRelativeError) {
    std::mt19937_64 rng(1);
    for (int i = 0; i < 200'000; ++i) {
        const uint64_t v = rng() >> (rng() % 64);
        const std::size_t idx = LatencyHistogram::index(v);
        ASSERT_LT(idx, LatencyHistogram::kBuckets);
        const uint64_t lo = LatencyHistogram::lower_bound(idx);
        const uint64_t hi = LatencyHistogram::upper_bound(idx);
        ASSERT_LE(lo, v);
        ASSERT_GE(hi, v);
        if (v >= LatencyHistogram::kLinear) {
            ASSERT_LE(static_cast<double>(hi - lo) / static_cast<double>(lo), 1.0 / 128.0 + 1e-12);
        } else {
            ASSERT_EQ(lo, v);
        }
    }
}

TEST(Histogram, IndexIsMonotonic) {
    std::size_t prev = 0;
    for (uint64_t v = 0; v < (1u << 20); ++v) {
        const std::size_t idx = LatencyHistogram::index(v);
        ASSERT_GE(idx, prev);
        prev = idx;
    }
}

TEST(Histogram, PercentilesOfUniformDistribution) {
    LatencyHistogram h;
    for (uint64_t v = 1; v <= 100'000; ++v) h.record(v);
    EXPECT_EQ(h.count(), 100'000u);
    EXPECT_EQ(h.min(), 1u);
    EXPECT_EQ(h.max(), 100'000u);
    EXPECT_NEAR(static_cast<double>(h.percentile(50)), 50'000, 50'000 * 0.01);
    EXPECT_NEAR(static_cast<double>(h.percentile(99)), 99'000, 99'000 * 0.01);
    EXPECT_EQ(h.percentile(100), 100'000u);
    EXPECT_NEAR(h.mean(), 50'000.5, 1e-6);
}

TEST(Histogram, OutOfRangePercentilesAreClamped) {
    LatencyHistogram h;
    h.record(10);
    h.record(20);
    EXPECT_EQ(h.percentile(-5), 10u);
    EXPECT_EQ(h.percentile(0), 10u);
    EXPECT_EQ(h.percentile(250), 20u);
}

TEST(Histogram, TailIsNotLost) {
    LatencyHistogram h;
    for (int i = 0; i < 9999; ++i) h.record(50);
    h.record(1'000'000);
    EXPECT_EQ(h.percentile(99), 50u);
    EXPECT_EQ(h.percentile(99.99), 50u);        // rank 9999 of 10000
    EXPECT_GE(h.percentile(99.995), 990'000u);  // rank 10000: the outlier
    EXPECT_EQ(h.max(), 1'000'000u);
}

} // namespace
