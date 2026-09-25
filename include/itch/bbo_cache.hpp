#pragma once

/// Single-writer / multi-reader top-of-book cache (seqlock).
///
/// The previous version published each field with a relaxed store plus a
/// release store of the timestamp. That orders the writes, but a reader could
/// still see a torn snapshot, e.g. the new bid price paired with the old bid
/// quantity. A seqlock fixes that:
///
///   writer:  seq = odd -> fence(release) -> fields -> seq = even (release)
///   reader:  s1 = seq (acquire) -> fields -> fence(acquire) -> s2 = seq;
///            retry unless s1 == s2 and s1 is even
///
/// Fields are std::atomic accessed with relaxed ordering, so the racy reads
/// are well-defined C++ (no data-race UB, clean under TSan). On x86 these
/// compile to plain MOVs. All fields share one cache line: a reader needs all
/// of them anyway, and spreading them over 5 lines (as before) cost 5 misses
/// per snapshot.

#include "itch/platform.hpp"

#include <atomic>
#include <cstdint>

namespace itch {

struct BboSnapshot {
    uint32_t bid_price = 0;
    uint32_t ask_price = 0;
    uint64_t bid_qty   = 0;
    uint64_t ask_qty   = 0;
    uint64_t timestamp = 0;  ///< ITCH ns since midnight of the last update
};

class alignas(kFalseSharingRange) BboCache {
public:
    /// Writer side. Must be called from a single thread.
    ITCH_ALWAYS_INLINE void store(const BboSnapshot& s) noexcept {
        const uint64_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        prices_.store(pack(s.bid_price, s.ask_price), std::memory_order_relaxed);
        bid_qty_.store(s.bid_qty, std::memory_order_relaxed);
        ask_qty_.store(s.ask_qty, std::memory_order_relaxed);
        ts_.store(s.timestamp, std::memory_order_relaxed);
        seq_.store(seq + 2, std::memory_order_release);
    }

    /// Reader side: one attempt. Returns false if a write was in progress.
    [[nodiscard]] ITCH_ALWAYS_INLINE bool try_load(BboSnapshot& out) const noexcept {
        const uint64_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1) return false;
        const uint64_t prices = prices_.load(std::memory_order_relaxed);
        out.bid_qty   = bid_qty_.load(std::memory_order_relaxed);
        out.ask_qty   = ask_qty_.load(std::memory_order_relaxed);
        out.timestamp = ts_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t s2 = seq_.load(std::memory_order_relaxed);
        out.bid_price = static_cast<uint32_t>(prices);
        out.ask_price = static_cast<uint32_t>(prices >> 32);
        return s1 == s2;
    }

    /// Reader side: spin until a consistent snapshot is read.
    [[nodiscard]] BboSnapshot load() const noexcept {
        BboSnapshot s;
        while (!try_load(s)) cpu_relax();
        return s;
    }

    /// Number of completed writes.
    [[nodiscard]] uint64_t version() const noexcept {
        return seq_.load(std::memory_order_acquire) / 2;
    }

private:
    static constexpr uint64_t pack(uint32_t lo, uint32_t hi) noexcept {
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }

    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> prices_{0};
    std::atomic<uint64_t> bid_qty_{0};
    std::atomic<uint64_t> ask_qty_{0};
    std::atomic<uint64_t> ts_{0};
};

static_assert(sizeof(BboCache) == kFalseSharingRange);

} // namespace itch
