#pragma once

/// ITCH 5.0 decoder with compile-time handler binding.
///
/// Parser<Handler> calls handler.on(const MsgX&) for every message type the
/// handler has an overload for. The check is a C++20 requires-expression
/// evaluated per type at compile time, so:
///   * handled types decode and call straight into the handler (inlinable;
///     no std::function, no virtual call);
///   * unhandled types are validated and counted but never decoded.
///
/// Dispatch goes through a 256-entry constexpr table of function pointers
/// indexed by the type byte. That is one indirect branch per message; its
/// target is predicted from the recent type history, which on real ITCH data
/// is dominated by A/D/U/X/E runs.
///
/// Those runs are short, though, so the target is often mispredicted, and
/// the branch is most of a message's cost when the handler does little (about
/// 4 of 7 ns per message on the 2019-01-30 Nasdaq day, parse-only, measured
/// under Rosetta). A handler may therefore declare
/// `bool wants(uint8_t type, uint16_t locate) const`: messages it rejects are
/// validated and counted but never dispatched, so a handler that follows a
/// few symbols skips the branch for all the others.
///
/// Every message length is checked against the spec (kMessageLength) before
/// decoding, so a truncated or corrupt frame can't cause an out-of-bounds
/// read.

#include "itch/bytes.hpp"
#include "itch/messages.hpp"
#include "itch/platform.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace itch {

template <class Handler, class M>
concept HandlesMessage = requires(Handler& h, const M& m) { h.on(m); };

/// A handler that wants only some messages (see the file comment).
template <class Handler>
concept Filtering = requires(const Handler& h, uint8_t type, uint16_t locate) {
    { h.wants(type, locate) } -> std::convertible_to<bool>;
};

/// A handler that can issue software prefetches for a record ahead of time.
template <class Handler>
concept Prefetching = requires(const Handler& h, const uint8_t* p, std::size_t n) { h.prefetch(p, n); };

template <class Handler>
class Parser {
public:
    struct Stats {
        uint64_t messages     = 0;  ///< well-formed messages of a known type
        uint64_t unknown_type = 0;
        uint64_t bad_length   = 0;
        uint64_t empty        = 0;  ///< zero-length frames (legal in MoldUDP64)
        uint64_t filtered     = 0;  ///< well-formed, but rejected by handler.wants()
        std::array<uint64_t, 256> by_type{};
    };

    explicit Parser(Handler& handler) noexcept : handler_(handler) {}

    /// Parse one message (msg[0] is the type byte).
    ITCH_ALWAYS_INLINE void parse(const uint8_t* msg, std::size_t len) noexcept {
        if (ITCH_UNLIKELY(len == 0)) { ++stats_.empty; return; }
        const uint8_t type = msg[0];
        const uint16_t expected = kMessageLength[type];
        if (ITCH_UNLIKELY(expected != len)) {
            if (expected == 0) ++stats_.unknown_type;
            else               ++stats_.bad_length;
            return;
        }
        ++stats_.messages;
        ++stats_.by_type[type];
        if constexpr (Filtering<Handler>) {
            if (!handler_.wants(type, load_be16(msg + 1))) { ++stats_.filtered; return; }
        }
        static constexpr auto kTable = make_dispatch_table();
        kTable[type](handler_, msg);
    }

    /// Parse a buffer of [u16 big-endian length][message] records: the Nasdaq
    /// binary file format and the MoldUDP64 message-block layout. Returns the
    /// bytes consumed; a trailing partial record is left unconsumed.
    std::size_t parse_stream(const uint8_t* buf, std::size_t len) noexcept {
        std::size_t off = 0;
        while (off + 2 <= len) {
            const std::size_t n = load_be16(buf + off);
            if (ITCH_UNLIKELY(off + 2 + n > len)) break;
            parse(buf + off + 2, n);
            off += 2 + n;
        }
        return off;
    }

    /// parse_stream with a software-prefetch lookahead: while record i is
    /// parsed, handler.prefetch() is called for record i + Distance, so the
    /// cache misses of the next Distance records overlap instead of arriving
    /// one after another. Distance should cover memory latency: roughly
    /// (DRAM latency) / (per-message time), so 8-32 for this workload.
    /// Measure it (bench/bm_parse.cpp: BM_Parse_Prefetch) rather than guess.
    /// Results are identical to parse_stream.
    template <std::size_t Distance>
    std::size_t parse_stream_prefetch(const uint8_t* buf, std::size_t len) noexcept {
        if constexpr (Distance == 0 || !Prefetching<Handler>) {
            return parse_stream(buf, len);
        } else {
            std::size_t ahead = 0;  // next record to prefetch
            const auto prefetch_next = [&]() noexcept {
                if (ahead + 2 > len) return;
                const std::size_t n = load_be16(buf + ahead);
                if (ahead + 2 + n > len) return;
                handler_.prefetch(buf + ahead + 2, n);
                ahead += 2 + n;
            };
            for (std::size_t i = 0; i < Distance; ++i) prefetch_next();
            std::size_t off = 0;
            while (off + 2 <= len) {
                const std::size_t n = load_be16(buf + off);
                if (ITCH_UNLIKELY(off + 2 + n > len)) break;
                parse(buf + off + 2, n);
                off += 2 + n;
                prefetch_next();
            }
            return off;
        }
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] Handler& handler() noexcept { return handler_; }

private:
    using DispatchFn = void (*)(Handler&, const uint8_t*);

    static void ignore(Handler&, const uint8_t*) noexcept {}

    template <class M>
    static void dispatch(Handler& h, const uint8_t* msg) noexcept {
        h.on(decode<M>(msg));
    }

    template <class M>
    static constexpr DispatchFn entry() noexcept {
        if constexpr (HandlesMessage<Handler, M>) return &dispatch<M>;
        else                                      return &ignore;
    }

    static constexpr std::array<DispatchFn, 256> make_dispatch_table() noexcept {
        std::array<DispatchFn, 256> t{};
        t.fill(&ignore);
        [&]<class... Ms>(TypeList<Ms...>) {
            ((t[static_cast<uint8_t>(Ms::kType)] = entry<Ms>()), ...);
        }(AllMessages{});
        return t;
    }

    Handler& handler_;
    Stats    stats_{};
};

} // namespace itch
