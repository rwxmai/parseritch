// bbalouki/itchcpp runner.  run_itchcpp FILE (parse|overlay|book)
//   parse    Parser::parse(span, callback): eager decode into itch::Message
//   overlay  overlay::for_each_message: lazy views, reads locate+timestamp only
//   book     Parser + BookManager::process: full L3 books for every symbol
#include "common.hpp"

#include "itch/book/book_manager.hpp"
#include "itch/overlay.hpp"
#include "itch/parser.hpp"

#include <span>
#include <string>
#include <variant>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s FILE parse|overlay|book\n", argv[0]); return 2; }
    const std::string mode = argv[2];
    const cmp::Buffer buf = cmp::load(argv[1]);
    const std::span<const std::byte> data{reinterpret_cast<const std::byte*>(buf.data), buf.size};

    uint64_t n = 0, sum = 0;
    if (mode == "parse") {
        itch::Parser p;
        const auto t0 = cmp::Clock::now();
        p.parse(data, [&](const itch::Message& msg) {
            ++n;
            sum += std::visit([](const auto& m) -> uint64_t { return m.stock_locate + m.timestamp; }, msg);
        });
        cmp::result("itchcpp", "parse", n, cmp::seconds_since(t0), buf.size, sum);
        return 0;
    }
    if (mode == "overlay") {
        const auto t0 = cmp::Clock::now();
        itch::overlay::for_each_message(data, [&](const itch::overlay::MessageView& v) {
            ++n;
            sum += v.stock_locate() + v.timestamp();
        });
        cmp::result("itchcpp", "overlay", n, cmp::seconds_since(t0), buf.size, sum);
        return 0;
    }

    itch::Parser p;
    itch::book::BookManager mgr;
    const auto t0 = cmp::Clock::now();
    p.parse(data, [&](const itch::Message& msg) { ++n; mgr.process(msg); });
    const double s = cmp::seconds_since(t0);
    cmp::result("itchcpp", "book", n, s, buf.size, 0, "books=" + std::to_string(mgr.book_count()));
    for (const auto& sym : cmp::digest_symbols()) {
        const auto* b = mgr.book_for_symbol(sym);
        if (b == nullptr) continue;
        const itch::book::Bbo q = b->bbo();
        cmp::print_bbo("itchcpp", sym, q.has_bid ? q.bid_price.raw() : 0, q.has_bid ? q.bid_shares : 0,
                       q.has_ask ? q.ask_price.raw() : 0, q.has_ask ? q.ask_shares : 0);
    }
    return 0;
}
