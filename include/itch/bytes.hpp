#pragma once

/// Big-endian load/store helpers for ITCH wire fields.
///
/// No intrinsics needed here: memcpy + __builtin_bswap is recognised by both
/// GCC and Clang and lowers to MOV + BSWAP in the default x86-64-v2 build, or
/// to a single MOVBE where MOVBE is available (x86-64-v3, the *_avx2 build).
/// Writing these by hand with intrinsics would only make them less portable.

#include "itch/platform.hpp"

#include <cstdint>
#include <cstring>

namespace itch {

ITCH_ALWAYS_INLINE uint16_t load_be16(const uint8_t* p) noexcept {
    uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap16(v);
}

ITCH_ALWAYS_INLINE uint32_t load_be32(const uint8_t* p) noexcept {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap32(v);
}

ITCH_ALWAYS_INLINE uint64_t load_be64(const uint8_t* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap64(v);
}

/// 48-bit big-endian integer (ITCH timestamps).
///
/// Reads the 8 bytes ending at p+6 (i.e. starting 2 bytes *before* the field)
/// and masks off the two leading bytes: one 8-byte byte-swapped load (MOV +
/// BSWAP, or MOVBE) plus one AND, instead of a 4-byte + 2-byte
/// load-and-merge. Precondition: p-2 is
/// readable. The timestamp sits at offset 5 in every ITCH message, so the
/// over-read covers bytes 3..10, which are always inside the message.
ITCH_ALWAYS_INLINE uint64_t load_be48_overread(const uint8_t* p) noexcept {
    return load_be64(p - 2) & 0x0000'FFFF'FFFF'FFFFULL;
}

/// 48-bit big-endian integer without the over-read precondition.
ITCH_ALWAYS_INLINE uint64_t load_be48(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(load_be16(p)) << 32) | load_be32(p + 2);
}

ITCH_ALWAYS_INLINE void store_be16(uint8_t* p, uint16_t v) noexcept {
    v = __builtin_bswap16(v);
    std::memcpy(p, &v, sizeof v);
}

ITCH_ALWAYS_INLINE void store_be32(uint8_t* p, uint32_t v) noexcept {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, sizeof v);
}

ITCH_ALWAYS_INLINE void store_be64(uint8_t* p, uint64_t v) noexcept {
    v = __builtin_bswap64(v);
    std::memcpy(p, &v, sizeof v);
}

ITCH_ALWAYS_INLINE void store_be48(uint8_t* p, uint64_t v) noexcept {
    store_be16(p, static_cast<uint16_t>(v >> 32));
    store_be32(p + 2, static_cast<uint32_t>(v));
}

} // namespace itch
