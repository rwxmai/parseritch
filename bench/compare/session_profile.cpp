// session_profile FILE [SLICE_MSGS]  ->  CSV on stdout
//
// Builds full-depth books for the whole file (parse_stream_prefetch<16>, as
// feed_handler replays by default) in slices of SLICE_MSGS records (default
// 2M) and prints, per slice, the session time of its last message and the
// slice's throughput: how the engine keeps up across a trading day.
#include "common.hpp"

#include "itch/book_builder.hpp"
#include "itch/parser.hpp"

#include <memory>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s FILE [SLICE_MSGS]\n", argv[0]); return 2; }
    const std::size_t slice_msgs = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2'000'000;
    const cmp::Buffer buf = cmp::load(argv[1]);

    // Slice boundaries first, so the timed loop does nothing but parse.
    std::vector<std::size_t> cuts{0};
    std::vector<uint64_t> last_ts;
    std::size_t off = 0, n = 0;
    uint64_t ts = 0;
    while (off + 2 <= buf.size) {
        const std::size_t len = cmp::be16(buf.data + off);
        if (off + 2 + len > buf.size) break;
        const uint8_t* m = buf.data + off + 2;
        ts = 0;
        for (int k = 0; k < 6; ++k) ts = (ts << 8) | m[5 + k];
        off += 2 + len;
        if (++n % slice_msgs == 0) { cuts.push_back(off); last_ts.push_back(ts); }
    }
    if (cuts.back() != off) { cuts.push_back(off); last_ts.push_back(ts); }

    auto engine = std::make_unique<itch::BookEngine>(8u << 20);
    itch::NullSink sink;
    itch::BookBuilder<itch::NullSink> builder(*engine, sink);
    itch::Parser<itch::BookBuilder<itch::NullSink>> p(builder);
    std::vector<double> secs(cuts.size() - 1);
    std::vector<uint64_t> msgs(cuts.size() - 1);
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const uint64_t before = p.stats().messages;
        const auto t0 = cmp::Clock::now();
        p.parse_stream_prefetch<16>(buf.data + cuts[i], cuts[i + 1] - cuts[i]);
        secs[i] = cmp::seconds_since(t0);
        msgs[i] = p.stats().messages - before;
    }
    std::printf("session_time_s,messages,seconds,mmsg_s,ns_per_msg\n");
    for (std::size_t i = 0; i < secs.size(); ++i)
        std::printf("%.3f,%llu,%.6f,%.3f,%.3f\n", double(last_ts[i]) / 1e9, (unsigned long long)msgs[i], secs[i],
                    double(msgs[i]) / secs[i] / 1e6, secs[i] * 1e9 / double(msgs[i]));
    std::fprintf(stderr, "unknown_ref=%llu missing_level=%llu\n", (unsigned long long)engine->stats().unknown_ref,
                 (unsigned long long)engine->stats().missing_level);
    return 0;
}
