#include "itch/book_builder.hpp"
#include "itch/order_book.hpp"
#include "itch/parser.hpp"
#include "itch/sim/synthetic_feed.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <unordered_map>

namespace {

using namespace itch;

// ---------------------------------------------------------------------------
// LevelSide against std::map
// ---------------------------------------------------------------------------

TEST(LevelSide, RandomAddReduceMatchesMap) {
    std::mt19937 rng(11);
    LevelSide side;
    std::map<uint32_t, std::pair<uint64_t, uint32_t>> model;  // key -> (qty, orders)
    std::vector<std::pair<uint32_t, uint32_t>> resting;       // (key, shares) per live order

    for (int i = 0; i < 200'000; ++i) {
        if (resting.empty() || rng() % 100 < 55) {
            // Concentrate near the top like real flow, with an occasional deep level.
            const uint32_t key = (rng() % 10 == 0) ? static_cast<uint32_t>(rng() % 5000)
                                                   : 4900 + static_cast<uint32_t>(rng() % 100);
            const uint32_t sh = 1 + static_cast<uint32_t>(rng() % 500);
            const bool was_top = !model.empty() && key >= model.rbegin()->first;
            const bool top = side.add(key, sh);
            EXPECT_EQ(top, was_top || model.empty());
            auto& m = model[key];
            m.first += sh;
            ++m.second;
            resting.emplace_back(key, sh);
        } else {
            const std::size_t idx = rng() % resting.size();
            auto [key, sh] = resting[idx];
            const bool partial = rng() % 3 == 0 && sh > 1;
            const uint32_t take = partial ? sh / 2 : sh;
            const bool is_top = key == model.rbegin()->first;
            const auto r = side.reduce(key, take, !partial);
            ASSERT_NE(r, LevelSide::Reduce::kMissingLevel);
            EXPECT_EQ(r == LevelSide::Reduce::kTopChanged, is_top);
            auto& m = model[key];
            m.first -= take;
            if (!partial) --m.second;
            if (m.second == 0) model.erase(key);
            if (partial) resting[idx].second -= take;
            else { resting[idx] = resting.back(); resting.pop_back(); }
        }
        if (i % 1000 == 0) {
            ASSERT_EQ(side.depth(), model.size());
            uint32_t lvl = 0;
            for (auto it = model.rbegin(); it != model.rend(); ++it, ++lvl) {
                ASSERT_EQ(side.key_at(lvl), it->first);
                ASSERT_EQ(side.qty_at(lvl), it->second.first);
                ASSERT_EQ(side.orders_at(lvl), it->second.second);
            }
        }
    }
    EXPECT_GE(side.capacity(), 64u) << "deep levels should have forced growth";
}

TEST(LevelSide, ReduceOnEmptyOrMissingLevelIsReported) {
    LevelSide s;
    EXPECT_EQ(s.reduce(100, 1, true), LevelSide::Reduce::kMissingLevel);
    s.add(100, 10);
    EXPECT_EQ(s.reduce(101, 1, true), LevelSide::Reduce::kMissingLevel);
}

// ---------------------------------------------------------------------------
// BookEngine semantics
// ---------------------------------------------------------------------------

TEST(BookEngine, BidAskOrderingAndTopOfBook) {
    BookEngine e(1024);
    e.add(1, 1, 'B', 100, 10'0000);
    e.add(1, 2, 'B', 200, 10'0100);  // better bid
    e.add(1, 3, 'S', 300, 10'0300);
    e.add(1, 4, 'S', 400, 10'0200);  // better ask
    const auto t = e.book(1).top();
    EXPECT_EQ(t.bid_price, 10'0100u);
    EXPECT_EQ(t.bid_qty, 200u);
    EXPECT_EQ(t.ask_price, 10'0200u);
    EXPECT_EQ(t.ask_qty, 400u);
    EXPECT_FALSE(e.book(1).crossed());
    e.add(1, 5, 'B', 1, 10'0200);
    EXPECT_TRUE(e.book(1).crossed());
}

TEST(BookEngine, ReplaceKeepsSideAndLocate) {
    BookEngine e(1024);
    e.add(7, 10, 'S', 500, 20'0000);
    const auto u = e.replace(10, 11, 300, 19'9900);
    ASSERT_TRUE(u);
    EXPECT_EQ(u.side, 'S');
    EXPECT_EQ(u.locate, 7);
    EXPECT_TRUE(u.top_changed);
    EXPECT_EQ(e.orders().find(10), nullptr);
    ASSERT_NE(e.orders().find(11), nullptr);
    EXPECT_EQ(e.book(7).asks.depth(), 1u);
    EXPECT_EQ(e.book(7).top().ask_price, 19'9900u);
    EXPECT_EQ(e.book(7).top().ask_qty, 300u);
}

TEST(BookEngine, PartialThenFullExecution) {
    BookEngine e(1024);
    e.add(1, 1, 'B', 100, 10'0000);
    e.add(1, 2, 'B', 50, 10'0000);
    EXPECT_TRUE(e.reduce(1, 40));
    EXPECT_EQ(e.book(1).top().bid_qty, 110u);
    EXPECT_EQ(e.book(1).bids.orders_at(0), 2u);
    EXPECT_TRUE(e.reduce(1, 60));  // order 1 now fully filled
    EXPECT_EQ(e.orders().find(1), nullptr);
    EXPECT_EQ(e.book(1).bids.orders_at(0), 1u);
    EXPECT_TRUE(e.remove(2));
    EXPECT_TRUE(e.book(1).bids.empty());
}

TEST(BookEngine, ZeroShareReduceIsANoOp) {
    BookEngine e(1024);
    e.add(1, 1, 'B', 100, 10'0000);
    const auto u = e.reduce(1, 0);
    EXPECT_TRUE(u);
    EXPECT_FALSE(u.top_changed) << "nothing changed, so nothing to publish";
    EXPECT_EQ(e.book(1).top().bid_qty, 100u);
    ASSERT_NE(e.orders().find(1), nullptr);
}

TEST(BookEngine, AnomaliesAreCountedNotFatal) {
    BookEngine e(1024);
    EXPECT_FALSE(e.reduce(999, 1));
    EXPECT_FALSE(e.remove(999));
    EXPECT_FALSE(e.replace(999, 1000, 1, 1));
    EXPECT_EQ(e.stats().unknown_ref, 3u);
    e.add(1, 1, 'B', 10, 100);
    e.reduce(1, 25);  // more than resting
    EXPECT_EQ(e.stats().overfill, 1u);
    EXPECT_EQ(e.orders().find(1), nullptr);
}

// ---------------------------------------------------------------------------
// Full pipeline vs. a naive reference engine on a synthetic session
// ---------------------------------------------------------------------------

class ReferenceEngine {
public:
    void on(const MsgAddOrder& m) { add(m.locate, m.ref, m.side, m.shares, m.price); }
    void on(const MsgAddOrderMpid& m) { add(m.locate, m.ref, m.side, m.shares, m.price); }
    void on(const MsgOrderExecuted& m) { reduce(m.ref, m.executed_shares); }
    void on(const MsgOrderExecutedWithPrice& m) { reduce(m.ref, m.executed_shares); }
    void on(const MsgOrderCancel& m) { reduce(m.ref, m.cancelled_shares); }
    void on(const MsgOrderDelete& m) {
        auto& o = orders_.at(m.ref);
        reduce(m.ref, o.shares);
    }
    void on(const MsgOrderReplace& m) {
        const Ord o = orders_.at(m.original_ref);
        reduce(m.original_ref, o.shares);
        add(o.locate, m.new_ref, o.side, m.shares, m.price);
    }

    /// (price -> (qty, orders)) per side; bids iterate best-first via rbegin.
    using Side = std::map<uint32_t, std::pair<uint64_t, uint32_t>>;
    std::unordered_map<uint16_t, std::pair<Side, Side>> books;  // locate -> (bids, asks)
    std::size_t live() const { return orders_.size(); }

private:
    struct Ord { uint16_t locate; char side; uint32_t price, shares; };

    void add(uint16_t loc, uint64_t ref, char side, uint32_t sh, uint32_t px) {
        orders_[ref] = {loc, side, px, sh};
        auto& lvl = side_of(loc, side)[px];
        lvl.first += sh;
        ++lvl.second;
    }
    void reduce(uint64_t ref, uint32_t sh) {
        auto it = orders_.find(ref);
        Ord& o = it->second;
        auto& s = side_of(o.locate, o.side);
        auto& lvl = s[o.price];
        lvl.first -= sh;
        o.shares -= sh;
        if (o.shares == 0) {
            if (--lvl.second == 0) s.erase(o.price);
            orders_.erase(it);
        }
    }
    Side& side_of(uint16_t loc, char side) {
        auto& b = books[loc];
        return side == 'B' ? b.first : b.second;
    }
    std::unordered_map<uint64_t, Ord> orders_;
};

void expect_books_equal(const BookEngine& e, const ReferenceEngine& ref) {
    for (const auto& [loc, sides] : ref.books) {
        const OrderBook& b = e.book(loc);
        const auto& [bids, asks] = sides;
        ASSERT_EQ(b.bids.depth(), bids.size()) << "locate " << loc;
        ASSERT_EQ(b.asks.depth(), asks.size()) << "locate " << loc;
        uint32_t i = 0;
        for (auto it = bids.rbegin(); it != bids.rend(); ++it, ++i) {
            ASSERT_EQ(b.bids.key_at(i), it->first);
            ASSERT_EQ(b.bids.qty_at(i), it->second.first);
            ASSERT_EQ(b.bids.orders_at(i), it->second.second);
        }
        i = 0;
        for (auto it = asks.begin(); it != asks.end(); ++it, ++i) {  // lowest ask is best
            ASSERT_EQ(OrderBook::price_of('S', b.asks.key_at(i)), it->first);
            ASSERT_EQ(b.asks.qty_at(i), it->second.first);
            ASSERT_EQ(b.asks.orders_at(i), it->second.second);
        }
    }
}

TEST(BookPipeline, SyntheticSessionMatchesReferenceEngine) {
    sim::SyntheticFeedConfig cfg;
    cfg.symbols = 64;
    cfg.events  = 300'000;
    sim::SyntheticFeed feed(cfg);
    const auto stream = feed.build();

    BookEngine engine(1 << 10);  // deliberately small: exercises map growth
    NullSink sink;
    BookBuilder<NullSink> builder(engine, sink);
    Parser<BookBuilder<NullSink>> parser(builder);

    ReferenceEngine ref;
    Parser<ReferenceEngine> ref_parser(ref);

    // Walk record by record and compare books at checkpoints mid-session,
    // when books are deep, not just at the end when they are empty.
    std::size_t off = 0, n = 0;
    while (off + 2 <= stream.size()) {
        const std::size_t len = load_be16(stream.data() + off);
        parser.parse(stream.data() + off + 2, len);
        ref_parser.parse(stream.data() + off + 2, len);
        off += 2 + len;
        if (++n % 50'000 == 0) {
            ASSERT_EQ(engine.orders().size(), ref.live());
            expect_books_equal(engine, ref);
        }
    }
    EXPECT_EQ(parser.stats().messages, feed.messages());
    EXPECT_EQ(engine.stats().unknown_ref, 0u);
    EXPECT_EQ(engine.stats().missing_level, 0u);
    EXPECT_EQ(engine.stats().overfill, 0u);
    EXPECT_EQ(engine.orders().size(), 0u) << "stream drains every order";
    EXPECT_GT(engine.orders().stats().grows, 0u);
    expect_books_equal(engine, ref);
}

} // namespace
