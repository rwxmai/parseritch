#include "itch/simd/level_search.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace {

using SearchFn = uint32_t (*)(const uint32_t*, uint32_t, uint32_t);

struct Variant {
    const char* name;
    SearchFn    fn;
};

const std::vector<Variant>& variants() {
    namespace s = itch::simd;
    static const std::vector<Variant> v = {
        {"scalar_branchless", &s::scalar::lower_bound_branchless},
        {"scalar_from_back", &s::scalar::lower_bound_from_back},
        {"sse2", &s::sse2::lower_bound_from_back},
#if defined(ITCH_HAVE_SSE41)
        {"sse41", &s::sse41::lower_bound_from_back},
#endif
#if defined(ITCH_HAVE_AVX2)
        {"avx2", &s::avx2::lower_bound_from_back},
#endif
        {"dispatch", &s::lower_bound_from_back},
    };
    return v;
}

/// Sorted keys in a buffer padded to at least kLevelSearchPad; the padding
/// holds garbage so any unmasked lane would corrupt the result.
std::vector<uint32_t> padded(std::vector<uint32_t> keys, std::mt19937& rng) {
    const std::size_t n = keys.size();
    keys.resize(std::max<std::size_t>(n, itch::simd::kLevelSearchPad) + 8);
    for (std::size_t i = n; i < keys.size(); ++i) keys[i] = static_cast<uint32_t>(rng());
    return keys;
}

void check_all(const std::vector<uint32_t>& buf, uint32_t n, uint32_t key) {
    const uint32_t expect = itch::simd::scalar::lower_bound(buf.data(), n, key);
    for (const auto& v : variants()) {
        ASSERT_EQ(v.fn(buf.data(), n, key), expect) << v.name << " n=" << n << " key=" << key;
    }
}

std::vector<uint32_t> probes_for(const std::vector<uint32_t>& sorted, std::mt19937& rng) {
    std::vector<uint32_t> probes = {0u, 1u, 0x7FFF'FFFFu, 0x8000'0000u, 0x8000'0001u, 0xFFFF'FFFEu, 0xFFFF'FFFFu};
    for (uint32_t k : sorted) {
        probes.push_back(k);
        probes.push_back(k + 1);
        probes.push_back(k - 1);
    }
    for (int i = 0; i < 16; ++i) probes.push_back(static_cast<uint32_t>(rng()));
    return probes;
}

TEST(LevelSearch, MatchesStdLowerBoundForAllSizes) {
    std::mt19937 rng(42);
    for (uint32_t n = 0; n <= 130; ++n) {
        for (int trial = 0; trial < 4; ++trial) {
            std::vector<uint32_t> keys(n);
            for (auto& k : keys) k = static_cast<uint32_t>(rng());
            std::sort(keys.begin(), keys.end());
            const auto buf = padded(keys, rng);
            for (uint32_t probe : probes_for(keys, rng)) check_all(buf, n, probe);
        }
    }
}

TEST(LevelSearch, DuplicatesAndDenseTicks) {
    std::mt19937 rng(7);
    for (uint32_t n : {1u, 7u, 8u, 9u, 31u, 32u, 33u, 64u, 200u}) {
        std::vector<uint32_t> keys(n);
        for (auto& k : keys) k = 1'000'000 + 100 * static_cast<uint32_t>(rng() % 16);  // many equal keys
        std::sort(keys.begin(), keys.end());
        const auto buf = padded(keys, rng);
        for (uint32_t probe : probes_for(keys, rng)) check_all(buf, n, probe);
    }
}

// Asks are stored as ~price, which sets the top bit: a signed compare would
// order them wrongly. This is the case the SSE2 sign-bias trick exists for.
TEST(LevelSearch, UnsignedOrderingAcrossSignBit) {
    std::mt19937 rng(3);
    std::vector<uint32_t> keys;
    for (uint32_t p = 1'000'000; p < 1'000'000 + 40 * 100; p += 100) keys.push_back(~p);
    for (uint32_t p = 1'000; p < 1'000 + 40 * 100; p += 100) keys.push_back(p);
    std::sort(keys.begin(), keys.end());
    const auto buf = padded(keys, rng);
    for (uint32_t probe : probes_for(keys, rng))
        check_all(buf, static_cast<uint32_t>(keys.size()), probe);
}

} // namespace
