#pragma once

/// Cycle-accurate timing with the TSC.
///
/// Requires an invariant TSC (constant rate across P-states; every x86 server
/// CPU of the last decade has one; check `constant_tsc nonstop_tsc` in
/// /proc/cpuinfo). RDTSC is not serializing, so a measured region is fenced:
///   start: LFENCE; RDTSC       (wait for earlier instructions to finish)
///   stop:  RDTSCP; LFENCE      (RDTSCP waits for the region; LFENCE keeps
///                               later instructions out of it)
/// following Intel's "How to Benchmark Code Execution Times" white paper.

#include "itch/platform.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace itch {

class TscClock {
public:
    [[nodiscard]] static ITCH_ALWAYS_INLINE uint64_t now() noexcept { return __rdtsc(); }

    [[nodiscard]] static ITCH_ALWAYS_INLINE uint64_t start() noexcept {
        _mm_lfence();
        return __rdtsc();
    }

    [[nodiscard]] static ITCH_ALWAYS_INLINE uint64_t stop() noexcept {
        unsigned aux;
        const uint64_t t = __rdtscp(&aux);
        _mm_lfence();
        return t;
    }

    /// Measure TSC ticks per nanosecond against steady_clock.
    [[nodiscard]] static double calibrate_ticks_per_ns(std::chrono::milliseconds window =
                                                           std::chrono::milliseconds(100)) {
        using clk = std::chrono::steady_clock;
        const auto     t0 = clk::now();
        const uint64_t c0 = start();
        std::this_thread::sleep_for(window);
        const uint64_t c1 = stop();
        const auto     t1 = clk::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        return static_cast<double>(c1 - c0) / static_cast<double>(ns);
    }
};

} // namespace itch
