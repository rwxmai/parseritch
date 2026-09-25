#pragma once

/// Bounded lock-free single-producer / single-consumer ring.
///
/// * Free-running 64-bit indices. The slot is index & (N-1), and all N slots
///   are usable (no sentinel slot). Full is tail - head == N.
/// * Cached opposite index. The producer keeps a private copy of the
///   consumer's head and reloads the shared one only when the ring looks full;
///   the consumer does the same with tail. In steady state each side touches
///   only its own cache line, and cross-core traffic drops to one line
///   transfer per ring wrap instead of one per operation.
/// * Producer, consumer and storage sit in separate kFalseSharingRange-aligned
///   regions.
/// * pop_batch() drains many items for a single release store of head.
///
/// The object embeds its storage (N * sizeof(T) bytes), so allocate large
/// rings on the heap.

#include "itch/platform.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace itch {

template <class T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "element type must be trivially copyable");
    static constexpr uint64_t kMask = N - 1;

public:
    SpscRing() = default;
    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /// Producer. Returns false if the ring is full.
    [[nodiscard]] ITCH_ALWAYS_INLINE bool try_push(const T& item) noexcept {
        const uint64_t tail = prod_.tail.load(std::memory_order_relaxed);
        if (ITCH_UNLIKELY(tail - prod_.cached_head == N)) {
            prod_.cached_head = cons_.head.load(std::memory_order_acquire);
            if (tail - prod_.cached_head == N) return false;
        }
        slots_[tail & kMask] = item;
        prod_.tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer. Returns false if the ring is empty.
    [[nodiscard]] ITCH_ALWAYS_INLINE bool try_pop(T& out) noexcept {
        const uint64_t head = cons_.head.load(std::memory_order_relaxed);
        if (head == cons_.cached_tail) {
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
            if (head == cons_.cached_tail) return false;
        }
        out = slots_[head & kMask];
        cons_.head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// Consumer. Calls f(const T&) for up to `max` items, then publishes the
    /// new head once. Returns the number consumed.
    template <class F>
    ITCH_ALWAYS_INLINE std::size_t pop_batch(F&& f, std::size_t max = N) noexcept(noexcept(f(std::declval<const T&>()))) {
        const uint64_t head = cons_.head.load(std::memory_order_relaxed);
        if (head == cons_.cached_tail)
            cons_.cached_tail = prod_.tail.load(std::memory_order_acquire);
        uint64_t avail = cons_.cached_tail - head;
        if (avail > max) avail = max;
        for (uint64_t i = 0; i < avail; ++i) f(slots_[(head + i) & kMask]);
        if (avail != 0) cons_.head.store(head + avail, std::memory_order_release);
        return static_cast<std::size_t>(avail);
    }

    /// Approximate occupancy (exact when both sides are quiescent).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(prod_.tail.load(std::memory_order_acquire) -
                                        cons_.head.load(std::memory_order_acquire));
    }

    static constexpr std::size_t capacity() noexcept { return N; }

private:
    struct alignas(kFalseSharingRange) Producer {
        std::atomic<uint64_t> tail{0};
        uint64_t cached_head = 0;
    };
    struct alignas(kFalseSharingRange) Consumer {
        std::atomic<uint64_t> head{0};
        uint64_t cached_tail = 0;
    };

    Producer prod_;
    Consumer cons_;
    alignas(kFalseSharingRange) T slots_[N];
};

} // namespace itch
