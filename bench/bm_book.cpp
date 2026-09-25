/// Book-engine operation benchmarks. Each benchmark states exactly which
/// operations one iteration performs. (The old "BookUpdate_Add" timed a delete
/// *and* an add per iteration and reported the sum as the add latency.)

#include "itch/order_book.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <deque>
#include <random>
#include <vector>

namespace {

using itch::BookEngine;

constexpr uint16_t kSymbols        = 64;
constexpr uint32_t kOrdersPerSide  = 256;
constexpr uint32_t kMid            = 100'0000;  // $100.0000
constexpr uint32_t kTick           = 100;

struct Seeded {
    BookEngine engine{1 << 20};
    std::deque<uint64_t> fifo;  // live refs, oldest first
    uint64_t next_ref = 1;
    std::mt19937 rng{7};
    std::geometric_distribution<uint32_t> behind{0.3};

    Seeded() {
        for (uint16_t s = 1; s <= kSymbols; ++s)
            for (uint32_t i = 0; i < kOrdersPerSide; ++i) {
                add(s, 'B');
                add(s, 'S');
            }
    }
    uint32_t price(uint8_t side) {
        const uint32_t d = kTick * (1 + behind(rng));
        return side == 'B' ? kMid - d : kMid + d;
    }
    void add(uint16_t sym, uint8_t side) {
        engine.add(sym, next_ref, side, 100, price(side));
        fifo.push_back(next_ref++);
    }
};

/// One iteration = one Add Order + one Order Delete (live count stays fixed).
void BM_Engine_AddThenDelete(benchmark::State& state) {
    Seeded s;
    uint16_t sym = 1;
    for (auto _ : state) {
        s.add(sym, (s.next_ref & 1) ? 'B' : 'S');
        benchmark::DoNotOptimize(s.engine.remove(s.fifo.front()));
        s.fifo.pop_front();
        sym = static_cast<uint16_t>(sym % kSymbols + 1);
    }
    state.SetItemsProcessed(state.iterations() * 2);
    state.SetLabel("items = messages (2 per iteration)");
}
BENCHMARK(BM_Engine_AddThenDelete);

/// One iteration = one partial execution (1 share) of a resting order.
void BM_Engine_PartialExecute(benchmark::State& state) {
    BookEngine e(1 << 16);
    std::vector<uint64_t> refs;
    for (uint64_t r = 1; r <= 4096; ++r) {
        e.add(static_cast<uint16_t>(1 + r % kSymbols), r, 'B', 1'000'000'000u,
              kMid - kTick * static_cast<uint32_t>(r % 32));
        refs.push_back(r);
    }
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(e.reduce(refs[i], 1));
        i = (i + 1) & 4095;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Engine_PartialExecute);

/// One iteration = one Order Replace (old ref out, new ref in, new price).
void BM_Engine_Replace(benchmark::State& state) {
    Seeded s;
    for (auto _ : state) {
        const uint64_t old_ref = s.fifo.front();
        s.fifo.pop_front();
        const uint64_t new_ref = s.next_ref++;
        benchmark::DoNotOptimize(s.engine.replace(old_ref, new_ref, 200, s.price((new_ref & 1) ? 'B' : 'S')));
        s.fifo.push_back(new_ref);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Engine_Replace);

} // namespace
