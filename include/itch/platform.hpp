#pragma once

/// Platform, ISA and micro-architecture helpers shared by the whole library.
///
/// x86-64 only. ISA selection is compile-time, per binary:
///
///   default   -march=x86-64-v2  128-bit only: SSE2/SSSE3/SSE4.1/SSE4.2, POPCNT
///   *_avx2    -march=x86-64-v3  adds AVX2 (256-bit), BMI1/2, MOVBE, LZCNT
///
/// 128-bit is the default because of frequency licenses. On Intel server
/// parts, wide instructions can lower the core's frequency. Light 256-bit
/// integer instructions (the only kind these kernels use) are nominally free,
/// but the core still powers the upper vector lanes up and down; after an idle
/// gap the first burst pays a warm-up window of reduced wide-op throughput.
/// Both are sources of latency jitter, which is exactly what a feed handler
/// must avoid. The AVX2 build exists to be *measured* against the default
/// (bench/bm_burst.cpp, tools/license_check.sh), not to be assumed faster.
/// 512-bit AVX-512 is deliberately not used.
///
/// Every SIMD kernel keeps a scalar reference that the tests compare it with.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if !defined(__x86_64__)
#  error "x86-64 only. On Apple Silicon, build with -DCMAKE_OSX_ARCHITECTURES=x86_64 (runs under Rosetta 2, which executes AVX2)."
#endif

#include <cpuid.h>
#include <immintrin.h>
#include <x86intrin.h>  // __rdtsc, __rdtscp
#if defined(__APPLE__)
#  include <sys/sysctl.h>
#endif

#define ITCH_HAVE_SSE2 1  // x86-64 baseline
#if defined(__SSSE3__)
#  define ITCH_HAVE_SSSE3 1
#endif
#if defined(__SSE4_1__)
#  define ITCH_HAVE_SSE41 1
#endif
#if defined(__AVX2__)
#  define ITCH_HAVE_AVX2 1
#endif

#define ITCH_ALWAYS_INLINE inline __attribute__((always_inline))
#define ITCH_NOINLINE      __attribute__((noinline))
#define ITCH_COLD          __attribute__((cold, noinline))
#define ITCH_LIKELY(x)     __builtin_expect(!!(x), 1)
#define ITCH_UNLIKELY(x)   __builtin_expect(!!(x), 0)

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "wire decoding assumes a little-endian host");

namespace itch {

/// Padding unit used to keep independently-written data on separate lines.
/// 128 rather than 64: Intel's adjacent-line (spatial) prefetcher pulls 64-byte
/// lines in pairs, so 64-byte padding can still false-share under load. Same
/// choice as folly's hardware_destructive_interference_size.
inline constexpr std::size_t kFalseSharingRange = 128;

/// Natural cache-line size for data-layout decisions (SIMD loads, slot packing).
inline constexpr std::size_t kCacheLine = 64;

/// Spin-wait hint. PAUSE also avoids the memory-order machine clear when the
/// spin loop exits.
ITCH_ALWAYS_INLINE void cpu_relax() noexcept { _mm_pause(); }

/// Human-readable name of the SIMD level this translation unit was built for.
constexpr const char* simd_level_name() noexcept {
#if defined(ITCH_FORCE_SCALAR)
    return "scalar";
#elif defined(ITCH_HAVE_AVX2)
    return "AVX2";
#elif defined(ITCH_HAVE_SSE41)
    return "SSE4.1";
#elif defined(ITCH_HAVE_SSSE3)
    return "SSSE3";
#else
    return "SSE2";
#endif
}

/// True when this x86-64 process is being translated by Rosetta 2. Rosetta
/// executes AVX2 but does not advertise it (or BMI1/2) through CPUID, so
/// CPUID-based feature checks are wrong there.
inline bool running_under_rosetta() noexcept {
#if defined(__APPLE__)
    int translated = 0;
    std::size_t size = sizeof translated;
    return ::sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 && translated == 1;
#else
    return false;
#endif
}

/// Instruction-set extensions this binary was compiled to use that the
/// running CPU lacks, as a space-separated list ("" = OK). Call it first
/// thing in main(): running an x86-64-v3 build on an older CPU otherwise dies
/// with SIGILL somewhere unhelpful. See check_cpu_or_exit() for the policy.
inline const char* missing_cpu_features() noexcept {
    static char buf[128];
    buf[0] = '\0';
    const auto need = [](bool have, const char* name) {
        if (!have) {
            __builtin_strncat(buf, name, sizeof(buf) - __builtin_strlen(buf) - 1);
            __builtin_strncat(buf, " ", sizeof(buf) - __builtin_strlen(buf) - 1);
        }
    };
    __builtin_cpu_init();
#if defined(__SSSE3__)
    need(__builtin_cpu_supports("ssse3"), "ssse3");
#endif
#if defined(__SSE4_1__)
    need(__builtin_cpu_supports("sse4.1"), "sse4.1");
#endif
#if defined(__SSE4_2__)
    need(__builtin_cpu_supports("sse4.2"), "sse4.2");
#endif
#if defined(__POPCNT__)
    need(__builtin_cpu_supports("popcnt"), "popcnt");
#endif
#if defined(__AVX2__)
    need(__builtin_cpu_supports("avx2"), "avx2");
#endif
#if defined(__BMI2__)
    need(__builtin_cpu_supports("bmi2"), "bmi2");
#endif
    // Compilers disagree on which names __builtin_cpu_supports accepts
    // (Clang 18 rejects "movbe" and "lzcnt"), so read these bits from CPUID.
    [[maybe_unused]] const auto cpuid_bit = [](unsigned leaf, unsigned sub, int reg, unsigned bit) {
        unsigned r[4] = {0, 0, 0, 0};  // eax, ebx, ecx, edx
        if (__get_cpuid_count(leaf, sub, &r[0], &r[1], &r[2], &r[3]) == 0) return false;
        return ((r[reg] >> bit) & 1u) != 0;
    };
    [[maybe_unused]] constexpr int kEbx = 1, kEcx = 2;
#if defined(__BMI__)
    need(cpuid_bit(7, 0, kEbx, 3), "bmi");
#endif
#if defined(__MOVBE__)
    need(cpuid_bit(1, 0, kEcx, 22), "movbe");
#endif
#if defined(__LZCNT__)
    need(cpuid_bit(0x80000001u, 0, kEcx, 5), "lzcnt");  // ABM
#endif
#if defined(__FMA__)
    need(cpuid_bit(1, 0, kEcx, 12), "fma");
#endif
#if defined(__F16C__)
    need(cpuid_bit(1, 0, kEcx, 29), "f16c");
#endif
    (void)need;  // plain x86-64 builds check nothing beyond the baseline
    return buf;
}

/// Refuse to start on a CPU lacking the features this binary was built for.
/// Skipped under Rosetta (see running_under_rosetta) and when the
/// environment sets ITCH_SKIP_CPU_CHECK=1.
inline void check_cpu_or_exit() noexcept {
    const char* missing = missing_cpu_features();
    if (missing[0] == '\0') return;
    if (running_under_rosetta()) {
        std::fprintf(stderr, "note: Rosetta 2 hides %sfrom CPUID but executes them; continuing\n", missing);
        return;
    }
    const char* skip = std::getenv("ITCH_SKIP_CPU_CHECK");
    if (skip != nullptr && skip[0] == '1') return;
    std::fprintf(stderr,
                 "this binary was built for CPU features this machine lacks: %s\n"
                 "(use the default x86-64-v2 build, or set ITCH_SKIP_CPU_CHECK=1 if CPUID is wrong)\n",
                 missing);
    std::exit(2);
}

} // namespace itch
