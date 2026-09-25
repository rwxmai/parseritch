#pragma once

/// Level-search depth profiler behind `feed_handler --depth-profile`, which is
/// how ITCH_LEVEL_LINEAR_CHUNKS gets chosen from a real session.

#include "itch/book_builder.hpp"
#include "itch/messages.hpp"
#include "itch/order_book.hpp"
#include "itch/simd/level_search.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace itch {

/// Measures every level search exactly as the kernel sees it: the number of
/// keys at or above the target key (`ge`) on the searched side, taken just
/// *before* the event is applied. The back-scan compares 8 keys per chunk
/// and stops at the first chunk holding a key below the target, so a search
/// with `ge` such keys costs floor(ge / 8) + 1 chunks before any binary search.
///
///   A/F        one search: insertion point for the new order's price
///   E/C/X/D    one search: the resting order's level (always present)
///   U          two searches: the old level (removal), then the new price
///
/// Runs as its own parser handler in front of a BookBuilder, only in
/// profiling mode, so the hot path pays nothing for it.
class DepthProfiler {
public:
    explicit DepthProfiler(BookEngine& engine) : engine_(engine), builder_(engine, sink_) {}

    void on(const MsgStockDirectory& m) { builder_.on(m); }
    void on(const MsgAddOrder& m) { measure(m.locate, static_cast<uint8_t>(m.side), m.price); builder_.on(m); }
    void on(const MsgAddOrderMpid& m) { measure(m.locate, static_cast<uint8_t>(m.side), m.price); builder_.on(m); }
    void on(const MsgOrderExecuted& m) { measure_order(m.ref); builder_.on(m); }
    void on(const MsgOrderExecutedWithPrice& m) { measure_order(m.ref); builder_.on(m); }
    void on(const MsgOrderCancel& m) { measure_order(m.ref); builder_.on(m); }
    void on(const MsgOrderDelete& m) { measure_order(m.ref); builder_.on(m); }
    void on(const MsgOrderReplace& m) {
        const Order* o = engine_.orders().find(m.original_ref);
        if (o == nullptr) { builder_.on(m); return; }
        const uint16_t locate = o->locate;
        const uint8_t side = o->side;
        measure(locate, side, o->price);   // removal of the old order
        // The engine removes the old order before inserting the new one, so
        // the insertion search sees the book without it. Emulate that order.
        const uint32_t removed_key = OrderBook::key_for(side, o->price);
        const uint32_t new_key = OrderBook::key_for(side, m.price);
        const LevelSide& s = engine_.book(locate).side(side);
        const bool old_level_vanishes = s.orders_at(level_of(s, removed_key)) == 1;
        uint32_t ge = keys_at_or_above(s, new_key);
        if (old_level_vanishes && removed_key >= new_key) --ge;
        record(ge);
        builder_.on(m);
    }

    /// searches[ge] = number of searches that met `ge` keys at or above the
    /// target (the last bucket collects everything >= kMax).
    [[nodiscard]] const uint64_t* histogram() const noexcept { return hist_; }
    [[nodiscard]] uint64_t searches() const noexcept { return total_; }

    /// Chunks the back-scan compares for a search meeting `ge` such keys.
    static constexpr uint32_t chunks_for(uint32_t ge) noexcept { return ge / 8 + 1; }

    void print() const {
        std::printf("\n=== Level search profile (%" PRIu64 " searches) ===\n", total_);
        std::printf("  keys>=target  searches      cum%%   chunks\n");
        uint64_t cum = 0;
        uint32_t p99_chunks = 0, p999_chunks = 0;
        for (uint32_t ge = 0; ge <= kMax; ++ge) {
            if (hist_[ge] == 0) continue;
            cum += hist_[ge];
            const double pct = 100.0 * static_cast<double>(cum) / static_cast<double>(total_);
            const uint32_t chunks = chunks_for(ge);
            if (ge < 24 || ge % 16 == 0 || ge == kMax)
                std::printf("  %s%-11u %-13" PRIu64 " %6.2f   %u\n", ge == kMax ? ">=" : "  ", ge, hist_[ge], pct, chunks);
            if (p99_chunks == 0 && pct >= 99.0) p99_chunks = chunks;
            if (p999_chunks == 0 && pct >= 99.9) p999_chunks = chunks;
        }
        std::printf("  back-scan chunks covering 99%% of searches: %u, 99.9%%: %u "
                    "(this build: ITCH_LEVEL_LINEAR_CHUNKS=%u)\n",
                    p99_chunks, p999_chunks, simd::kLinearChunks);
    }

    static constexpr uint32_t kMax = 1024;

private:

    static uint32_t keys_at_or_above(const LevelSide& s, uint32_t key) {
        uint32_t ge = 0;
        while (ge < s.depth() && s.key_at(ge) >= key) ++ge;  // key_at counts from the top
        return ge;
    }
    static uint32_t level_of(const LevelSide& s, uint32_t key) {
        uint32_t d = 0;
        while (d < s.depth() && s.key_at(d) > key) ++d;
        return d;
    }
    void measure(uint16_t locate, uint8_t side, uint32_t price) {
        record(keys_at_or_above(engine_.book(locate).side(side), OrderBook::key_for(side, price)));
    }
    void measure_order(uint64_t ref) {
        if (const Order* o = engine_.orders().find(ref)) measure(o->locate, o->side, o->price);
    }
    void record(uint32_t ge) {
        ++hist_[ge < kMax ? ge : kMax];
        ++total_;
    }

    BookEngine&                    engine_;
    NullSink                       sink_;
    BookBuilder<NullSink>    builder_;
    uint64_t hist_[kMax + 1] = {};
    uint64_t total_ = 0;
};

} // namespace itch
