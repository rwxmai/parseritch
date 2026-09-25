// itch_slice IN OUT (--until-ns T | --skip N --count M)
//   --until-ns T     copy every record up to the first with timestamp >= T
//                    (a "noon cut" file for cross-checking books mid-session)
//   --skip N --count M  copy records [N, N+M): a frame-aligned slice for the
//                    slow (Python) parsers
// Also prints the reference message count and per-type counts of IN's copy.
#include "common.hpp"

#include <array>
#include <string>

int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: %s IN OUT (--until-ns T | --skip N --count M)\n", argv[0]); return 2; }
    const cmp::Buffer b = cmp::load(argv[1]);
    uint64_t until = UINT64_MAX, skip = 0, count = UINT64_MAX;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string o = argv[i];
        const uint64_t v = std::strtoull(argv[i + 1], nullptr, 10);
        if (o == "--until-ns") until = v; else if (o == "--skip") skip = v; else if (o == "--count") count = v;
    }
    std::size_t off = 0, begin = SIZE_MAX, end = b.size;
    uint64_t idx = 0;
    std::array<uint64_t, 256> by_type{};
    while (off + 2 <= b.size) {
        const std::size_t n = cmp::be16(b.data + off);
        const uint8_t* m = b.data + off + 2;
        uint64_t ts = 0;
        for (int k = 0; k < 6; ++k) ts = (ts << 8) | m[5 + k];
        if (ts >= until || idx >= skip + count) { end = off; break; }
        if (idx == skip) begin = off;
        if (idx >= skip) ++by_type[m[0]];
        off += 2 + n;
        ++idx;
    }
    if (begin == SIZE_MAX) { std::fprintf(stderr, "empty slice\n"); return 1; }
    std::FILE* f = std::fopen(argv[2], "wb");
    std::fwrite(b.data + begin, 1, end - begin, f);
    std::fclose(f);
    uint64_t total = 0;
    std::printf("wrote %s: %zu bytes; types:", argv[2], end - begin);
    for (int t = 0; t < 256; ++t) if (by_type[t]) { std::printf(" %c=%llu", t, (unsigned long long)by_type[t]); total += by_type[t]; }
    std::printf("\nmessages=%llu\n", (unsigned long long)total);
    return 0;
}
