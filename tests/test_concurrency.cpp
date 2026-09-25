#include "itch/bbo_cache.hpp"
#include "itch/book_event.hpp"
#include "itch/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace itch;

TEST(SpscRing, SingleThreadFillAndDrain) {
    auto ring = std::make_unique<SpscRing<uint64_t, 8>>();
    for (uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(ring->try_push(i));
    EXPECT_FALSE(ring->try_push(99)) << "all N slots are usable, the N+1th is not";
    EXPECT_EQ(ring->size_approx(), 8u);
    uint64_t v = 0;
    for (uint64_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(ring->try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(ring->try_pop(v));
}

// Producer and consumer on separate threads: every value arrives exactly once
// and in order, across many wrap-arounds of a small ring.
TEST(SpscRing, TwoThreadsPreserveOrder) {
    constexpr uint64_t kItems = 2'000'000;
    auto ring = std::make_unique<SpscRing<uint64_t, 1024>>();
    std::thread producer([&] {
        for (uint64_t i = 0; i < kItems; ++i)
            while (!ring->try_push(i)) cpu_relax();
    });
    uint64_t expect = 0;
    while (expect < kItems) {
        if (expect % 2 == 0) {
            uint64_t v;
            if (ring->try_pop(v)) { ASSERT_EQ(v, expect); ++expect; }
        } else {
            ring->pop_batch([&](const uint64_t& v) { ASSERT_EQ(v, expect); ++expect; }, 64);
        }
    }
    producer.join();
    EXPECT_EQ(ring->size_approx(), 0u);
}

TEST(SpscRing, BookEventsSurviveTransport) {
    auto ring = std::make_unique<SpscRing<BookEvent, 256>>();
    constexpr uint64_t kItems = 200'000;
    std::thread producer([&] {
        for (uint64_t i = 0; i < kItems; ++i) {
            BookEvent e{};
            e.ref = i;
            e.bid_qty = i * 3;
            e.ask_qty = ~i;
            e.locate = static_cast<uint16_t>(i);
            while (!ring->try_push(e)) cpu_relax();
        }
    });
    for (uint64_t i = 0; i < kItems;) {
        BookEvent e;
        if (!ring->try_pop(e)) continue;
        ASSERT_EQ(e.ref, i);
        ASSERT_EQ(e.bid_qty, i * 3);
        ASSERT_EQ(e.ask_qty, ~i);
        ASSERT_EQ(e.locate, static_cast<uint16_t>(i));
        ++i;
    }
    producer.join();
}

// The writer stores internally consistent snapshots (all fields derived from
// one counter). A torn read, mixing fields from two writes, breaks the
// relation and fails the test. The writer only starts once both readers are
// running, and keeps writing until they have checked enough snapshots, so the
// test can't pass vacuously on a loaded machine where the readers are
// scheduled late.
TEST(BboCache, ReadersNeverObserveTornSnapshots) {
    constexpr int      kReaders   = 2;
    constexpr uint64_t kMinWrites = 200'000;
    constexpr uint64_t kMinReads  = 20'000;  // snapshots checked while writes were in flight
    constexpr uint64_t kMaxWrites = 50'000'000;

    BboCache cache;
    std::atomic<int> ready{0};
    std::atomic<bool> done{false};
    std::atomic<uint64_t> reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            uint64_t last = 0;
            while (!done.load(std::memory_order_acquire)) {
                const BboSnapshot s = cache.load();
                if (s.timestamp == 0) continue;
                const uint64_t i = s.timestamp;
                ASSERT_EQ(s.bid_price, static_cast<uint32_t>(i));
                ASSERT_EQ(s.ask_price, static_cast<uint32_t>(i + 1));
                ASSERT_EQ(s.bid_qty, i * 7);
                ASSERT_EQ(s.ask_qty, i * 11);
                ASSERT_GE(i, last) << "snapshots must not go back in time";
                last = i;
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    while (ready.load(std::memory_order_acquire) < kReaders) cpu_relax();

    uint64_t writes = 0;
    for (uint64_t i = 1; i <= kMaxWrites; ++i) {
        BboSnapshot s;
        s.bid_price = static_cast<uint32_t>(i);
        s.ask_price = static_cast<uint32_t>(i + 1);
        s.bid_qty   = i * 7;
        s.ask_qty   = i * 11;
        s.timestamp = i;
        cache.store(s);
        writes = i;
        if (i >= kMinWrites && reads.load(std::memory_order_relaxed) >= kMinReads) break;
    }
    done.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();
    EXPECT_EQ(cache.version(), writes);
    EXPECT_GE(reads.load(), kMinReads) << "readers barely ran; the test proved nothing";
}

} // namespace
