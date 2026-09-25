#pragma once

/// Decode every book-relevant field of an ITCH Add Order ('A', 36 bytes) with
/// byte shuffles instead of per-field big-endian loads.
///
/// Wire layout (big-endian)       ->  AddOrderFields (little-endian, 32 bytes)
///   locate    @1   2                  ref        @0   8
///   tracking  @3   2                  timestamp  @8   8  (48-bit, zero-extended)
///   timestamp @5   6                  shares     @16  4
///   ref       @11  8                  price      @20  4
///   side      @19  1                  locate     @24  2
///   shares    @20  4                  tracking   @26  2
///   stock     @24  8  (not needed)    side       @28  1
///   price     @32  4
///
/// Each output byte is one input byte, or zero, so the whole decode is a
/// byte permutation: exactly what PSHUFB does. The catch is that PSHUFB
/// indexes only within one 16-byte lane, and even the 256-bit VPSHUFB is two
/// independent 128-bit shuffles, so no output byte can come from the other
/// lane. The fields here straddle 16-byte boundaries (ref is bytes 11..18;
/// price is 32..35), so a single shuffle can't do it:
///
///   SSSE3:  split the input into 16-byte chunks c0 c1 c2; each output half is
///           the OR of per-chunk shuffles whose masks zero every byte that
///           belongs to another chunk (mask bit 7 set -> PSHUFB writes 0).
///           5 x PSHUFB + 3 x POR.
///   AVX2:   broadcast each chunk into *both* 128-bit lanes (VBROADCASTI128 is
///           a plain load uop), so lane 0 builds the low output half and lane 1
///           the high half in parallel: 3 x VPSHUFB + 2 x VPOR, 1 store.
///
/// Precondition for the SIMD variants: 48 bytes are readable from `msg` (the
/// 36-byte message plus 12 bytes of over-read). Inside a stream buffer the next
/// message follows; callers use the scalar path for the last message.
///
/// Whether this beats the scalar path (seven MOVBE-class loads the compiler
/// already schedules well) is an empirical question; bench/bm_kernels.cpp
/// measures it. The SSSE3 version is the one the default 128-bit build uses.

#include "itch/bytes.hpp"
#include "itch/platform.hpp"

#include <array>
#include <cstdint>
#include <cstring>

namespace itch::simd {

struct alignas(32) AddOrderFields {
    uint64_t ref;
    uint64_t timestamp;
    uint32_t shares;
    uint32_t price;
    uint16_t locate;
    uint16_t tracking;
    char     side;
    uint8_t  zero_[3];

    friend bool operator==(const AddOrderFields&, const AddOrderFields&) = default;
};
static_assert(sizeof(AddOrderFields) == 32);

inline constexpr std::size_t kAddOrderOverread = 48;

namespace detail {

/// kAddOrderSrc[k] = wire byte feeding output byte k, or -1 for zero.
inline constexpr std::array<int, 32> kAddOrderSrc = {
    18, 17, 16, 15, 14, 13, 12, 11,   // ref       <- wire 11..18 reversed
    10,  9,  8,  7,  6,  5, -1, -1,   // timestamp <- wire 5..10 reversed, zero-extended
    23, 22, 21, 20,                   // shares    <- wire 20..23 reversed
    35, 34, 33, 32,                   // price     <- wire 32..35 reversed
     2,  1,                           // locate    <- wire 1..2 reversed
     4,  3,                           // tracking  <- wire 3..4 reversed
    19,                               // side
    -1, -1, -1};

/// PSHUFB control for input chunk `chunk` producing output bytes
/// [out_base, out_base+16): selects bytes that live in that chunk and zeroes
/// (0x80) everything else.
consteval std::array<int8_t, 16> shuffle_mask(int chunk, int out_base) {
    std::array<int8_t, 16> m{};
    for (int k = 0; k < 16; ++k) {
        const int src = kAddOrderSrc[static_cast<std::size_t>(out_base + k)];
        m[static_cast<std::size_t>(k)] =
            (src >= 0 && src / 16 == chunk) ? static_cast<int8_t>(src % 16) : static_cast<int8_t>(-128);
    }
    return m;
}

template <int Chunk, int OutBase>
inline constexpr std::array<int8_t, 16> kMask = shuffle_mask(Chunk, OutBase);

/// 32-byte VPSHUFB control: lane 0 builds output bytes 0..15, lane 1 builds 16..31.
template <int Chunk>
inline constexpr std::array<int8_t, 32> kMask256 = [] {
    std::array<int8_t, 32> m{};
    for (std::size_t k = 0; k < 16; ++k) {
        m[k]      = kMask<Chunk, 0>[k];
        m[16 + k] = kMask<Chunk, 16>[k];
    }
    return m;
}();

} // namespace detail

namespace scalar {

inline AddOrderFields decode_add_order(const uint8_t* msg) noexcept {
    AddOrderFields f{};
    f.ref       = load_be64(msg + 11);
    f.timestamp = load_be48_overread(msg + 5);
    f.shares    = load_be32(msg + 20);
    f.price     = load_be32(msg + 32);
    f.locate    = load_be16(msg + 1);
    f.tracking  = load_be16(msg + 3);
    f.side      = static_cast<char>(msg[19]);
    return f;
}

} // namespace scalar

#if defined(ITCH_HAVE_SSSE3)
namespace ssse3 {

inline AddOrderFields decode_add_order(const uint8_t* msg) noexcept {
    using detail::kMask;
    const auto ld = [](const auto& m) { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(m.data())); };
    const __m128i c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(msg));
    const __m128i c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(msg + 16));
    const __m128i c2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(msg + 32));
    // Bytes 0..15 (ref, timestamp) come only from chunks 0 and 1.
    const __m128i lo = _mm_or_si128(_mm_shuffle_epi8(c0, ld(kMask<0, 0>)),
                                    _mm_shuffle_epi8(c1, ld(kMask<1, 0>)));
    // Bytes 16..31 (shares, price, locate, tracking, side) need all three.
    const __m128i hi = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(c0, ld(kMask<0, 16>)),
                                                 _mm_shuffle_epi8(c1, ld(kMask<1, 16>))),
                                    _mm_shuffle_epi8(c2, ld(kMask<2, 16>)));
    AddOrderFields f;
    _mm_store_si128(reinterpret_cast<__m128i*>(&f), lo);
    _mm_store_si128(reinterpret_cast<__m128i*>(&f) + 1, hi);
    return f;
}

} // namespace ssse3
#endif

#if defined(ITCH_HAVE_AVX2)
namespace avx2 {

inline AddOrderFields decode_add_order(const uint8_t* msg) noexcept {
    using detail::kMask256;
    const auto ld = [](const auto& m) { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(m.data())); };
    // Same 16 bytes in both lanes, so each lane can pick any byte of the chunk.
    const __m256i c0 = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(msg)));
    const __m256i c1 = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(msg + 16)));
    const __m256i c2 = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(msg + 32)));
    const __m256i out = _mm256_or_si256(_mm256_or_si256(_mm256_shuffle_epi8(c0, ld(kMask256<0>)),
                                                        _mm256_shuffle_epi8(c1, ld(kMask256<1>))),
                                        _mm256_shuffle_epi8(c2, ld(kMask256<2>)));
    AddOrderFields f;
    _mm256_store_si256(reinterpret_cast<__m256i*>(&f), out);
    return f;
}

} // namespace avx2
#endif

} // namespace itch::simd
