#include "itch/book_builder.hpp"
#include "itch/parser.hpp"
#include "itch/sim/synthetic_feed.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace itch;

struct Snapshot {
    uint64_t messages, bad_length, unknown_ref, live;
    std::vector<TopOfBook> tops;
    bool operator==(const Snapshot&) const = default;
};

/// Replays `stream` up to `stop` bytes into a fresh engine, with or without
/// prefetching, and snapshots the result.
template <std::size_t Distance>
Snapshot run(const std::vector<uint8_t>& stream, std::size_t stop, uint32_t symbols,
             std::size_t* consumed = nullptr) {
    BookEngine engine(1 << 12);
    NullSink sink;
    BookBuilder<NullSink> builder(engine, sink);
    Parser<BookBuilder<NullSink>> p(builder);
    const std::size_t used = Distance == 0 ? p.parse_stream(stream.data(), stop)
                                           : p.template parse_stream_prefetch<Distance>(stream.data(), stop);
    if (consumed) *consumed = used;
    Snapshot s{p.stats().messages, p.stats().bad_length, engine.stats().unknown_ref,
               engine.orders().size(), {}};
    for (uint32_t loc = 1; loc <= symbols; ++loc) s.tops.push_back(engine.book(static_cast<uint16_t>(loc)).top());
    return s;
}

TEST(PrefetchPipeline, IdenticalResultsForEveryDistance) {
    sim::SyntheticFeedConfig cfg;
    cfg.symbols = 32;
    cfg.events = 100'000;
    const auto stream = sim::SyntheticFeed(cfg).build();
    // Stop mid-session so books are non-empty and the comparison has teeth.
    const std::size_t stop = stream.size() / 2;
    std::size_t used0 = 0, used1 = 0, used16 = 0, used64 = 0;
    const auto base = run<0>(stream, stop, cfg.symbols, &used0);
    EXPECT_GT(base.live, 0u);
    EXPECT_EQ(run<1>(stream, stop, cfg.symbols, &used1), base);
    EXPECT_EQ(run<16>(stream, stop, cfg.symbols, &used16), base);
    EXPECT_EQ(run<64>(stream, stop, cfg.symbols, &used64), base);
    EXPECT_EQ(used1, used0);  // same trailing partial record left unconsumed
    EXPECT_EQ(used16, used0);
    EXPECT_EQ(used64, used0);
}

/// Books only locates 3 and 17, through BookBuilder's forwarding of wants().
struct TwoSymbols {
    bool wants(uint8_t, uint16_t locate) const { return locate == 3 || locate == 17; }
};

template <std::size_t Distance>
Snapshot run_filtered(const std::vector<uint8_t>& stream, std::size_t stop, uint32_t symbols,
                      uint64_t* filtered) {
    BookEngine engine(1 << 12);
    TwoSymbols sink;
    BookBuilder<TwoSymbols> builder(engine, sink);
    Parser<BookBuilder<TwoSymbols>> p(builder);
    if constexpr (Distance == 0) p.parse_stream(stream.data(), stop);
    else                         p.template parse_stream_prefetch<Distance>(stream.data(), stop);
    *filtered = p.stats().filtered;
    Snapshot s{p.stats().messages, p.stats().bad_length, engine.stats().unknown_ref,
               engine.orders().size(), {}};
    for (uint32_t loc = 1; loc <= symbols; ++loc) s.tops.push_back(engine.book(static_cast<uint16_t>(loc)).top());
    return s;
}

TEST(PrefetchPipeline, LocateFilterKeepsWantedBooksExact) {
    sim::SyntheticFeedConfig cfg;
    cfg.symbols = 32;
    cfg.events = 100'000;
    const auto stream = sim::SyntheticFeed(cfg).build();
    const std::size_t stop = stream.size() / 2;
    const auto full = run<0>(stream, stop, cfg.symbols);
    for (const bool prefetch : {false, true}) {
        uint64_t filtered = 0;
        const auto f = prefetch ? run_filtered<16>(stream, stop, cfg.symbols, &filtered)
                                : run_filtered<0>(stream, stop, cfg.symbols, &filtered);
        EXPECT_EQ(f.messages, full.messages);
        EXPECT_GT(filtered, 0u);
        EXPECT_EQ(f.unknown_ref, 0u);  // no dangling references from the other books
        EXPECT_LT(f.live, full.live);
        for (uint32_t loc = 1; loc <= cfg.symbols; ++loc) {
            const TopOfBook want = (loc == 3 || loc == 17) ? full.tops[loc - 1] : TopOfBook{};
            EXPECT_EQ(f.tops[loc - 1], want) << "locate " << loc << " prefetch " << prefetch;
        }
        EXPECT_NE(f.tops[2], TopOfBook{});  // the comparison has teeth
    }
}

TEST(PrefetchPipeline, TinyAndMalformedInputs) {
    // Empty buffer, a lone length prefix, and a short 'A' record (the hint
    // must not read past it; the parser must still reject it).
    const std::vector<uint8_t> empty;
    const std::vector<uint8_t> prefix_only = {0x00, 0x24};
    const std::vector<uint8_t> short_add = {0x00, 0x05, 'A', 0, 1, 0, 0};
    for (const auto* s : {&empty, &prefix_only, &short_add}) {
        std::size_t a = 0, b = 0;
        EXPECT_EQ(run<16>(*s, s->size(), 1, &a), run<0>(*s, s->size(), 1, &b));
        EXPECT_EQ(a, b);
    }
}

} // namespace
