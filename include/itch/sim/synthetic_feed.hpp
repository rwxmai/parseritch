#pragma once

/// Deterministic synthetic ITCH 5.0 stream generator for tests and benchmarks.
///
/// The stream is self-consistent: every execute, cancel, delete and replace
/// references a live order, and the stream ends by deleting every remaining
/// order, so replaying it from an empty engine always returns the engine to
/// empty. A benchmark can therefore replay it repeatedly without resetting
/// state between iterations. (The old benchmark replayed the same adds into
/// books that already held them.)
///
/// Shape, roughly after the published Nasdaq message mix:
///   * symbol activity is Zipf-distributed (a few names dominate);
///   * new orders land a geometric number of ticks behind the touch, so most
///     activity is at or near the top of the book;
///   * message mix: ~42% A/F, ~36% D, ~10% U, ~5% X, ~5% E/C, ~2% P.
/// It is still synthetic: use the free Nasdaq full-day files for real numbers.

#include "itch/messages.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace itch::sim {

struct SyntheticFeedConfig {
    uint32_t symbols        = 256;        ///< 1..65535 (locate = index + 1)
    uint64_t events         = 1'000'000;  ///< order events before the final drain
    uint32_t initial_orders = 64;         ///< resting orders seeded per symbol
    double   zipf_s         = 1.1;
    double   depth_p        = 0.35;       ///< geometric parameter for ticks behind the touch
    uint64_t seed           = 0x17C4'5EED;
};

class SyntheticFeed {
public:
    explicit SyntheticFeed(const SyntheticFeedConfig& cfg = {}) : cfg_(cfg), rng_(cfg.seed) {
        assert(cfg.symbols >= 1 && cfg.symbols <= 65535 && "locates are 16-bit and 0 is reserved");
    }

    /// Build the full stream as [u16 BE length][message] records.
    std::vector<uint8_t> build() {
        out_.clear();
        init_symbols();
        emit_system('O');
        for (uint32_t s = 0; s < cfg_.symbols; ++s) emit_directory(s);
        emit_system('Q');
        for (uint32_t s = 0; s < cfg_.symbols; ++s)
            for (uint32_t i = 0; i < cfg_.initial_orders; ++i) add_order(s);
        for (uint64_t e = 0; e < cfg_.events; ++e) step();
        drain();
        emit_system('M');
        emit_system('C');
        return std::move(out_);
    }

    [[nodiscard]] uint64_t messages() const noexcept { return messages_; }

private:
    struct Live {
        uint64_t ref;
        uint32_t price;
        uint32_t shares;
        char     side;
    };
    struct Symbol {
        uint32_t          mid;  // Price(4); spread is 2 ticks around it
        std::vector<Live> live;
    };
    static constexpr uint32_t kTick = 100;  // $0.01

    void init_symbols() {
        syms_.assign(cfg_.symbols, {});
        std::uniform_int_distribution<uint32_t> px(10 * 100, 500 * 100);  // $10..$500 in cents
        for (auto& s : syms_) s.mid = px(rng_) * kTick;
        cdf_.resize(cfg_.symbols);
        double acc = 0;
        for (uint32_t i = 0; i < cfg_.symbols; ++i) {
            acc += 1.0 / std::pow(static_cast<double>(i + 1), cfg_.zipf_s);
            cdf_[i] = acc;
        }
        for (auto& c : cdf_) c /= acc;
    }

    uint32_t pick_symbol() {
        const double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
        const auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return static_cast<uint32_t>(std::min<std::ptrdiff_t>(it - cdf_.begin(), cfg_.symbols - 1));
    }

    uint32_t pick_price(const Symbol& s, char side) {
        const uint32_t behind = std::geometric_distribution<uint32_t>(cfg_.depth_p)(rng_);
        return side == 'B' ? s.mid - kTick * (1 + behind) : s.mid + kTick * (1 + behind);
    }

    uint32_t pick_shares() { return 100 * std::uniform_int_distribution<uint32_t>(1, 10)(rng_); }

    void step() {
        const uint32_t sym = pick_symbol();
        Symbol& s = syms_[sym];
        const double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
        if (s.live.empty() || u < 0.42) { add_order(sym); return; }
        const std::size_t idx = std::uniform_int_distribution<std::size_t>(0, s.live.size() - 1)(rng_);
        if (u < 0.78)      delete_order(sym, idx);
        else if (u < 0.88) replace_order(sym, idx);
        else if (u < 0.93) cancel_order(sym, idx);
        else if (u < 0.98) execute_order(sym, idx);
        else               trade(sym);
        // Occasional one-tick drift of the mid keeps prices from being static.
        if (u > 0.9995)     s.mid += kTick;
        else if (u > 0.999) s.mid -= kTick;
    }

    void add_order(uint32_t sym) {
        Symbol& s = syms_[sym];
        const char side = coin() ? 'B' : 'S';
        const Live o{next_ref_++, pick_price(s, side), pick_shares(), side};
        s.live.push_back(o);
        if (coin(0.05)) {
            MsgAddOrderMpid m;
            fill_header(m, sym);
            m.ref = o.ref; m.side = side; m.shares = o.shares; m.price = o.price;
            m.stock = stock_of(sym);
            std::memcpy(m.attribution.data(), "MPID", 4);
            write(m);
        } else {
            MsgAddOrder m;
            fill_header(m, sym);
            m.ref = o.ref; m.side = side; m.shares = o.shares; m.price = o.price;
            m.stock = stock_of(sym);
            write(m);
        }
    }

    void delete_order(uint32_t sym, std::size_t idx) {
        MsgOrderDelete m;
        fill_header(m, sym);
        m.ref = syms_[sym].live[idx].ref;
        write(m);
        erase_live(sym, idx);
    }

    void replace_order(uint32_t sym, std::size_t idx) {
        Symbol& s = syms_[sym];
        Live& o = s.live[idx];
        MsgOrderReplace m;
        fill_header(m, sym);
        m.original_ref = o.ref;
        m.new_ref      = next_ref_++;
        m.shares       = pick_shares();
        m.price        = pick_price(s, o.side);
        write(m);
        o.ref = m.new_ref; o.shares = m.shares; o.price = m.price;
    }

    void cancel_order(uint32_t sym, std::size_t idx) {
        Live& o = syms_[sym].live[idx];
        MsgOrderCancel m;
        fill_header(m, sym);
        m.ref = o.ref;
        m.cancelled_shares = coin() ? o.shares : std::max<uint32_t>(1, o.shares / 2);
        write(m);
        o.shares -= m.cancelled_shares;
        if (o.shares == 0) erase_live(sym, idx);
    }

    void execute_order(uint32_t sym, std::size_t idx) {
        Live& o = syms_[sym].live[idx];
        const uint32_t qty = coin() ? o.shares : std::max<uint32_t>(1, o.shares / 3);
        if (coin(0.2)) {
            MsgOrderExecutedWithPrice m;
            fill_header(m, sym);
            m.ref = o.ref; m.executed_shares = qty; m.match_number = next_match_++;
            m.printable = 'Y'; m.execution_price = o.price;
            write(m);
        } else {
            MsgOrderExecuted m;
            fill_header(m, sym);
            m.ref = o.ref; m.executed_shares = qty; m.match_number = next_match_++;
            write(m);
        }
        o.shares -= qty;
        if (o.shares == 0) erase_live(sym, idx);
    }

    void trade(uint32_t sym) {
        MsgTrade m;
        fill_header(m, sym);
        m.ref = 0; m.side = 'B'; m.shares = pick_shares(); m.stock = stock_of(sym);
        m.price = syms_[sym].mid; m.match_number = next_match_++;
        write(m);
    }

    void drain() {
        for (uint32_t sym = 0; sym < cfg_.symbols; ++sym)
            while (!syms_[sym].live.empty()) delete_order(sym, syms_[sym].live.size() - 1);
    }

    void erase_live(uint32_t sym, std::size_t idx) {
        auto& v = syms_[sym].live;
        v[idx] = v.back();
        v.pop_back();
    }

    void emit_system(char code) {
        MsgSystemEvent m;
        m.timestamp = ts_ += 1000;
        m.event_code = code;
        write(m);
    }

    void emit_directory(uint32_t sym) {
        MsgStockDirectory m;
        fill_header(m, sym);
        m.stock = stock_of(sym);
        m.market_category = 'Q';
        m.financial_status = 'N';
        m.round_lot_size = 100;
        m.round_lots_only = 'N';
        m.issue_classification = 'C';
        m.issue_subtype = {'Z', ' '};
        m.authenticity = 'P';
        m.short_sale_threshold = 'N';
        m.ipo_flag = 'N';
        m.luld_ref_price_tier = '1';
        m.etp_flag = 'N';
        m.inverse_indicator = 'N';
        write(m);
    }

    template <class M>
    void fill_header(M& m, uint32_t sym) {
        m.locate    = static_cast<uint16_t>(sym + 1);
        m.tracking  = 0;
        m.timestamp = ts_ += 1 + (rng_() & 0xFF);
    }

    static Alpha<8> stock_of(uint32_t sym) {
        Alpha<8> a;
        a.fill(' ');
        const std::string name = "S" + std::to_string(sym);
        std::memcpy(a.data(), name.data(), std::min<std::size_t>(name.size(), 8));
        return a;
    }

    template <class M>
    void write(const M& m) {
        const std::size_t at = out_.size();
        out_.resize(at + 2 + M::kLength);
        store_be16(out_.data() + at, static_cast<uint16_t>(M::kLength));
        encode(m, out_.data() + at + 2);
        ++messages_;
    }

    bool coin(double p = 0.5) { return std::bernoulli_distribution(p)(rng_); }

    SyntheticFeedConfig  cfg_;
    std::mt19937_64      rng_;
    std::vector<Symbol>  syms_;
    std::vector<double>  cdf_;
    std::vector<uint8_t> out_;
    uint64_t next_ref_   = 1;
    uint64_t next_match_ = 1;
    uint64_t ts_         = 34'200'000'000'000ULL;  // 09:30:00
    uint64_t messages_   = 0;
};

} // namespace itch::sim
