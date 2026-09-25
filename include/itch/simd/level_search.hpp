#pragma once

/// Price-level search: lower_bound over a sorted uint32_t key array, scanning
/// from the *back*.
///
/// Book layout (see order_book.hpp): each side is an ascending array of keys
/// with the best price at the back. Bids use key = price and asks use
/// key = ~price, so both sides share one search routine. Real order flow is
/// concentrated at the top of the book, so the target is almost always within
/// the last few elements. A linear SIMD scan from the back finds it in one or
/// two compares; a binary search would pay log2(n) dependent,
/// hard-to-predict branches.
///
/// Every variant returns the same value as std::lower_bound(keys, keys+n, key):
/// the first index i with keys[i] >= key.
///
/// Trick: in a sorted chunk, the lanes with keys[j] < key form a prefix, so
/// the insertion point is chunk_base + popcount(lt_mask). No per-lane
/// branching, no scalar tail loop.
///
/// Precondition for the SIMD variants: the allocation behind `keys` holds at
/// least kLevelSearchPad elements, even when n is smaller. The tail is then
/// handled with one full-width load from keys[0] whose out-of-range lanes are
/// masked off. Lanes past n are never used for the result.

#include "itch/platform.hpp"

#include <algorithm>
#include <cstdint>

namespace itch::simd {

/// Elements examined per chunk, and the minimum readable length of `keys`.
inline constexpr uint32_t kLevelSearchPad = 8;

/// Chunks scanned from the back before switching to binary search, so the
/// worst case for a deep level stays logarithmic. Set with the CMake option
/// ITCH_LEVEL_LINEAR_CHUNKS; pick it from `feed_handler --depth-profile`
/// on real data (the smallest value covering ~99% of level operations).
#if !defined(ITCH_LEVEL_LINEAR_CHUNKS)
#  define ITCH_LEVEL_LINEAR_CHUNKS 4
#endif
inline constexpr uint32_t kLinearChunks = ITCH_LEVEL_LINEAR_CHUNKS;

namespace detail {

ITCH_ALWAYS_INLINE uint32_t popcount8(uint32_t m) noexcept {
    return static_cast<uint32_t>(__builtin_popcount(m));
}

/// Branchless lower_bound (Khuong & Morin, "Array layouts for comparison-based
/// searching"). The loop body compiles to a CMOV, so the only branch is the
/// loop counter, which is predictable.
ITCH_ALWAYS_INLINE uint32_t lower_bound_branchless(const uint32_t* a, uint32_t n,
                                                   uint32_t key) noexcept {
    const uint32_t* base = a;
    while (n > 1) {
        const uint32_t half = n / 2;
        base = (base[half] < key) ? base + half : base;
        n -= half;
    }
    return static_cast<uint32_t>(base - a) + static_cast<uint32_t>(n == 1 && *base < key);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Scalar variants: the reference and the scalar form of the back-scan strategy
// ---------------------------------------------------------------------------

namespace scalar {

/// Reference implementation. Tests compare every other variant against this.
inline uint32_t lower_bound(const uint32_t* keys, uint32_t n, uint32_t key) noexcept {
    return static_cast<uint32_t>(std::lower_bound(keys, keys + n, key) - keys);
}

/// Branchless binary search over the whole array.
inline uint32_t lower_bound_branchless(const uint32_t* keys, uint32_t n,
                                       uint32_t key) noexcept {
    return detail::lower_bound_branchless(keys, n, key);
}

/// Linear scan from the back, then binary search: same strategy as the SIMD
/// variants, one element at a time.
template <uint32_t Chunks = kLinearChunks>
inline uint32_t lower_bound_from_back(const uint32_t* keys, uint32_t n,
                                      uint32_t key) noexcept {
    uint32_t i = n;
    const uint32_t stop = (n > Chunks * kLevelSearchPad) ? n - Chunks * kLevelSearchPad : 0;
    while (i > stop && keys[i - 1] >= key) --i;
    if (i > stop || i == 0) return i;
    return detail::lower_bound_branchless(keys, i, key);
}

} // namespace scalar

// ---------------------------------------------------------------------------
// SSE2: 2 x 4 lanes. SSE2 has no unsigned 32-bit compare, so flip the sign bit
// of both operands and use the signed compare: (a ^ 0x80000000) < (b ^ 0x80000000)
// as signed  <=>  a < b as unsigned. ITCH prices fit in 31 bits, but asks are
// stored as ~price, which sets the top bit, so the unsigned compare is required.
// ---------------------------------------------------------------------------

#if defined(ITCH_HAVE_SSE2)
namespace sse2 {

/// 8-bit mask of lanes in keys[base .. base+8) that are < key.
ITCH_ALWAYS_INLINE uint32_t lt_mask8(const uint32_t* p, __m128i key_biased) noexcept {
    const __m128i bias = _mm_set1_epi32(static_cast<int>(0x8000'0000u));
    const __m128i lo = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), bias);
    const __m128i hi = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 4)), bias);
    const auto m_lo = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(key_biased, lo))));
    const auto m_hi = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(key_biased, hi))));
    return m_lo | (m_hi << 4);
}

template <uint32_t Chunks = kLinearChunks>
inline uint32_t lower_bound_from_back(const uint32_t* keys, uint32_t n, uint32_t key) noexcept {
    const __m128i kb = _mm_set1_epi32(static_cast<int>(key ^ 0x8000'0000u));
    uint32_t i = n;
    for (uint32_t c = 0; c < Chunks && i >= kLevelSearchPad; ++c) {
        const uint32_t lt = lt_mask8(keys + i - 8, kb);
        if (lt != 0) return i - 8 + detail::popcount8(lt);
        i -= 8;
    }
    if (i >= kLevelSearchPad) return detail::lower_bound_branchless(keys, i, key);
    const uint32_t lt = lt_mask8(keys, kb) & ((1u << i) - 1u);
    return detail::popcount8(lt);
}

} // namespace sse2
#endif

// ---------------------------------------------------------------------------
// SSE4.1: 2 x 4 lanes with the unsigned max trick (pmaxud, new in SSE4.1):
// (max_epu32(v, k) == v)  <=>  v >= k. This is the default 128-bit kernel:
// same instruction count per lane as AVX2, no wide-register frequency risk.
// ---------------------------------------------------------------------------

#if defined(ITCH_HAVE_SSE41)
namespace sse41 {

/// 8-bit mask of lanes in p[0..8) that are >= key.
ITCH_ALWAYS_INLINE uint32_t ge_mask8(const uint32_t* p, __m128i k) noexcept {
    const __m128i lo = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    const __m128i hi = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 4));
    const auto m_lo = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_max_epu32(lo, k), lo))));
    const auto m_hi = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_max_epu32(hi, k), hi))));
    return m_lo | (m_hi << 4);
}

template <uint32_t Chunks = kLinearChunks>
inline uint32_t lower_bound_from_back(const uint32_t* keys, uint32_t n, uint32_t key) noexcept {
    const __m128i k = _mm_set1_epi32(static_cast<int>(key));
    uint32_t i = n;
    for (uint32_t c = 0; c < Chunks && i >= kLevelSearchPad; ++c) {
        const uint32_t ge = ge_mask8(keys + i - 8, k);
        if (ge != 0xFFu) return i - detail::popcount8(ge);
        i -= 8;
    }
    if (i >= kLevelSearchPad) return detail::lower_bound_branchless(keys, i, key);
    const uint32_t ge = ge_mask8(keys, k) & ((1u << i) - 1u);
    return i - detail::popcount8(ge);
}

} // namespace sse41
#endif

// ---------------------------------------------------------------------------
// AVX2: 8 lanes in one register, same unsigned-max trick as SSE4.1. Opt-in
// (x86-64-v3 build); see platform.hpp on frequency licenses.
// ---------------------------------------------------------------------------

#if defined(ITCH_HAVE_AVX2)
namespace avx2 {

/// 8-bit mask of lanes in p[0..8) that are >= key.
ITCH_ALWAYS_INLINE uint32_t ge_mask8(const uint32_t* p, __m256i k) noexcept {
    const __m256i v  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i ge = _mm256_cmpeq_epi32(_mm256_max_epu32(v, k), v);
    return static_cast<uint32_t>(_mm256_movemask_ps(_mm256_castsi256_ps(ge)));
}

template <uint32_t Chunks = kLinearChunks>
inline uint32_t lower_bound_from_back(const uint32_t* keys, uint32_t n, uint32_t key) noexcept {
    const __m256i k = _mm256_set1_epi32(static_cast<int>(key));
    uint32_t i = n;
    for (uint32_t c = 0; c < Chunks && i >= kLevelSearchPad; ++c) {
        const uint32_t ge = ge_mask8(keys + i - 8, k);
        if (ge != 0xFFu) return i - detail::popcount8(ge);
        i -= 8;
    }
    if (i >= kLevelSearchPad) return detail::lower_bound_branchless(keys, i, key);
    const uint32_t ge = ge_mask8(keys, k) & ((1u << i) - 1u);
    return i - detail::popcount8(ge);
}

} // namespace avx2
#endif

// ---------------------------------------------------------------------------
// Best available variant for this build
// ---------------------------------------------------------------------------

ITCH_ALWAYS_INLINE uint32_t lower_bound_from_back(const uint32_t* keys, uint32_t n,
                                                  uint32_t key) noexcept {
#if defined(ITCH_FORCE_SCALAR)
    return scalar::lower_bound_from_back(keys, n, key);
#elif defined(ITCH_HAVE_AVX2)
    return avx2::lower_bound_from_back(keys, n, key);
#elif defined(ITCH_HAVE_SSE41)
    return sse41::lower_bound_from_back(keys, n, key);
#elif defined(ITCH_HAVE_SSE2)
    return sse2::lower_bound_from_back(keys, n, key);
#else
    return scalar::lower_bound_from_back(keys, n, key);
#endif
}

} // namespace itch::simd
