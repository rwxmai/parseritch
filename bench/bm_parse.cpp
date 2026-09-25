/// End-to-end decode + book-building benchmarks.
///
///   BM_Parse_ValidateOnly   framing, length validation, dispatch (no handler work)
///   BM_Parse_BuildBooks     full pipeline over a synthetic session
///   BM_Parse_Prefetch       full pipeline, large live set, prefetch lookahead sweep
///   BM_Parse_PerMessage     per-message TSC latency distribution (p50/p99/p99.9)
///   BM_Replay_File          real Nasdaq file, if ITCH_FILE is set
///
/// The synthetic session drains every order at the end, so each iteration
/// starts from an empty book without a timed or untimed reset.

#include "itch/book_builder.hpp"
#include "itch/clock.hpp"
#include "itch/histogram.hpp"
#include "itch/parser.hpp"
#include "itch/sim/synthetic_feed.hpp"
#include "net/itch_file.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using namespace itch;

const std::vector<uint8_t>& session() {
    static const std::vector<uint8_t> s = [] {
        sim::SyntheticFeedConfig cfg;
        cfg.symbols = 512;
        cfg.events  = 2'000'000;
        return sim::SyntheticFeed(cfg).build();
    }();
    return s;
}

struct NoHandler {};

void BM_Parse_ValidateOnly(benchmark::State& state) {
    const auto& s = session();
    NoHandler h;
    Parser<NoHandler> p(h);
    for (auto _ : state) benchmark::DoNotOptimize(p.parse_stream(s.data(), s.size()));
    const uint64_t per_pass = p.stats().messages / static_cast<uint64_t>(state.iterations());
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(per_pass));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(s.size()));
}
BENCHMARK(BM_Parse_ValidateOnly)->Unit(benchmark::kMillisecond);

void BM_Parse_BuildBooks(benchmark::State& state) {
    const auto& s = session();
    BookEngine engine(1 << 20);
    NullSink sink;
    BookBuilder<NullSink> builder(engine, sink);
    Parser<BookBuilder<NullSink>> p(builder);
    for (auto _ : state) benchmark::DoNotOptimize(p.parse_stream(s.data(), s.size()));
    const uint64_t per_pass = p.stats().messages / static_cast<uint64_t>(state.iterations());
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(per_pass));
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(s.size()));
    state.counters["unknown_ref"] = static_cast<double>(engine.stats().unknown_ref);
}
BENCHMARK(BM_Parse_BuildBooks)->Unit(benchmark::kMillisecond);

/// A session with a large live-order set (~1.5M resting orders, a ~40 MB
/// slot array plus books), so order-map lookups miss the caches, which is
/// what the prefetch pipeline exists to hide.
const std::vector<uint8_t>& large_session() {
    static const std::vector<uint8_t> s = [] {
        sim::SyntheticFeedConfig cfg;
        cfg.symbols        = 2048;
        cfg.initial_orders = 750;
        cfg.events         = 2'000'000;
        return sim::SyntheticFeed(cfg).build();
    }();
    return s;
}

/// Full pipeline on the large session with a prefetch lookahead of
/// `Distance` records (0 = plain parse_stream, the baseline).
template <std::size_t Distance>
void BM_Parse_Prefetch(benchmark::State& state) {
    const auto& s = large_session();
    BookEngine engine(2u << 20);
    NullSink sink;
    BookBuilder<NullSink> builder(engine, sink);
    Parser<BookBuilder<NullSink>> p(builder);
    for (auto _ : state) benchmark::DoNotOptimize(p.template parse_stream_prefetch<Distance>(s.data(), s.size()));
    const uint64_t per_pass = p.stats().messages / static_cast<uint64_t>(state.iterations());
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(per_pass));
}
BENCHMARK(BM_Parse_Prefetch<0>)->Name("BM_Parse_Prefetch/distance:0")->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Parse_Prefetch<4>)->Name("BM_Parse_Prefetch/distance:4")->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Parse_Prefetch<8>)->Name("BM_Parse_Prefetch/distance:8")->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Parse_Prefetch<16>)->Name("BM_Parse_Prefetch/distance:16")->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Parse_Prefetch<32>)->Name("BM_Parse_Prefetch/distance:32")->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Parse_Prefetch<64>)->Name("BM_Parse_Prefetch/distance:64")->Unit(benchmark::kMillisecond);

/// Times every message individually with fenced RDTSC/RDTSCP. The fixed cost
/// of the timer itself (an empty fenced region) is measured and reported, not
/// silently subtracted.
void BM_Parse_PerMessage(benchmark::State& state) {
    const auto& s = session();
    static const double ticks_per_ns = TscClock::calibrate_ticks_per_ns();
    BookEngine engine(1 << 20);
    NullSink sink;
    BookBuilder<NullSink> builder(engine, sink);
    Parser<BookBuilder<NullSink>> p(builder);
    LatencyHistogram hist, timer;
    for (auto _ : state) {
        std::size_t off = 0;
        while (off + 2 <= s.size()) {
            const std::size_t n = load_be16(s.data() + off);
            const uint64_t t0 = TscClock::start();
            p.parse(s.data() + off + 2, n);
            const uint64_t t1 = TscClock::stop();
            hist.record(t1 - t0);
            off += 2 + n;
        }
        for (int i = 0; i < 100'000; ++i) {
            const uint64_t t0 = TscClock::start();
            const uint64_t t1 = TscClock::stop();
            timer.record(t1 - t0);
        }
    }
    const auto ns = [&](uint64_t ticks) { return static_cast<double>(ticks) / ticks_per_ns; };
    state.counters["p50_ns"]   = ns(hist.percentile(50));
    state.counters["p99_ns"]   = ns(hist.percentile(99));
    state.counters["p99.9_ns"] = ns(hist.percentile(99.9));
    state.counters["max_ns"]   = ns(hist.max());
    state.counters["timer_p50_ns"] = ns(timer.percentile(50));
    state.SetItemsProcessed(static_cast<int64_t>(hist.count()));
}
BENCHMARK(BM_Parse_PerMessage)->Unit(benchmark::kMillisecond)->Iterations(3);

/// Full real-data replay: ITCH_FILE=/path/to/01302019.NASDAQ_ITCH50 (gunzipped).
void BM_Replay_File(benchmark::State& state) {
    const char* path = std::getenv("ITCH_FILE");
    if (path == nullptr) {
        state.SkipWithMessage("set ITCH_FILE to a gunzipped Nasdaq ITCH 5.0 file");
        return;
    }
    net::ItchFile file;
    if (const auto err = file.open(path); !err.empty()) {
        state.SkipWithError(err.c_str());
        return;
    }
    for (auto _ : state) {
        BookEngine engine(8u << 20);  // sized for a full session's peak live orders
        NullSink sink;
        BookBuilder<NullSink> builder(engine, sink);
        Parser<BookBuilder<NullSink>> p(builder);
        p.parse_stream(file.data(), file.size());
        state.counters["messages"]      = static_cast<double>(p.stats().messages);
        state.counters["unknown_ref"]   = static_cast<double>(engine.stats().unknown_ref);
        state.counters["missing_level"] = static_cast<double>(engine.stats().missing_level);
        state.counters["map_rehashes"]  = static_cast<double>(engine.orders().stats().rehashes);
        state.SetItemsProcessed(static_cast<int64_t>(p.stats().messages));
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(file.size()));
}
BENCHMARK(BM_Replay_File)->Unit(benchmark::kSecond)->Iterations(1);

} // namespace
