#include "itch/depth_profiler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using namespace itch;

/// Chunks the back-scan compares, simulated step by step (no chunk cap):
/// full 8-key chunks from the top while every key is >= target, then either
/// the chunk that contains a smaller key or the final partial (tail) load.
uint32_t simulate_chunks(const std::vector<uint32_t>& ascending, uint32_t key) {
    std::size_t i = ascending.size();
    uint32_t chunks = 0;
    while (i >= 8) {
        ++chunks;
        if (std::any_of(ascending.begin() + static_cast<std::ptrdiff_t>(i - 8),
                        ascending.begin() + static_cast<std::ptrdiff_t>(i),
                        [&](uint32_t k) { return k < key; }))
            return chunks;
        i -= 8;
    }
    return chunks + 1;  // tail
}

TEST(DepthProfiler, ChunkFormulaMatchesTheKernelScan) {
    for (uint32_t n = 0; n <= 100; ++n) {
        std::vector<uint32_t> keys(n);
        for (uint32_t i = 0; i < n; ++i) keys[i] = 100 + 10 * i;
        for (uint32_t key = 95; key <= 100 + 10 * n + 5; key += 5) {
            const auto ge = static_cast<uint32_t>(std::count_if(keys.begin(), keys.end(),
                                                                [&](uint32_t k) { return k >= key; }));
            ASSERT_EQ(DepthProfiler::chunks_for(ge), simulate_chunks(keys, key)) << "n=" << n << " key=" << key;
        }
    }
}

MsgAddOrder add(uint64_t ref, uint32_t price) {
    MsgAddOrder m;
    m.locate = 1; m.ref = ref; m.side = 'B'; m.shares = 100; m.price = price;
    return m;
}

uint64_t searches_at(const DepthProfiler& p, uint32_t ge) { return p.histogram()[ge]; }

// The reviewer's case: an existing level with 7 better levels above it. The
// search meets 8 keys >= target (the 7 better ones and the level itself),
// fills the first chunk, and scans 2 chunks.
TEST(DepthProfiler, ExistingLevelCountsItself) {
    BookEngine engine(1024);
    DepthProfiler p(engine);
    for (uint32_t i = 0; i < 12; ++i) p.on(add(i + 1, 1000 + 100 * i));  // 12 bid levels
    const uint64_t before = p.searches();
    MsgOrderDelete d;
    d.ref = 5;  // price 1400: levels 1500..2100 (7 of them) are better
    p.on(d);
    EXPECT_EQ(p.searches(), before + 1);
    EXPECT_EQ(searches_at(p, 8), 1u);
    EXPECT_EQ(DepthProfiler::chunks_for(8), 2u);
}

TEST(DepthProfiler, NewLevelInsertionCountsOnlyBetterLevels) {
    BookEngine engine(1024);
    DepthProfiler p(engine);
    for (uint32_t i = 0; i < 12; ++i) p.on(add(i + 1, 1000 + 100 * i));
    // The 12 adds ascend, so each new price is the best so far: 0 keys >= it.
    EXPECT_EQ(searches_at(p, 0), 12u);
    p.on(add(100, 1450));  // new level between 1400 and 1500: 7 better levels
    EXPECT_EQ(searches_at(p, 7), 1u);
    p.on(add(101, 1450));  // same price again: now an existing level -> 8
    EXPECT_EQ(searches_at(p, 8), 1u);
}

TEST(DepthProfiler, ReplaceIsTwoSearchesAndSeesTheRemoval) {
    BookEngine engine(1024);
    DepthProfiler p(engine);
    for (uint32_t i = 0; i < 12; ++i) p.on(add(i + 1, 1000 + 100 * i));
    const uint64_t before = p.searches();
    MsgOrderReplace u;
    u.original_ref = 12;  // the best level (2100), alone on its level
    u.new_ref = 50;
    u.shares = 100;
    u.price = 1050;       // re-enters deep in the book
    p.on(u);
    EXPECT_EQ(p.searches(), before + 2);
    EXPECT_EQ(searches_at(p, 1), 1u);   // removal: only the level itself
    // Insertion at 1050 after 2100 vanished: 1100..2000 = 10 levels above it.
    EXPECT_EQ(searches_at(p, 10), 1u);
    EXPECT_EQ(engine.book(1).bids.depth(), 12u);
}

} // namespace
