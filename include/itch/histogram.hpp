#pragma once

/// Log-linear latency histogram (the HdrHistogram bucketing scheme).
///
/// Values below 2^kSubBits are recorded exactly. Above that, each power of two
/// is split into 2^(kSubBits-1) equal sub-buckets, so any recorded value is
/// reported to within a relative error of 2^-(kSubBits-1) (0.78% for
/// kSubBits = 8). Recording is branch-light O(1); percentiles are an O(buckets)
/// scan. Unlike the old benchmark histogram (power-of-two buckets over
/// per-pass averages), this records every sample and keeps the tail.

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace itch {

class LatencyHistogram {
public:
    static constexpr unsigned kSubBits  = 8;
    static constexpr uint64_t kLinear   = 1ULL << kSubBits;        // exact region
    static constexpr uint64_t kHalf     = 1ULL << (kSubBits - 1);  // sub-buckets per octave
    static constexpr std::size_t kBuckets = kLinear + (64 - kSubBits) * kHalf;

    void record(uint64_t v) noexcept {
        ++counts_[index(v)];
        ++count_;
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
        sum_ += v;
    }

    /// Nearest-rank percentile: the bucket holding the ceil(p/100 * n)-th
    /// smallest sample, reported as that bucket's upper bound (clamped to max).
    [[nodiscard]] uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        if (!(p > 0.0)) return min_;    // also catches NaN; avoids UB in the cast below
        if (p >= 100.0) return max_;
        const double rank = p / 100.0 * static_cast<double>(count_);
        auto target = static_cast<uint64_t>(std::ceil(rank - 1e-9));
        if (target == 0) target = 1;
        uint64_t cum = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            cum += counts_[i];
            if (cum >= target) {
                const uint64_t hi = upper_bound(i);
                return hi < max_ ? hi : max_;
            }
        }
        return max_;
    }

    [[nodiscard]] uint64_t count() const noexcept { return count_; }
    [[nodiscard]] uint64_t min() const noexcept { return count_ ? min_ : 0; }
    [[nodiscard]] uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
    }

    void merge(const LatencyHistogram& o) noexcept {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        count_ += o.count_;
        sum_   += o.sum_;
        if (o.count_ && o.min_ < min_) min_ = o.min_;
        if (o.max_ > max_) max_ = o.max_;
    }

    static constexpr std::size_t index(uint64_t v) noexcept {
        if (v < kLinear) return static_cast<std::size_t>(v);
        const unsigned e     = static_cast<unsigned>(std::bit_width(v)) - 1;  // >= kSubBits
        const unsigned shift = e - (kSubBits - 1);                            // >= 1
        const uint64_t top   = v >> shift;                                    // [kHalf, kLinear)
        return static_cast<std::size_t>(kLinear + (shift - 1) * kHalf + (top - kHalf));
    }

    static constexpr uint64_t lower_bound(std::size_t i) noexcept {
        if (i < kLinear) return i;
        const uint64_t j     = i - kLinear;
        const unsigned shift = static_cast<unsigned>(j / kHalf) + 1;
        const uint64_t top   = kHalf + j % kHalf;
        return top << shift;
    }

    static constexpr uint64_t upper_bound(std::size_t i) noexcept {
        if (i < kLinear) return i;
        const uint64_t j     = i - kLinear;
        const unsigned shift = static_cast<unsigned>(j / kHalf) + 1;
        const uint64_t top   = kHalf + j % kHalf;
        return ((top + 1) << shift) - 1;
    }

private:
    std::array<uint64_t, kBuckets> counts_{};
    uint64_t count_ = 0;
    uint64_t sum_   = 0;
    uint64_t min_   = UINT64_MAX;
    uint64_t max_   = 0;
};

} // namespace itch
