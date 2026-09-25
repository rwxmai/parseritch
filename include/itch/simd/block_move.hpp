#pragma once

/// Overlapping element shifts for the level arrays, 128-bit only.
///
/// A level insert shifts the elements above the insertion point up by one; an
/// erase shifts them down by one. std::memmove would do it, but glibc chooses
/// its memmove implementation at run time from the CPU's features, and on
/// AVX2 or AVX-512 hardware that is a 256- or 512-bit copy loop, whatever
/// -march this code was built with. That would put wide instructions (and
/// their frequency-license effects, see platform.hpp) back on the hot path.
/// These moves use SSE2 16-byte loads and stores and are always inlined.
///
/// Overlap is safe because each direction reads a block before any write can
/// reach it: shifting up (dst > src) walks from the top down, shifting down
/// (dst < src) walks from the bottom up.

#include "itch/platform.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itch::simd {

namespace detail {

// Every loaded value passes through an empty asm with a register constraint
// ("+x" for an XMM register, "+r" for a GPR). It emits no instruction, but the
// compiler can no longer see a plain load->store copy, so loop idiom
// recognition can't rewrite these loops as a call to memmove: the libc
// routine this file exists to avoid. Unlike a "memory" clobber, it doesn't
// stop the compiler from scheduling the loads and stores freely.
ITCH_ALWAYS_INLINE void opaque(__m128i& v) noexcept { __asm__("" : "+x"(v)); }
template <class T>
ITCH_ALWAYS_INLINE void opaque(T& v) noexcept { __asm__("" : "+r"(v)); }

/// Move `n` bytes (a multiple of 4) from src to dst, dst > src (overlap allowed).
ITCH_ALWAYS_INLINE void move_up(uint8_t* dst, const uint8_t* src, std::size_t n) noexcept {
    std::size_t i = n;
    while (i >= 16) {
        i -= 16;
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        opaque(v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), v);
    }
    // 0, 4, 8 or 12 bytes left at the bottom.
    if (i >= 8) {
        i -= 8;
        uint64_t w;
        std::memcpy(&w, src + i, 8);
        opaque(w);
        std::memcpy(dst + i, &w, 8);
    }
    if (i >= 4) {
        uint32_t w;
        std::memcpy(&w, src, 4);
        opaque(w);
        std::memcpy(dst, &w, 4);
    }
}

/// Move `n` bytes (a multiple of 4) from src to dst, dst < src (overlap allowed).
ITCH_ALWAYS_INLINE void move_down(uint8_t* dst, const uint8_t* src, std::size_t n) noexcept {
    std::size_t i = 0;
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        opaque(v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), v);
        i += 16;
    }
    if (i + 8 <= n) {
        uint64_t w;
        std::memcpy(&w, src + i, 8);
        opaque(w);
        std::memcpy(dst + i, &w, 8);
        i += 8;
    }
    if (i + 4 <= n) {
        uint32_t w;
        std::memcpy(&w, src + i, 4);
        opaque(w);
        std::memcpy(dst + i, &w, 4);
    }
}

} // namespace detail

/// a[pos .. pos+count) -> a[pos+1 .. pos+count+1)
template <class T>
ITCH_ALWAYS_INLINE void shift_up(T* a, uint32_t pos, uint32_t count) noexcept {
    static_assert(sizeof(T) % 4 == 0);
    detail::move_up(reinterpret_cast<uint8_t*>(a + pos + 1),
                    reinterpret_cast<const uint8_t*>(a + pos), count * sizeof(T));
}

/// a[pos+1 .. pos+count+1) -> a[pos .. pos+count)
template <class T>
ITCH_ALWAYS_INLINE void shift_down(T* a, uint32_t pos, uint32_t count) noexcept {
    static_assert(sizeof(T) % 4 == 0);
    detail::move_down(reinterpret_cast<uint8_t*>(a + pos),
                      reinterpret_cast<const uint8_t*>(a + pos + 1), count * sizeof(T));
}

} // namespace itch::simd
