#pragma once

/// Swiss-table control-byte groups: the SIMD core of the order map.
///
/// Each hash-table slot has a 1-byte control tag:
///   0x00..0x7F  FULL:    the slot holds a key; the low 7 bits are the key's
///               hash fragment, "H2"
///   0xFF        EMPTY:   never used since the last rehash; ends a probe
///   0x80        DELETED: tombstone; probing continues past it
/// (the same encoding hashbrown uses). A group is kWidth adjacent control
/// bytes, so one compare checks every slot in it:
///
///   match(h2)               -> slots whose tag equals h2 (candidate hits)
///   match_empty()           -> slots that are EMPTY (probe can stop)
///   match_empty_or_deleted()-> slots an insert may claim
///
/// The result is a BitMask with one set bit per matching slot; walk it with
/// countr_zero and clear the lowest bit each step. How wide one slot is in the
/// mask depends on the ISA (the Shift parameter):
///
///   AVX2   32 slots, 1 bit/slot  (vpcmpeqb + vpmovmskb)
///   SSE2   16 slots, 1 bit/slot  (pcmpeqb  + pmovmskb)
///   SWAR    8 slots, 8 bits/slot (plain uint64 arithmetic, any CPU)
///
/// Compare the legacy find_order (bench/legacy/legacy_find_order.hpp): it
/// gathered four *scattered* 8-byte keys into one vector, which costs four
/// scalar loads from four cache lines per step. Here the tags are contiguous,
/// so one aligned load covers the whole group.

#include "itch/platform.hpp"

#include <bit>
#include <cstdint>
#include <cstring>

namespace itch::simd {

inline constexpr uint8_t kCtrlEmpty   = 0xFF;
inline constexpr uint8_t kCtrlDeleted = 0x80;

/// Set bits of a group-match result. Shift = log2(bits per slot).
template <int Shift>
class BitMask {
public:
    constexpr explicit BitMask(uint64_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] constexpr bool any() const noexcept { return bits_ != 0; }
    [[nodiscard]] constexpr uint32_t lowest() const noexcept {
        return static_cast<uint32_t>(std::countr_zero(bits_)) >> Shift;
    }
    constexpr void clear_lowest() noexcept { bits_ &= bits_ - 1; }

    // Range-for support: for (uint32_t i : mask) { ... }
    struct Iter {
        uint64_t bits;
        constexpr uint32_t operator*() const noexcept {
            return static_cast<uint32_t>(std::countr_zero(bits)) >> Shift;
        }
        constexpr Iter& operator++() noexcept { bits &= bits - 1; return *this; }
        constexpr bool operator!=(const Iter& o) const noexcept { return bits != o.bits; }
    };
    [[nodiscard]] constexpr Iter begin() const noexcept { return {bits_}; }
    [[nodiscard]] constexpr Iter end() const noexcept { return {0}; }

private:
    uint64_t bits_;
};

// ---------------------------------------------------------------------------
// SWAR: 8 tags packed into a uint64_t. Works on any CPU, so it doubles as the
// portable fallback.
// ---------------------------------------------------------------------------

struct GroupSwar {
    static constexpr uint32_t kWidth = 8;
    using Mask = BitMask<3>;
    static constexpr uint64_t kLsbs = 0x0101'0101'0101'0101ULL;
    static constexpr uint64_t kMsbs = 0x8080'8080'8080'8080ULL;

    uint64_t ctrl;

    ITCH_ALWAYS_INLINE explicit GroupSwar(const uint8_t* p) noexcept { std::memcpy(&ctrl, p, 8); }

    /// Classic "has zero byte" test on ctrl ^ broadcast(h2). A borrow can set a
    /// false-positive bit in the byte just above a true match; the caller
    /// compares the full key on every candidate, so a false positive costs one
    /// compare and never produces a wrong result.
    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match(uint8_t h2) const noexcept {
        const uint64_t x = ctrl ^ (kLsbs * h2);
        return Mask((x - kLsbs) & ~x & kMsbs);
    }
    /// Exact: EMPTY (0xFF) is the only tag with both bit 7 and bit 6 set
    /// (DELETED is 0x80 and FULL tags have bit 7 clear).
    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match_empty() const noexcept {
        return Mask(ctrl & (ctrl << 1) & kMsbs);
    }
    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match_empty_or_deleted() const noexcept {
        return Mask(ctrl & kMsbs);
    }
};

// ---------------------------------------------------------------------------
// SSE2: 16 tags, pcmpeqb + pmovmskb. EMPTY and DELETED both have the sign bit
// set, so match_empty_or_deleted is a single pmovmskb with no compare.
// ---------------------------------------------------------------------------

#if defined(ITCH_HAVE_SSE2)
struct GroupSse2 {
    static constexpr uint32_t kWidth = 16;
    using Mask = BitMask<0>;

    __m128i ctrl;

    ITCH_ALWAYS_INLINE explicit GroupSse2(const uint8_t* p) noexcept
        : ctrl(_mm_load_si128(reinterpret_cast<const __m128i*>(p))) {}

    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match(uint8_t h2) const noexcept {
        const __m128i eq = _mm_cmpeq_epi8(ctrl, _mm_set1_epi8(static_cast<char>(h2)));
        return Mask(static_cast<uint32_t>(_mm_movemask_epi8(eq)));
    }
    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match_empty() const noexcept {
        const __m128i eq = _mm_cmpeq_epi8(ctrl, _mm_set1_epi8(static_cast<char>(kCtrlEmpty)));
        return Mask(static_cast<uint32_t>(_mm_movemask_epi8(eq)));
    }
    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match_empty_or_deleted() const noexcept {
        return Mask(static_cast<uint32_t>(_mm_movemask_epi8(ctrl)));
    }
};
#endif

// ---------------------------------------------------------------------------
// AVX2: 32 tags. The group is 32 bytes, so a probe step covers twice as many
// slots and at a 7/8 load factor almost every lookup finishes in its first
// group.
// ---------------------------------------------------------------------------

#if defined(ITCH_HAVE_AVX2)
struct GroupAvx2 {
    static constexpr uint32_t kWidth = 32;
    using Mask = BitMask<0>;

    __m256i ctrl;

    ITCH_ALWAYS_INLINE explicit GroupAvx2(const uint8_t* p) noexcept
        : ctrl(_mm256_load_si256(reinterpret_cast<const __m256i*>(p))) {}

    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match(uint8_t h2) const noexcept {
        const __m256i eq = _mm256_cmpeq_epi8(ctrl, _mm256_set1_epi8(static_cast<char>(h2)));
        return Mask(static_cast<uint32_t>(_mm256_movemask_epi8(eq)));
    }
    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match_empty() const noexcept {
        const __m256i eq = _mm256_cmpeq_epi8(ctrl, _mm256_set1_epi8(static_cast<char>(kCtrlEmpty)));
        return Mask(static_cast<uint32_t>(_mm256_movemask_epi8(eq)));
    }
    [[nodiscard]] ITCH_ALWAYS_INLINE Mask match_empty_or_deleted() const noexcept {
        return Mask(static_cast<uint32_t>(_mm256_movemask_epi8(ctrl)));
    }
};
#endif

// ---------------------------------------------------------------------------
// Best group for this build
// ---------------------------------------------------------------------------

#if defined(ITCH_FORCE_SCALAR)
using GroupBest = GroupSwar;
#elif defined(ITCH_HAVE_AVX2)
using GroupBest = GroupAvx2;
#elif defined(ITCH_HAVE_SSE2)
using GroupBest = GroupSse2;
#else
using GroupBest = GroupSwar;
#endif

} // namespace itch::simd
