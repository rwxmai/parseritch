/// Kernel microbenchmarks: every SIMD kernel next to its scalar baseline.
///
///   BM_LevelSearch/<variant>/<depth>/<dist>  price-level lookup
///   BM_MapFind/<variant>/<live orders>       order-reference lookup (hits)
///   BM_MapFindMiss/<variant>/<live orders>   lookups of absent refs
///   BM_MapChurn/<variant>/<live orders>      insert + erase at constant size
///   BM_AddOrderDecode/<variant>              decode of an 'A' message
///
/// Lookups in a loop are independent, so these measure throughput (the CPU
/// overlaps cache misses of consecutive lookups), not single-lookup latency.

#include "itch/messages.hpp"
#include "itch/order_map.hpp"
#include "itch/simd/add_order_decode.hpp"
#include "itch/simd/level_search.hpp"
#include "legacy/legacy_find_order.hpp"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <random>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Level search
// ---------------------------------------------------------------------------

enum class Dist { kNearTop, kUniform };

struct LevelFixture {
    std::vector<uint32_t> keys;     // ascending, best at back
    std::vector<uint32_t> queries;  // existing level keys
};

LevelFixture make_levels(uint32_t depth, Dist dist) {
    LevelFixture f;
    f.keys.resize(std::max<uint32_t>(depth, itch::simd::kLevelSearchPad) + 8);
    for (uint32_t i = 0; i < depth; ++i) f.keys[i] = 1'000'000 + 100 * i;
    std::mt19937 rng(17);
    std::geometric_distribution<uint32_t> near(0.3);
    f.queries.resize(1 << 14);
    for (auto& q : f.queries) {
        uint32_t from_top = dist == Dist::kNearTop ? near(rng) : static_cast<uint32_t>(rng() % depth);
        from_top = std::min(from_top, depth - 1);
        q = f.keys[depth - 1 - from_top];
    }
    return f;
}

template <uint32_t (*Fn)(const uint32_t*, uint32_t, uint32_t)>
void BM_LevelSearch(benchmark::State& state) {
    const auto depth = static_cast<uint32_t>(state.range(0));
    const auto f = make_levels(depth, static_cast<Dist>(state.range(1)));
    const std::size_t mask = f.queries.size() - 1;
    std::size_t i = 0;
    uint64_t sum = 0;
    for (auto _ : state) {
        sum += Fn(f.keys.data(), depth, f.queries[i++ & mask]);
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations());
}

void level_args(benchmark::internal::Benchmark* b) {
    b->ArgNames({"depth", "uniform"});
    for (int d : {8, 32, 128, 512})
        for (int dist : {0, 1}) b->Args({d, dist});
}

namespace s = itch::simd;
BENCHMARK(BM_LevelSearch<&s::scalar::lower_bound>)->Name("BM_LevelSearch/std_lower_bound")->Apply(level_args);
BENCHMARK(BM_LevelSearch<&s::scalar::lower_bound_branchless>)->Name("BM_LevelSearch/branchless_binary")->Apply(level_args);
BENCHMARK(BM_LevelSearch<&s::scalar::lower_bound_from_back>)->Name("BM_LevelSearch/scalar_from_back")->Apply(level_args);
BENCHMARK(BM_LevelSearch<&s::sse2::lower_bound_from_back>)->Name("BM_LevelSearch/sse2_from_back")->Apply(level_args);
#if defined(ITCH_HAVE_SSE41)
BENCHMARK(BM_LevelSearch<&s::sse41::lower_bound_from_back>)->Name("BM_LevelSearch/sse41_from_back")->Apply(level_args);
#endif
#if defined(ITCH_HAVE_AVX2)
BENCHMARK(BM_LevelSearch<&s::avx2::lower_bound_from_back>)->Name("BM_LevelSearch/avx2_from_back")->Apply(level_args);
#endif

/// kLinearChunks sweep: how many 8-key chunks to scan from the top before
/// switching to binary search. Pair with `feed_handler --depth-profile` on a
/// real session, which reports where operations actually land.
template <uint32_t Chunks>
void BM_LevelSearchChunks(benchmark::State& state) {
    const auto depth = static_cast<uint32_t>(state.range(0));
    const auto f = make_levels(depth, static_cast<Dist>(state.range(1)));
    const std::size_t mask = f.queries.size() - 1;
    std::size_t i = 0;
    uint64_t sum = 0;
    for (auto _ : state) {
#if defined(ITCH_HAVE_AVX2)
        sum += s::avx2::lower_bound_from_back<Chunks>(f.keys.data(), depth, f.queries[i++ & mask]);
#elif defined(ITCH_HAVE_SSE41)
        sum += s::sse41::lower_bound_from_back<Chunks>(f.keys.data(), depth, f.queries[i++ & mask]);
#else
        sum += s::sse2::lower_bound_from_back<Chunks>(f.keys.data(), depth, f.queries[i++ & mask]);
#endif
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations());
}
void chunk_args(benchmark::internal::Benchmark* b) {
    b->ArgNames({"depth", "uniform"});
    for (int dist : {0, 1}) b->Args({256, dist});
}
BENCHMARK(BM_LevelSearchChunks<0>)->Name("BM_LevelSearchChunks/chunks:0")->Apply(chunk_args);
BENCHMARK(BM_LevelSearchChunks<1>)->Name("BM_LevelSearchChunks/chunks:1")->Apply(chunk_args);
BENCHMARK(BM_LevelSearchChunks<2>)->Name("BM_LevelSearchChunks/chunks:2")->Apply(chunk_args);
BENCHMARK(BM_LevelSearchChunks<4>)->Name("BM_LevelSearchChunks/chunks:4")->Apply(chunk_args);
BENCHMARK(BM_LevelSearchChunks<8>)->Name("BM_LevelSearchChunks/chunks:8")->Apply(chunk_args);
BENCHMARK(BM_LevelSearchChunks<32>)->Name("BM_LevelSearchChunks/chunks:32")->Apply(chunk_args);

// ---------------------------------------------------------------------------
// Order map lookups: legacy quadratic table vs Swiss-table groups
// ---------------------------------------------------------------------------

/// ITCH-like refs: increasing with random gaps, then shuffled for lookup order.
std::vector<uint64_t> make_refs(std::size_t n) {
    std::mt19937_64 rng(5);
    std::vector<uint64_t> refs(n);
    uint64_t r = 1000;
    for (auto& x : refs) x = (r += 1 + (rng() % 4));
    return refs;
}

std::vector<uint64_t> shuffled(std::vector<uint64_t> v) {
    std::shuffle(v.begin(), v.end(), std::mt19937_64(9));
    v.resize(std::bit_floor(v.size()));
    return v;
}

enum class Legacy { kAvx2, kScalar };

template <Legacy L>
void BM_MapFindLegacy(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto refs = make_refs(n);
    // The legacy design capped the load factor at 0.5.
    legacy::QuadraticRefTable t(static_cast<unsigned>(std::countr_zero(std::bit_ceil(2 * n))));
    for (uint64_t r : refs) t.insert(r);
    const auto q = shuffled(refs);
    const std::size_t mask = q.size() - 1;
    std::size_t i = 0;
    uint64_t sum = 0;
    // Read the order's data like BM_MapFind does (v1 kept it in a parallel
    // array), so both benchmarks do the same work per lookup.
    for (auto _ : state) {
        const uint64_t ref = q[i++ & mask];
#if defined(ITCH_HAVE_AVX2)
        if constexpr (L == Legacy::kAvx2) sum += t.entry(t.find_avx2(ref)).qty;
        else                              sum += t.entry(t.find_scalar(ref)).qty;
#else
        sum += t.entry(t.find_scalar(ref)).qty;
#endif
    }
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations());
}

template <class Group>
void BM_MapFind(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto refs = make_refs(n);
    itch::BasicOrderMap<Group> map(n);
    for (uint64_t r : refs) map.insert(r)->shares = static_cast<uint32_t>(r);
    const auto q = shuffled(refs);
    const std::size_t mask = q.size() - 1;
    std::size_t i = 0;
    uint64_t sum = 0;
    for (auto _ : state) sum += map.find(q[i++ & mask])->shares;
    benchmark::DoNotOptimize(sum);
    state.SetItemsProcessed(state.iterations());
    state.counters["load"] = static_cast<double>(n) / static_cast<double>(map.capacity());
}

template <class Group>
void BM_MapFindMiss(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto refs = make_refs(n);
    itch::BasicOrderMap<Group> map(n);
    for (uint64_t r : refs) (void)map.insert(r);
    std::vector<uint64_t> q(1 << 16);
    std::mt19937_64 rng(3);
    for (auto& x : q) x = (1ULL << 50) + rng();  // never inserted
    std::size_t i = 0, hits = 0;
    for (auto _ : state) hits += map.find(q[i++ & (q.size() - 1)]) != nullptr;
    benchmark::DoNotOptimize(hits);
    state.SetItemsProcessed(state.iterations());
}

/// Delete the oldest order and add a new one: the dominant ITCH pattern.
template <class Group>
void BM_MapChurn(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    itch::BasicOrderMap<Group> map(n);
    std::vector<uint64_t> ring(n);
    for (std::size_t k = 0; k < n; ++k) { ring[k] = k + 1; (void)map.insert(k + 1); }
    uint64_t next = n + 1;
    std::size_t head = 0;
    for (auto _ : state) {
        map.erase(map.find(ring[head]));
        (void)map.insert(next);
        ring[head] = next++;
        head = head + 1 == n ? 0 : head + 1;
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["rehashes"] = static_cast<double>(map.stats().rehashes);
}

/// Live-order counts. Powers of two land the Swiss table at a 0.5 load
/// factor (the legacy table's cap). 114'000 and 3'600'000 land it at ~0.87 and
/// ~0.86, near its 7/8 design maximum, which is where it saves memory.
void map_args(benchmark::internal::Benchmark* b) {
    b->ArgName("live");
    for (long n : {1L << 12, 1L << 16, 114'000L, 1L << 20, 3'600'000L}) b->Arg(n);
}

#if defined(ITCH_HAVE_AVX2)
BENCHMARK(BM_MapFindLegacy<Legacy::kAvx2>)->Name("BM_MapFind/legacy_quadratic_avx2")->Apply(map_args);
#endif
BENCHMARK(BM_MapFindLegacy<Legacy::kScalar>)->Name("BM_MapFind/legacy_quadratic_scalar")->Apply(map_args);
BENCHMARK(BM_MapFind<itch::simd::GroupSwar>)->Name("BM_MapFind/swiss_swar8")->Apply(map_args);
BENCHMARK(BM_MapFind<itch::simd::GroupSse2>)->Name("BM_MapFind/swiss_sse2_16")->Apply(map_args);
#if defined(ITCH_HAVE_AVX2)
BENCHMARK(BM_MapFind<itch::simd::GroupAvx2>)->Name("BM_MapFind/swiss_avx2_32")->Apply(map_args);
#endif
BENCHMARK(BM_MapFindMiss<itch::simd::GroupSwar>)->Name("BM_MapFindMiss/swiss_swar8")->Apply(map_args);
BENCHMARK(BM_MapFindMiss<itch::simd::GroupSse2>)->Name("BM_MapFindMiss/swiss_sse2_16")->Apply(map_args);
#if defined(ITCH_HAVE_AVX2)
BENCHMARK(BM_MapFindMiss<itch::simd::GroupAvx2>)->Name("BM_MapFindMiss/swiss_avx2_32")->Apply(map_args);
#endif
BENCHMARK(BM_MapChurn<itch::simd::GroupSwar>)->Name("BM_MapChurn/swiss_swar8")->Apply(map_args);
BENCHMARK(BM_MapChurn<itch::simd::GroupSse2>)->Name("BM_MapChurn/swiss_sse2_16")->Apply(map_args);
#if defined(ITCH_HAVE_AVX2)
BENCHMARK(BM_MapChurn<itch::simd::GroupAvx2>)->Name("BM_MapChurn/swiss_avx2_32")->Apply(map_args);
#endif

// ---------------------------------------------------------------------------
// Add Order decode: generic per-field loads vs shuffle kernels
// ---------------------------------------------------------------------------

struct AddOrderCorpus {
    static constexpr std::size_t kCount = 4096;
    std::vector<uint8_t> buf;
    AddOrderCorpus() : buf(kCount * 36 + itch::simd::kAddOrderOverread) {
        std::mt19937_64 rng(1);
        for (auto& b : buf) b = static_cast<uint8_t>(rng());
        for (std::size_t i = 0; i < kCount; ++i) buf[i * 36] = 'A';
    }
};

uint64_t fold(const itch::simd::AddOrderFields& f) {
    return f.ref ^ f.timestamp ^ f.shares ^ f.price ^ f.locate ^ static_cast<uint64_t>(f.side);
}

template <itch::simd::AddOrderFields (*Fn)(const uint8_t*)>
void BM_AddOrderDecode(benchmark::State& state) {
    const AddOrderCorpus c;
    uint64_t acc = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < AddOrderCorpus::kCount; ++i) acc += fold(Fn(c.buf.data() + i * 36));
    }
    benchmark::DoNotOptimize(acc);
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(AddOrderCorpus::kCount));
}

void BM_AddOrderDecodeGeneric(benchmark::State& state) {
    const AddOrderCorpus c;
    uint64_t acc = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < AddOrderCorpus::kCount; ++i) {
            const auto m = itch::decode<itch::MsgAddOrder>(c.buf.data() + i * 36);
            acc += m.ref ^ m.timestamp ^ m.shares ^ m.price ^ m.locate ^ static_cast<uint64_t>(m.side);
        }
    }
    benchmark::DoNotOptimize(acc);
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(AddOrderCorpus::kCount));
}

BENCHMARK(BM_AddOrderDecodeGeneric)->Name("BM_AddOrderDecode/generic_fields");
BENCHMARK(BM_AddOrderDecode<&itch::simd::scalar::decode_add_order>)->Name("BM_AddOrderDecode/scalar_movbe");
#if defined(ITCH_HAVE_SSSE3)
BENCHMARK(BM_AddOrderDecode<&itch::simd::ssse3::decode_add_order>)->Name("BM_AddOrderDecode/ssse3_pshufb");
#endif
#if defined(ITCH_HAVE_AVX2)
BENCHMARK(BM_AddOrderDecode<&itch::simd::avx2::decode_add_order>)->Name("BM_AddOrderDecode/avx2_vpshufb");
#endif

} // namespace
