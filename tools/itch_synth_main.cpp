/// itch_synth: write a deterministic synthetic ITCH 5.0 session in the Nasdaq
/// binary file format ([u16 length][message]...), for trying feed_handler
/// without the multi-GB sample download.
///
///   itch_synth OUT_FILE [--events N] [--symbols N] [--seed N]

#include "itch/sim/synthetic_feed.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        std::fprintf(stderr, "Usage: %s OUT_FILE [--events N] [--symbols N] [--seed N]\n", argv[0]);
        return argc < 2 ? 2 : 0;
    }
    itch::sim::SyntheticFeedConfig cfg;
    cfg.events = 5'000'000;
    cfg.symbols = 1024;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string opt = argv[i];
        const auto v = std::strtoull(argv[i + 1], nullptr, 10);
        if (opt == "--events") cfg.events = v;
        else if (opt == "--symbols") cfg.symbols = static_cast<uint32_t>(v);
        else if (opt == "--seed") cfg.seed = v;
        else { std::fprintf(stderr, "unknown option %s\n", opt.c_str()); return 2; }
    }
    if (cfg.symbols == 0 || cfg.symbols > 65535) {
        std::fprintf(stderr, "--symbols must be in 1..65535 (stock locates are 16-bit, 0 is reserved)\n");
        return 2;
    }
    itch::sim::SyntheticFeed feed(cfg);
    const auto bytes = feed.build();
    std::FILE* f = std::fopen(argv[1], "wb");
    if (f == nullptr) { std::perror(argv[1]); return 1; }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!ok) { std::perror("write"); return 1; }
    std::printf("wrote %s: %llu messages, %zu bytes\n", argv[1],
                static_cast<unsigned long long>(feed.messages()), bytes.size());
    return 0;
}
