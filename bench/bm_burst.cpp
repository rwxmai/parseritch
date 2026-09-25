/// BM_Burst: tail latency after idle gaps, the case where frequency-license
/// and vector-lane power transitions hurt.
///
/// Market data is quiet, then bursty. After an idle stretch without wide
/// instructions the core powers down the upper vector lanes; the first 256-bit
/// instruction afterwards pays a warm-up window, and heavy wide instructions
/// can move the core to a lower frequency license. Averages hide this. This
/// benchmark exposes it:
///
///   repeat:  spin for `gap_us` with PAUSE only (no vector instructions)
///            process a burst of `burst` messages, timing each one
///
/// and reports the per-message latency of the first kHead messages of every
/// burst separately from the rest. Run it in both builds and compare:
///
///   ./bm_itch      --benchmark_filter=BM_Burst     # 128-bit default
///   ./bm_itch_avx2 --benchmark_filter=BM_Burst     # AVX2 opt-in
///
/// With PMU access (Linux, bare metal), "freq_ratio" is cycles / ref-cycles
/// over the bursts: effective frequency relative to nominal.
/// tools/license_check.sh adds Intel's model-specific license counters.

#include "itch/book_builder.hpp"
#include "itch/clock.hpp"
#include "itch/histogram.hpp"
#include "itch/parser.hpp"
#include "itch/perf_counters.hpp"
#include "itch/sim/synthetic_feed.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

namespace {

using namespace itch;

constexpr uint32_t kHead = 8;  // "first messages of a burst"

struct Session {
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offsets;  // start of each [len][msg] record
    Session() {
        sim::SyntheticFeedConfig cfg;
        cfg.symbols = 512;
        cfg.events  = 1'000'000;
        bytes = sim::SyntheticFeed(cfg).build();
        for (std::size_t off = 0; off + 2 <= bytes.size();) {
            offsets.push_back(static_cast<uint32_t>(off));
            off += 2u + std::size_t{load_be16(bytes.data() + off)};
        }
    }
};

const Session& session() {
    static const Session s;
    return s;
}

void spin_for_ticks(uint64_t ticks) {
    const uint64_t end = TscClock::now() + ticks;
    while (TscClock::now() < end) cpu_relax();
}

void BM_Burst(benchmark::State& state) {
    const auto gap_us = static_cast<uint64_t>(state.range(0));
    const auto burst  = static_cast<uint32_t>(state.range(1));
    static const double ticks_per_ns = TscClock::calibrate_ticks_per_ns();
    const auto gap_ticks = static_cast<uint64_t>(static_cast<double>(gap_us) * 1000.0 * ticks_per_ns);

    const Session& s = session();
    BookEngine engine(1 << 20);
    NullSink sink;
    BookBuilder<NullSink> builder(engine, sink);
    Parser<BookBuilder<NullSink>> parser(builder);

    // The session drains every order at its end, so wrapping around to its
    // start keeps the book state consistent. Warm the tables first.
    std::size_t next = 0;
    const auto parse_next = [&] {
        const uint32_t off = s.offsets[next];
        parser.parse(s.bytes.data() + off + 2, load_be16(s.bytes.data() + off));
        if (++next == s.offsets.size()) next = 0;
    };
    for (int i = 0; i < 200'000; ++i) parse_next();

    LatencyHistogram head, rest;
    PerfCounters pmu;
    PerfCounters::Sample in_bursts{};
    for (auto _ : state) {
        spin_for_ticks(gap_ticks);
        const PerfCounters::Sample before = pmu.read();
        for (uint32_t i = 0; i < burst; ++i) {
            const uint64_t t0 = TscClock::start();
            parse_next();
            const uint64_t t1 = TscClock::stop();
            (i < kHead ? head : rest).record(t1 - t0);
        }
        const PerfCounters::Sample d = pmu.read() - before;
        in_bursts.cycles += d.cycles;
        in_bursts.ref_cycles += d.ref_cycles;
        in_bursts.instructions += d.instructions;
    }

    const auto ns = [&](uint64_t t) { return static_cast<double>(t) / ticks_per_ns; };
    state.counters["head_p50_ns"] = ns(head.percentile(50));
    state.counters["head_p99_ns"] = ns(head.percentile(99));
    state.counters["head_max_ns"] = ns(head.max());
    state.counters["rest_p50_ns"] = ns(rest.percentile(50));
    state.counters["rest_p99_ns"] = ns(rest.percentile(99));
    if (pmu.available()) {
        if (in_bursts.ref_cycles != 0) state.counters["freq_ratio"] = in_bursts.frequency_ratio();
        state.counters["ipc"] = in_bursts.ipc();
    } else {
        state.SetLabel("no PMU: " + pmu.error());
    }
    state.SetItemsProcessed(state.iterations() * burst);
}
// gap in microseconds x burst length. 0 = back-to-back bursts (the control).
BENCHMARK(BM_Burst)
    ->ArgNames({"gap_us", "burst"})
    ->Args({0, 256})->Args({100, 256})->Args({1000, 256})->Args({10000, 256})
    ->Iterations(300)->Unit(benchmark::kMillisecond);

} // namespace
