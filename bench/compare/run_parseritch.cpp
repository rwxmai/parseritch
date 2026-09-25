// parseritch runner.  run_parseritch FILE (parse|frames|book|book_pf{8,16,32,64})   [env ITCH_ORDERS]
//   parse     decode every message (all 23 types), fold locate+timestamp
//   frames    for_each_frame: validate every record, fold locate+timestamp, no dispatch
//   book      Parser<BookBuilder> full-depth books for every symbol
//   book_pfN  same, with parse_stream_prefetch<N>
#include "common.hpp"

#include "itch/book_builder.hpp"
#include "itch/order_book.hpp"
#include "itch/parser.hpp"
#include "itch/symbol_directory.hpp"

#include <array>
#include <memory>
#include <string>

namespace {

#if defined(__AVX2__)
constexpr const char* kLib = "parseritch_avx2";
#else
constexpr const char* kLib = "parseritch";
#endif

struct Fold {
    uint64_t sum = 0;
    template <class M>
    ITCH_ALWAYS_INLINE void on(const M& m) noexcept { sum += m.locate + m.timestamp; }
};

struct DirSink {
    itch::SymbolDirectory* dir;
    void on(const itch::MsgStockDirectory& m) { dir->on(m); }
};

/// Books only the digest symbols, through BookBuilder's wants() forwarding.
struct FilterSink : DirSink {
    std::array<bool, 65536>* keep;
    bool wants(uint8_t type, uint16_t locate) const { return type == 'R' || (*keep)[locate]; }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s FILE parse|frames|book|book_pfN\n", argv[0]); return 2; }
    const std::string mode = argv[2];
    const cmp::Buffer buf = cmp::load(argv[1]);

    if (mode == "frames") {
        uint64_t sum = 0;
        const auto t0 = cmp::Clock::now();
        const itch::FrameScan r = itch::for_each_frame(buf.data, buf.size, [&](const itch::Frame& f) {
            sum += f.locate() + f.timestamp();
        });
        const double s = cmp::seconds_since(t0);
        cmp::result(kLib, "frames", r.frames, s, buf.size, sum);
        return 0;
    }

    if (mode == "parse") {
        Fold f;
        itch::Parser<Fold> p(f);
        const auto t0 = cmp::Clock::now();
        const std::size_t used = p.parse_stream(buf.data, buf.size);
        const double s = cmp::seconds_since(t0);
        if (used != buf.size) std::fprintf(stderr, "warning: %zu trailing bytes\n", buf.size - used);
        cmp::result(kLib, "parse", p.stats().messages, s, buf.size, f.sum);
        return 0;
    }

    const char* orders_env = std::getenv("ITCH_ORDERS");  // expected peak live orders (feed_handler --orders)
    if (mode == "book_pf16_digest") {  // wants(): the 8 digest symbols only
        auto keep = std::make_unique<std::array<bool, 65536>>();
        for (const auto& sym : cmp::digest_symbols())
            if (const int loc = cmp::locate_of(buf, sym); loc >= 0) (*keep)[static_cast<std::size_t>(loc)] = true;
        auto engine = std::make_unique<itch::BookEngine>(1u << 16);
        auto dir = std::make_unique<itch::SymbolDirectory>();
        FilterSink sink{{dir.get()}, keep.get()};
        itch::BookBuilder<FilterSink> builder(*engine, sink);
        itch::Parser<itch::BookBuilder<FilterSink>> p(builder);
        const auto t0 = cmp::Clock::now();
        p.parse_stream_prefetch<16>(buf.data, buf.size);
        const double s = cmp::seconds_since(t0);
        cmp::result(kLib, mode.c_str(), p.stats().messages, s, buf.size, 0,
                    "filtered=" + std::to_string(p.stats().filtered));
        return 0;
    }

    auto engine = std::make_unique<itch::BookEngine>(orders_env ? std::strtoull(orders_env, nullptr, 10) : (8u << 20));
    auto dir = std::make_unique<itch::SymbolDirectory>();
    DirSink sink{dir.get()};
    itch::BookBuilder<DirSink> builder(*engine, sink);
    itch::Parser<itch::BookBuilder<DirSink>> p(builder);
    const auto t0 = cmp::Clock::now();
    if (mode == "book_pf8")       p.parse_stream_prefetch<8>(buf.data, buf.size);
    else if (mode == "book_pf16") p.parse_stream_prefetch<16>(buf.data, buf.size);
    else if (mode == "book_pf32") p.parse_stream_prefetch<32>(buf.data, buf.size);
    else if (mode == "book_pf64") p.parse_stream_prefetch<64>(buf.data, buf.size);
    else                          p.parse_stream(buf.data, buf.size);
    const double s = cmp::seconds_since(t0);
    const auto& st = engine->stats();
    cmp::result(kLib, mode.c_str(), p.stats().messages, s, buf.size, 0,
                "live_orders=" + std::to_string(engine->orders().size()) +
                " unknown_ref=" + std::to_string(st.unknown_ref) +
                " missing_level=" + std::to_string(st.missing_level));
    for (const auto& sym : cmp::digest_symbols()) {
        const auto loc = dir->locate(sym);
        if (!loc) continue;
        const itch::TopOfBook t = engine->book(*loc).top();
        cmp::print_bbo(kLib, sym, t.bid_price, t.bid_qty, t.ask_price, t.ask_qty);
    }
    return 0;
}
