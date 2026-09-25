#pragma once

/// The pre-refactor order lookup, kept verbatim (apart from a runtime-sized
/// table) as a benchmark baseline. Source: OrderBook::find_order in the
/// baseline commit.
///
/// It is "SIMD" in name only. The four probe slots come from triangular
/// (quadratic) probing, so they are scattered across up to four cache lines.
/// _mm256_set_epi64x compiles to four scalar loads plus two VPUNPCKLQDQ and a
/// VINSERTI128 to assemble the vector, then one VPCMPEQQ does the only
/// actual SIMD work. At a load factor <= 0.5 the key is usually in the
/// *first* slot, yet every lookup computes four probe addresses and loads four
/// slots.

#include <immintrin.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace legacy {

class QuadraticRefTable {
public:
    static constexpr uint64_t kEmpty = 0;
    static constexpr uint64_t kTombstone = UINT64_MAX;

    /// v1's OrderEntry: the order data lived in a second array parallel to the
    /// refs, so a real lookup touched order_refs[] and then order_map[].
    struct Entry {
        uint64_t ref;
        uint64_t price;
        uint32_t qty;
        uint8_t  side;
        uint8_t  pad_[3];
    };
    static_assert(sizeof(Entry) == 24);

    explicit QuadraticRefTable(unsigned log2_cap)
        : log2_cap_(log2_cap), cap_(std::size_t{1} << log2_cap), mask_(cap_ - 1),
          refs_(std::make_unique<uint64_t[]>(cap_)), entries_(std::make_unique<Entry[]>(cap_)) {}

    /// The order data for a slot returned by find_*().
    [[nodiscard]] const Entry& entry(std::size_t slot) const noexcept { return entries_[slot]; }

    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

    void insert(uint64_t ref) noexcept {
        const std::size_t start = probe_start(ref);
        for (std::size_t i = 0; i < cap_; ++i) {
            const std::size_t slot = (start + i * (i + 1) / 2) & mask_;
            if (refs_[slot] == kEmpty || refs_[slot] == kTombstone) {
                refs_[slot] = ref;
                entries_[slot] = Entry{ref, 0, static_cast<uint32_t>(ref), 'B', {0, 0, 0}};
                return;
            }
        }
    }

    void erase_slot(std::size_t slot) noexcept { refs_[slot] = kTombstone; }

#if defined(__AVX2__)
    /// Original AVX2 path.
    [[nodiscard]] std::size_t find_avx2(uint64_t ref) const noexcept {
        const std::size_t start = probe_start(ref);
        const __m256i vref   = _mm256_set1_epi64x(static_cast<int64_t>(ref));
        const __m256i empty4 = _mm256_set1_epi64x(static_cast<int64_t>(kEmpty));
        for (std::size_t i = 0; i < cap_; i += 4) {
            const std::size_t s0 = (start + (i + 0) * (i + 1) / 2) & mask_;
            const std::size_t s1 = (start + (i + 1) * (i + 2) / 2) & mask_;
            const std::size_t s2 = (start + (i + 2) * (i + 3) / 2) & mask_;
            const std::size_t s3 = (start + (i + 3) * (i + 4) / 2) & mask_;
            const __m256i slots = _mm256_set_epi64x(
                static_cast<int64_t>(refs_[s3]), static_cast<int64_t>(refs_[s2]),
                static_cast<int64_t>(refs_[s1]), static_cast<int64_t>(refs_[s0]));
            const __m256i hit   = _mm256_cmpeq_epi64(slots, vref);
            const __m256i empty = _mm256_cmpeq_epi64(slots, empty4);
            const int hit_mask   = _mm256_movemask_epi8(hit);
            const int empty_mask = _mm256_movemask_epi8(empty);
            if (__builtin_expect(hit_mask != 0, 0)) {
                const std::size_t lane =
                    static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(hit_mask))) / 8u;
                const std::size_t slots_arr[4] = {s0, s1, s2, s3};
                return slots_arr[lane];
            }
            if (empty_mask != 0) return cap_;
        }
        return cap_;
    }
#endif

    /// Original non-AVX2 path.
    [[nodiscard]] std::size_t find_scalar(uint64_t ref) const noexcept {
        const std::size_t start = probe_start(ref);
        for (std::size_t i = 0; i < cap_; ++i) {
            const std::size_t slot = (start + i * (i + 1) / 2) & mask_;
            if (refs_[slot] == ref) return slot;
            if (refs_[slot] == kEmpty) return cap_;
        }
        return cap_;
    }

private:
    [[nodiscard]] std::size_t probe_start(uint64_t ref) const noexcept {
        return static_cast<std::size_t>((ref * 11400714819323198485ULL) >> (64 - log2_cap_));
    }

    unsigned                    log2_cap_;
    std::size_t                 cap_;
    std::size_t                 mask_;
    std::unique_ptr<uint64_t[]> refs_;
    std::unique_ptr<Entry[]>    entries_;
};

} // namespace legacy
