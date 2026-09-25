#pragma once

/// Stock locate <-> symbol mapping, filled from Stock Directory ('R') messages.
///
/// Off the hot path: the directory is sent once, before trading starts. The
/// forward map is a flat array indexed by locate. The reverse map keys on the
/// 8-byte space-padded symbol packed into a uint64. That fixes the old
/// StockLocate, whose lookup hashed the unpadded query ("AAPL") while entries
/// had been hashed padded ("AAPL    "), so lookups never matched.

#include "itch/messages.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace itch {

class SymbolDirectory {
public:
    SymbolDirectory() : symbols_(std::make_unique<Alpha<8>[]>(kMaxLocate)) {
        for (std::size_t i = 0; i < kMaxLocate; ++i) symbols_[i].fill(' ');  // unknown -> ""
    }

    void on(const MsgStockDirectory& m) { add(m.locate, m.stock); }

    void add(uint16_t locate, const Alpha<8>& symbol) {
        symbols_[locate] = symbol;
        by_symbol_[pack(symbol)] = locate;
    }

    /// Trimmed symbol for a locate; empty if unknown.
    [[nodiscard]] std::string_view symbol(uint16_t locate) const noexcept {
        return trimmed(symbols_[locate]);
    }

    [[nodiscard]] std::optional<uint16_t> locate(std::string_view symbol) const {
        if (symbol.size() > 8) return std::nullopt;
        Alpha<8> padded;
        padded.fill(' ');
        std::memcpy(padded.data(), symbol.data(), symbol.size());
        const auto it = by_symbol_.find(pack(padded));
        if (it == by_symbol_.end()) return std::nullopt;
        return it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return by_symbol_.size(); }

private:
    static constexpr std::size_t kMaxLocate = 65536;

    static uint64_t pack(const Alpha<8>& s) noexcept {
        uint64_t v;
        std::memcpy(&v, s.data(), 8);
        return v;
    }

    std::unique_ptr<Alpha<8>[]>            symbols_;
    std::unordered_map<uint64_t, uint16_t> by_symbol_;
};

} // namespace itch
