#pragma once

/// Global order-reference -> resting-order map (Swiss-table design).
///
/// ITCH order reference numbers are unique per day across all symbols, but the
/// spec does not guarantee they increase (a 2009 revision removed that
/// wording). So this is one global hash map, not a flat array and not one map
/// per book. The old design reserved 131072 slots (4 MiB) per stock locate:
/// 259 GiB for the full locate range.
///
/// Layout: a control-byte array (1 tag per slot, see simd/group.hpp) plus a
/// parallel slot array. A lookup:
///   1. hashes the ref once, then takes the group index from the top bits and
///      the 7-bit tag (H2) from the bits just below them;
///   2. loads one aligned group of tags and compares all of them against H2
///      in one instruction;
///   3. compares the full 8-byte ref only on tag hits (a false tag match
///      happens with probability ~1/128 per occupied slot);
///   4. stops at the first group that contains an EMPTY tag.
/// Groups are probed with triangular steps (+1, +2, +3, ...), which visits
/// every group when the group count is a power of two.
///
/// Deletion avoids tombstones where it can: erase writes EMPTY instead of
/// DELETED when the slot's group already contains an EMPTY. That is safe
/// because a probe only continues past a group that has no EMPTY, so no key
/// can live beyond a group that has one. ITCH is very delete-heavy (most
/// orders are cancelled), and this keeps probe chains short without periodic
/// rebuilds.
///
/// Load factor <= 7/8. Growth and tombstone purges rehash into a fresh
/// allocation; that is the only allocating path and it is marked cold. Size
/// the map up front (expected_orders) and it never runs during a session.
///
/// Pointers returned by find()/insert() are invalidated by the next insert().

#include "itch/memory.hpp"
#include "itch/platform.hpp"
#include "itch/simd/group.hpp"

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace itch {

/// A resting order. 24 bytes; the slot array is dense, so a 64-byte line holds
/// 2.67 slots.
struct Order {
    uint64_t ref;
    uint32_t price;   ///< ITCH Price(4): price x 10^4
    uint32_t shares;
    uint16_t locate;
    uint8_t  side;    ///< 'B' or 'S'
    uint8_t  pad_[5];
};
static_assert(sizeof(Order) == 24);

template <class Group>
class BasicOrderMap {
    static constexpr uint32_t W = Group::kWidth;

public:
    struct Stats {
        uint64_t rehashes = 0;
        uint64_t grows    = 0;
    };

    explicit BasicOrderMap(std::size_t expected_orders = 1u << 20) {
        allocate(groups_for(expected_orders));
    }

    BasicOrderMap(const BasicOrderMap&)            = delete;
    BasicOrderMap& operator=(const BasicOrderMap&) = delete;

    [[nodiscard]] ITCH_ALWAYS_INLINE Order* find(uint64_t ref) noexcept {
        const uint64_t h  = hash(ref);
        const uint8_t  h2 = tag(h);
        std::size_t g = group_index(h);
        for (std::size_t step = 1;; ++step) {
            const Group grp(ctrl_ + g * W);
            for (uint32_t i : grp.match(h2)) {
                Order* s = slots_ + g * W + i;
                if (ITCH_LIKELY(s->ref == ref)) return s;
            }
            if (ITCH_LIKELY(grp.match_empty().any())) return nullptr;
            g = (g + step) & group_mask_;
        }
    }

    [[nodiscard]] const Order* find(uint64_t ref) const noexcept {
        return const_cast<BasicOrderMap*>(this)->find(ref);
    }

    /// Claims a slot for `ref` and returns it with `ref` filled in. The caller
    /// fills the other fields. Precondition: ref is not already present
    /// (checked in debug builds).
    [[nodiscard]] ITCH_ALWAYS_INLINE Order* insert(uint64_t ref) noexcept {
        assert(find(ref) == nullptr && "duplicate order reference");
        if (ITCH_UNLIKELY(growth_left_ == 0)) rehash_for_insert();

        const uint64_t h = hash(ref);
        std::size_t g = group_index(h);
        for (std::size_t step = 1;; ++step) {
            const Group grp(ctrl_ + g * W);
            const auto free = grp.match_empty_or_deleted();
            if (ITCH_LIKELY(free.any())) {
                const std::size_t idx = g * W + free.lowest();
                if (ctrl_[idx] == simd::kCtrlEmpty) --growth_left_;
                else                                --tombstones_;
                ctrl_[idx] = tag(h);
                ++size_;
                Order* s = slots_ + idx;
                s->ref = ref;
                return s;
            }
            g = (g + step) & group_mask_;
        }
    }

    ITCH_ALWAYS_INLINE void erase(Order* s) noexcept {
        const auto idx = static_cast<std::size_t>(s - slots_);
        assert(idx < capacity_ && ctrl_[idx] < 0x80 && "erase of a non-full slot");
        const Group grp(ctrl_ + (idx / W) * W);
        if (grp.match_empty().any()) {
            ctrl_[idx] = simd::kCtrlEmpty;
            ++growth_left_;
        } else {
            ctrl_[idx] = simd::kCtrlDeleted;
            ++tombstones_;
        }
        --size_;
    }

    /// Prefetch the tag group and first slot line that a later find(ref) touches.
    ITCH_ALWAYS_INLINE void prefetch(uint64_t ref) const noexcept {
        const std::size_t g = group_index(hash(ref));
        __builtin_prefetch(ctrl_ + g * W);
        __builtin_prefetch(slots_ + g * W);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t tombstones() const noexcept { return tombstones_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    /// Remove everything, keep the allocation.
    void clear() noexcept {
        std::memset(ctrl_, simd::kCtrlEmpty, capacity_);
        size_ = 0;
        tombstones_ = 0;
        growth_left_ = max_load(capacity_);
    }

    /// Calls f(const Order&) for every live order (unspecified order).
    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < capacity_; ++i)
            if (ctrl_[i] < 0x80) f(slots_[i]);
    }

private:
    // Fibonacci hashing: one multiply by 2^64/phi. The high bits of the
    // product depend on every bit of the ref, so both the group index and H2
    // come from the top of the word.
    static ITCH_ALWAYS_INLINE uint64_t hash(uint64_t ref) noexcept {
        return ref * 0x9E37'79B9'7F4A'7C15ULL;
    }
    ITCH_ALWAYS_INLINE std::size_t group_index(uint64_t h) const noexcept {
        return static_cast<std::size_t>(h >> group_shift_);
    }
    ITCH_ALWAYS_INLINE uint8_t tag(uint64_t h) const noexcept {
        return static_cast<uint8_t>((h >> (group_shift_ - 7)) & 0x7F);
    }

    static constexpr std::size_t max_load(std::size_t cap) noexcept { return cap - cap / 8; }

    static std::size_t groups_for(std::size_t orders) noexcept {
        const std::size_t slots = orders + orders / 7 + 1;  // <= 7/8 full
        std::size_t groups = std::bit_ceil((slots + W - 1) / W);
        return groups < 2 ? 2 : groups;  // >= 2 keeps group_shift_ <= 63
    }

    void allocate(std::size_t groups) {
        capacity_    = groups * W;
        group_mask_  = groups - 1;
        group_shift_ = 64u - static_cast<unsigned>(std::countr_zero(groups));
        ctrl_buf_    = LargeBuffer(capacity_);
        slot_buf_    = LargeBuffer(capacity_ * sizeof(Order));
        ctrl_        = ctrl_buf_.as<uint8_t>();
        slots_       = slot_buf_.as<Order>();
        clear();
    }

    /// Cold path: no EMPTY budget left. If more than half of the load budget is
    /// live orders, double the table; otherwise the budget went to tombstones,
    /// so rebuild at the same size to purge them.
    ITCH_COLD void rehash_for_insert() {
        const std::size_t groups = capacity_ / W;
        const bool grow = size_ * 2 > max_load(capacity_);
        LargeBuffer old_ctrl  = std::move(ctrl_buf_);
        LargeBuffer old_slots = std::move(slot_buf_);
        const std::size_t old_cap = capacity_;
        allocate(grow ? groups * 2 : groups);
        const auto* oc = old_ctrl.as<const uint8_t>();
        const auto* os = old_slots.as<const Order>();
        for (std::size_t i = 0; i < old_cap; ++i) {
            if (oc[i] < 0x80) *insert(os[i].ref) = os[i];
        }
        ++stats_.rehashes;
        if (grow) ++stats_.grows;
    }

    LargeBuffer ctrl_buf_;
    LargeBuffer slot_buf_;
    uint8_t*    ctrl_  = nullptr;
    Order*      slots_ = nullptr;
    std::size_t capacity_    = 0;
    std::size_t group_mask_  = 0;
    unsigned    group_shift_ = 64;
    std::size_t size_        = 0;
    std::size_t tombstones_  = 0;
    std::size_t growth_left_ = 0;
    Stats       stats_{};
};

using OrderMap = BasicOrderMap<simd::GroupBest>;

} // namespace itch
