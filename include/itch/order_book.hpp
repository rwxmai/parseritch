#pragma once

/// Price-level books and the ITCH order-book engine.
///
/// Layout decisions:
///
/// * Structure of arrays. Each side keeps keys[], qty[] and orders[] in
///   separate arrays, so the SIMD level search streams through 4-byte keys
///   only (8 per AVX2 compare) without striding over quantities.
///
/// * Best price at the back. Each side is ascending by key, with the best
///   level last. Most adds, cancels and executions happen at or near the top
///   of the book, so the search usually ends in the first chunk and a level
///   insert or erase shifts only the few elements above it. With the best
///   level at index 0, every new top-of-book would shift the whole array.
///   Shifts use simd/block_move.hpp (SSE2 only), not libc memmove, which
///   picks 256/512-bit copies at run time on AVX2/AVX-512 hardware.
///
/// * One ordering for both sides. Bids use key = price and asks use
///   key = ~price, so "higher key = better" holds on both sides and a single
///   branch-free search routine serves both.
///
/// * Lazy, growable storage. A book allocates when it gets its first order;
///   if a side runs out of room, it doubles on a cold path. ITCH covers ~8-9k
///   symbols across 65536 possible locates, so the engine's book array is 4 MiB
///   of small headers instead of a fixed worst-case reservation per locate.

#include "itch/order_map.hpp"
#include "itch/platform.hpp"
#include "itch/simd/block_move.hpp"
#include "itch/simd/level_search.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

namespace itch {

/// One side of one book: ascending keys, best level at the back.
class LevelSide {
public:
    LevelSide() noexcept = default;
    LevelSide(const LevelSide&)            = delete;
    LevelSide& operator=(const LevelSide&) = delete;

    /// Adds one new resting order of `shares` at `key`. Returns true if the
    /// top of book changed.
    ITCH_ALWAYS_INLINE bool add(uint32_t key, uint32_t shares) {
        const uint32_t n   = size_;
        const uint32_t pos = n ? simd::lower_bound_from_back(keys_.get(), n, key) : 0;
        if (pos < n && keys_[pos] == key) {
            qty_[pos] += shares;
            ++orders_[pos];
            return pos == n - 1;
        }
        if (ITCH_UNLIKELY(n == cap_)) grow();
        const uint32_t tail = n - pos;  // levels better than the new one
        if (tail != 0) {
            simd::shift_up(keys_.get(), pos, tail);
            simd::shift_up(qty_.get(), pos, tail);
            simd::shift_up(orders_.get(), pos, tail);
        }
        keys_[pos]   = key;
        qty_[pos]    = shares;
        orders_[pos] = 1;
        size_        = n + 1;
        return tail == 0;
    }

    enum class Reduce : uint8_t { kOk, kTopChanged, kMissingLevel };

    /// Removes `shares` from the level at `key`; `order_gone` also removes one
    /// order from the level's count. Erases the level when its last order goes.
    ITCH_ALWAYS_INLINE Reduce reduce(uint32_t key, uint32_t shares, bool order_gone) noexcept {
        const uint32_t n = size_;
        if (ITCH_UNLIKELY(n == 0)) return Reduce::kMissingLevel;
        const uint32_t pos = simd::lower_bound_from_back(keys_.get(), n, key);
        if (ITCH_UNLIKELY(pos >= n || keys_[pos] != key)) return Reduce::kMissingLevel;

        qty_[pos] -= shares;
        orders_[pos] -= order_gone ? 1u : 0u;
        if (orders_[pos] == 0) {
            assert(qty_[pos] == 0 && "level quantity must drain with its last order");
            const uint32_t tail = n - pos - 1;
            if (tail != 0) {
                simd::shift_down(keys_.get(), pos, tail);
                simd::shift_down(qty_.get(), pos, tail);
                simd::shift_down(orders_.get(), pos, tail);
            }
            size_ = n - 1;
        }
        return pos == n - 1 ? Reduce::kTopChanged : Reduce::kOk;
    }

    [[nodiscard]] uint32_t depth() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    /// Level accessors counted from the top of book (0 = best).
    [[nodiscard]] uint32_t key_at(uint32_t level) const noexcept { return keys_[size_ - 1 - level]; }
    [[nodiscard]] uint64_t qty_at(uint32_t level) const noexcept { return qty_[size_ - 1 - level]; }
    [[nodiscard]] uint32_t orders_at(uint32_t level) const noexcept { return orders_[size_ - 1 - level]; }

    [[nodiscard]] uint32_t capacity() const noexcept { return cap_; }

    /// Allocate initial storage now instead of on the first add.
    void reserve_initial() { if (cap_ == 0) grow(); }

private:
    ITCH_COLD void grow();

    std::unique_ptr<uint32_t[]> keys_;
    std::unique_ptr<uint64_t[]> qty_;
    std::unique_ptr<uint32_t[]> orders_;
    uint32_t size_ = 0;
    uint32_t cap_  = 0;
};

struct TopOfBook {
    uint32_t bid_price = 0;  ///< 0 when the side is empty
    uint32_t ask_price = 0;  ///< 0 when the side is empty
    uint64_t bid_qty   = 0;
    uint64_t ask_qty   = 0;

    friend bool operator==(const TopOfBook&, const TopOfBook&) = default;
};

class OrderBook {
public:
    static constexpr uint32_t key_for(uint8_t side, uint32_t price) noexcept {
        return side == 'B' ? price : ~price;
    }
    static constexpr uint32_t price_of(uint8_t side, uint32_t key) noexcept {
        return side == 'B' ? key : ~key;
    }

    [[nodiscard]] LevelSide& side(uint8_t s) noexcept { return s == 'B' ? bids : asks; }
    [[nodiscard]] const LevelSide& side(uint8_t s) const noexcept { return s == 'B' ? bids : asks; }

    [[nodiscard]] TopOfBook top() const noexcept {
        TopOfBook t;
        if (!bids.empty()) { t.bid_price = bids.key_at(0);  t.bid_qty = bids.qty_at(0); }
        if (!asks.empty()) { t.ask_price = ~asks.key_at(0); t.ask_qty = asks.qty_at(0); }
        return t;
    }

    /// Locked or crossed: best bid >= best ask.
    [[nodiscard]] bool crossed() const noexcept {
        return !bids.empty() && !asks.empty() && bids.key_at(0) >= ~asks.key_at(0);
    }

    LevelSide bids;
    LevelSide asks;
};

/// Result of applying one ITCH order event.
struct BookUpdate {
    OrderBook* book        = nullptr;  ///< nullptr: the event referenced an unknown order
    uint32_t   price       = 0;
    uint16_t   locate      = 0;
    uint8_t    side        = 0;
    bool       top_changed = false;

    [[nodiscard]] explicit operator bool() const noexcept { return book != nullptr; }
};

/// Applies ITCH order events (A/F, E, C, X, D, U) to the global order map and
/// the per-locate level books.
class BookEngine {
public:
    static constexpr std::size_t kMaxLocate = 65536;

    struct Stats {
        uint64_t unknown_ref   = 0;  ///< E/C/X/D/U for a ref we never saw
        uint64_t missing_level = 0;  ///< order present but its level is not (bug indicator)
        uint64_t overfill      = 0;  ///< execute/cancel of more shares than rest
    };

    explicit BookEngine(std::size_t expected_orders = 1u << 20);

    ITCH_ALWAYS_INLINE BookUpdate add(uint16_t locate, uint64_t ref, uint8_t side,
                                      uint32_t shares, uint32_t price) {
        Order* o  = orders_.insert(ref);
        o->price  = price;
        o->shares = shares;
        o->locate = locate;
        o->side   = side;
        OrderBook& b = books_[locate];
        const bool top = b.side(side).add(OrderBook::key_for(side, price), shares);
        return {&b, price, locate, side, top};
    }

    /// Order Executed (E), Order Executed With Price (C) and Order Cancel (X)
    /// all reduce the resting size and delete the order when it reaches zero.
    ITCH_ALWAYS_INLINE BookUpdate reduce(uint64_t ref, uint32_t shares) noexcept {
        Order* o = orders_.find(ref);
        if (ITCH_UNLIKELY(o == nullptr)) { ++stats_.unknown_ref; return {}; }
        if (ITCH_UNLIKELY(shares > o->shares)) { ++stats_.overfill; shares = o->shares; }
        if (ITCH_UNLIKELY(shares == 0)) return {&books_[o->locate], o->price, o->locate, o->side, false};
        o->shares -= shares;
        const bool gone = o->shares == 0;
        const BookUpdate u = apply_reduce(*o, shares, gone);
        if (gone) orders_.erase(o);
        return u;
    }

    /// Order Delete (D).
    ITCH_ALWAYS_INLINE BookUpdate remove(uint64_t ref) noexcept {
        Order* o = orders_.find(ref);
        if (ITCH_UNLIKELY(o == nullptr)) { ++stats_.unknown_ref; return {}; }
        const BookUpdate u = apply_reduce(*o, o->shares, true);
        orders_.erase(o);
        return u;
    }

    /// Order Replace (U): the new order keeps the original side and locate
    /// (ITCH 5.0 section 1.3) and loses time priority.
    ITCH_ALWAYS_INLINE BookUpdate replace(uint64_t old_ref, uint64_t new_ref,
                                          uint32_t shares, uint32_t price) {
        Order* o = orders_.find(old_ref);
        if (ITCH_UNLIKELY(o == nullptr)) { ++stats_.unknown_ref; return {}; }
        const uint16_t locate = o->locate;
        const uint8_t  side   = o->side;
        const BookUpdate removed = apply_reduce(*o, o->shares, true);
        orders_.erase(o);
        BookUpdate added = add(locate, new_ref, side, shares, price);
        added.top_changed = added.top_changed || removed.top_changed;
        return added;
    }

    /// Software-prefetch hints for an upcoming event (see
    /// Parser::parse_stream_prefetch): the order-map group a ref hashes to,
    /// and a book's header. Pure hints: no effect on results.
    ITCH_ALWAYS_INLINE void prefetch_order(uint64_t ref) const noexcept { orders_.prefetch(ref); }
    ITCH_ALWAYS_INLINE void prefetch_book(uint16_t locate) const noexcept {
        __builtin_prefetch(&books_[locate]);
    }

    /// Pre-allocate level storage for a locate (called on Stock Directory).
    void prepare(uint16_t locate) {
        books_[locate].bids.reserve_initial();
        books_[locate].asks.reserve_initial();
    }

    [[nodiscard]] OrderBook& book(uint16_t locate) noexcept { return books_[locate]; }
    [[nodiscard]] const OrderBook& book(uint16_t locate) const noexcept { return books_[locate]; }
    [[nodiscard]] const OrderMap& orders() const noexcept { return orders_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    ITCH_ALWAYS_INLINE BookUpdate apply_reduce(const Order& o, uint32_t shares, bool gone) noexcept {
        OrderBook& b = books_[o.locate];
        const auto r = b.side(o.side).reduce(OrderBook::key_for(o.side, o.price), shares, gone);
        if (ITCH_UNLIKELY(r == LevelSide::Reduce::kMissingLevel)) ++stats_.missing_level;
        return {&b, o.price, o.locate, o.side, r == LevelSide::Reduce::kTopChanged};
    }

    OrderMap                     orders_;
    std::unique_ptr<OrderBook[]> books_;
    Stats                        stats_{};
};

} // namespace itch
