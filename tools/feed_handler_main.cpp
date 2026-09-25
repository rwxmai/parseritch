/// feed_handler: ITCH 5.0 book builder, from a file or a live MoldUDP64 feed.
///
///   feed_handler --replay 01302019.NASDAQ_ITCH50 [--latency] [--symbol AAPL]
///                [--prefetch 16] [--perf] [--depth-profile]
///   feed_handler --mcast 233.54.12.111:26477 [--iface 10.0.0.5] [--stats 5]
///   feed_handler --mcast 233.54.12.111:26477 --xdp eth0:3 [--xdp-native]
///   Options: --cpu-feed N --cpu-consumer N  pin threads (Linux)
///
/// Threads:
///   feed thread      recv -> MoldUDP64 sequencer -> Parser -> BookEngine
///                    -> per-locate BboCache (seqlock) + SpscRing<BookEvent>
///   consumer thread  drains the ring (the strategy hook)
///
/// Replay mode prints a correctness report for the whole session: unknown
/// order references, missing levels, crossed books during market hours, and
/// peak live orders.

#include "itch/bbo_cache.hpp"
#include "itch/book_builder.hpp"
#include "itch/book_event.hpp"
#include "itch/clock.hpp"
#include "itch/depth_profiler.hpp"
#include "itch/histogram.hpp"
#include "itch/mold_udp64.hpp"
#include "itch/parser.hpp"
#include "itch/perf_counters.hpp"
#include "itch/spsc_ring.hpp"
#include "itch/symbol_directory.hpp"
#include "net/itch_file.hpp"
#include "net/udp_receiver.hpp"
#if defined(ITCH_HAVE_XDP)
#  include "net/xdp_receiver.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#endif

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    std::string replay_path;
    std::string mcast_group;       ///< empty with a port = unicast
    uint16_t    mcast_port = 0;
    std::string iface_ip;
    std::string xdp_iface;         ///< --xdp IFACE[:QUEUE]: AF_XDP instead of a UDP socket
    uint32_t    xdp_queue = 0;
    bool        xdp_native = false;
    bool        xdp_zero_copy = false;
    std::string xdp_prog;
    std::string symbol;
    int         stats_secs = 5;
    int         cpu_feed = -1;
    int         cpu_consumer = -1;
    bool        latency = false;
    bool        perf = false;
    bool        depth_profile = false;
    int         prefetch = 0;
    std::size_t expected_orders = 8u << 20;
};

[[noreturn]] void usage(const char* argv0, int code) {
    std::fprintf(code == 0 ? stdout : stderr,
                 "Usage: %s (--replay FILE | --mcast GROUP:PORT | --port PORT) [options]\n"
                 "  --replay FILE        replay a Nasdaq ITCH 5.0 binary file (gunzipped)\n"
                 "  --mcast GROUP:PORT   receive a live MoldUDP64 multicast feed\n"
                 "  --port PORT          receive a live MoldUDP64 unicast feed on PORT\n"
                 "  --iface IP           local interface address for the multicast join\n"
                 "  --xdp IFACE[:QUEUE]  (live) receive through AF_XDP on IFACE's rx QUEUE (default 0)\n"
                 "  --xdp-native         (live) driver-mode XDP instead of generic (SKB) mode\n"
                 "  --xdp-zerocopy       (live) zero-copy AF_XDP bind (needs driver support)\n"
                 "  --xdp-prog PATH      (live) BPF object (default: the one built with this binary)\n"
                 "  --symbol SYM         print this symbol's top of book at the end\n"
                 "  --latency            (replay) per-message TSC latency histogram\n"
                 "  --prefetch N         (replay) software-prefetch lookahead: 0, 8, 16 or 32 records\n"
                 "  --perf               (replay) IPC and effective/nominal frequency ratio (Linux PMU)\n"
                 "  --depth-profile      (replay) where level operations land relative to the top of\n"
                 "                       book; use it to set ITCH_LEVEL_LINEAR_CHUNKS\n"
                 "  --stats SECS         (live) stats interval, default 5\n"
                 "  --orders N           expected peak live orders, default 8388608\n"
                 "  --cpu-feed N         pin the feed thread to CPU N (Linux)\n"
                 "  --cpu-consumer N     pin the consumer thread to CPU N (Linux)\n",
                 argv0);
    std::exit(code);
}

long parse_int(const char* s, const char* what, const char* argv0) {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0) {
        std::fprintf(stderr, "invalid %s: %s\n", what, s);
        usage(argv0, 2);
    }
    return v;
}

Config parse_args(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage(argv[0], 2);
            return argv[++i];
        };
        if (a == "--replay") c.replay_path = next();
        else if (a == "--mcast") {
            const std::string v = next();
            const auto colon = v.rfind(':');
            if (colon == std::string::npos) usage(argv[0], 2);
            c.mcast_group = v.substr(0, colon);
            c.mcast_port  = static_cast<uint16_t>(parse_int(v.c_str() + colon + 1, "port", argv[0]));
        }
        else if (a == "--iface") c.iface_ip = next();
        else if (a == "--port") c.mcast_port = static_cast<uint16_t>(parse_int(next(), "--port", argv[0]));
        else if (a == "--xdp") {
            const std::string v = next();
            const auto colon = v.rfind(':');
            c.xdp_iface = v.substr(0, colon);
            if (colon != std::string::npos)
                c.xdp_queue = static_cast<uint32_t>(parse_int(v.c_str() + colon + 1, "queue", argv[0]));
        }
        else if (a == "--xdp-native") c.xdp_native = true;
        else if (a == "--xdp-zerocopy") c.xdp_zero_copy = true;
        else if (a == "--xdp-prog") c.xdp_prog = next();
        else if (a == "--symbol") c.symbol = next();
        else if (a == "--latency") c.latency = true;
        else if (a == "--perf") c.perf = true;
        else if (a == "--depth-profile") c.depth_profile = true;
        else if (a == "--prefetch") {
            c.prefetch = static_cast<int>(parse_int(next(), "--prefetch", argv[0]));
            if (c.prefetch != 0 && c.prefetch != 8 && c.prefetch != 16 && c.prefetch != 32) {
                std::fprintf(stderr, "--prefetch must be 0, 8, 16 or 32\n");
                usage(argv[0], 2);
            }
        }
        else if (a == "--stats") c.stats_secs = static_cast<int>(parse_int(next(), "--stats", argv[0]));
        else if (a == "--orders") c.expected_orders = static_cast<std::size_t>(parse_int(next(), "--orders", argv[0]));
        else if (a == "--cpu-feed") c.cpu_feed = static_cast<int>(parse_int(next(), "--cpu-feed", argv[0]));
        else if (a == "--cpu-consumer") c.cpu_consumer = static_cast<int>(parse_int(next(), "--cpu-consumer", argv[0]));
        else if (a == "--help" || a == "-h") usage(argv[0], 0);
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(argv[0], 2); }
    }
    const bool live = c.mcast_port != 0;
    if (c.replay_path.empty() == !live) usage(argv[0], 2);  // exactly one of replay / live
    if (!c.xdp_iface.empty() && !live) {
        std::fprintf(stderr, "--xdp needs --mcast GROUP:PORT or --port PORT\n");
        usage(argv[0], 2);
    }
    return c;
}

void pin_current_thread(int cpu, const char* name) {
    if (cpu < 0) return;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);  // glibc macro takes size_t
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
        std::fprintf(stderr, "warning: could not pin %s thread to CPU %d\n", name, cpu);
#else
    std::fprintf(stderr, "warning: thread pinning is Linux-only (%s thread not pinned)\n", name);
#endif
}

// ---------------------------------------------------------------------------
// Sink: book updates -> BBO caches + event ring; session bookkeeping
// ---------------------------------------------------------------------------

using EventRing = itch::SpscRing<itch::BookEvent, 1 << 16>;

struct EventFields {
    itch::EventKind kind;
    uint64_t        ref;
    uint32_t        shares;
};
EventFields fields(const itch::MsgAddOrder& m) { return {itch::EventKind::kAdd, m.ref, m.shares}; }
EventFields fields(const itch::MsgAddOrderMpid& m) { return {itch::EventKind::kAdd, m.ref, m.shares}; }
EventFields fields(const itch::MsgOrderExecuted& m) { return {itch::EventKind::kExecute, m.ref, m.executed_shares}; }
EventFields fields(const itch::MsgOrderExecutedWithPrice& m) { return {itch::EventKind::kExecute, m.ref, m.executed_shares}; }
EventFields fields(const itch::MsgOrderCancel& m) { return {itch::EventKind::kCancel, m.ref, m.cancelled_shares}; }
EventFields fields(const itch::MsgOrderDelete& m) { return {itch::EventKind::kDelete, m.ref, 0}; }
EventFields fields(const itch::MsgOrderReplace& m) { return {itch::EventKind::kReplace, m.new_ref, m.shares}; }

class Publisher {
public:
    Publisher(itch::BookEngine& engine, EventRing& ring, itch::SymbolDirectory& dir)
        : engine_(engine), ring_(ring), dir_(dir),
          bbo_(std::make_unique<itch::BboCache[]>(itch::BookEngine::kMaxLocate)) {}

    template <class M>
    ITCH_ALWAYS_INLINE void on_book(const M& m, const itch::BookUpdate& u) {
        if (ITCH_UNLIKELY(!u)) return;
        if constexpr (std::is_same_v<M, itch::MsgAddOrder> || std::is_same_v<M, itch::MsgAddOrderMpid>) {
            const std::size_t live = engine_.orders().size();
            if (live > peak_live_) peak_live_ = live;
        }
        if (!u.top_changed) return;
        const itch::TopOfBook t = u.book->top();
        bbo_[u.locate].store({t.bid_price, t.ask_price, t.bid_qty, t.ask_qty, m.timestamp});
        if (market_open_ && u.book->crossed()) ++crossed_updates_;

        const EventFields f = fields(m);
        itch::BookEvent ev{};
        ev.timestamp = m.timestamp;
        ev.recv_tsc  = recv_tsc_;
        ev.ref       = f.ref;
        ev.bid_qty   = t.bid_qty;
        ev.ask_qty   = t.ask_qty;
        ev.price     = u.price;
        ev.shares    = f.shares;
        ev.bid_price = t.bid_price;
        ev.ask_price = t.ask_price;
        ev.locate    = u.locate;
        ev.kind      = f.kind;
        ev.side      = u.side;
        if (ITCH_UNLIKELY(!ring_.try_push(ev))) ++dropped_;
    }

    void on(const itch::MsgSystemEvent& m) {
        if (m.event_code == 'Q') market_open_ = true;   // start of market hours
        if (m.event_code == 'M') market_open_ = false;  // end of market hours
    }
    void on(const itch::MsgStockDirectory& m) { dir_.on(m); }

    void set_recv_tsc(uint64_t t) noexcept { recv_tsc_ = t; }
    [[nodiscard]] const itch::BboCache& bbo(uint16_t locate) const noexcept { return bbo_[locate]; }
    [[nodiscard]] uint64_t dropped() const noexcept { return dropped_; }
    [[nodiscard]] uint64_t crossed_updates() const noexcept { return crossed_updates_; }
    [[nodiscard]] std::size_t peak_live() const noexcept { return peak_live_; }

private:
    itch::BookEngine&                  engine_;
    EventRing&                         ring_;
    itch::SymbolDirectory&             dir_;
    std::unique_ptr<itch::BboCache[]>  bbo_;
    uint64_t    recv_tsc_ = 0;
    uint64_t    dropped_ = 0;
    uint64_t    crossed_updates_ = 0;
    std::size_t peak_live_ = 0;
    bool        market_open_ = false;
};

using Builder = itch::BookBuilder<Publisher>;
using FeedParser = itch::Parser<Builder>;

// ---------------------------------------------------------------------------
// --depth-profile (itch/depth_profiler.hpp)
// ---------------------------------------------------------------------------

int run_depth_profile(const Config& cfg) {
    net::ItchFile file;
    if (const auto err = file.open(cfg.replay_path.c_str()); !err.empty()) {
        std::fprintf(stderr, "cannot open %s: %s\n", cfg.replay_path.c_str(), err.c_str());
        return 1;
    }
    auto engine = std::make_unique<itch::BookEngine>(cfg.expected_orders);
    auto profiler = std::make_unique<itch::DepthProfiler>(*engine);
    itch::Parser<itch::DepthProfiler> parser(*profiler);
    parser.parse_stream(file.data(), file.size());
    profiler->print();
    return 0;
}

// ---------------------------------------------------------------------------
// Consumer thread: the strategy hook
// ---------------------------------------------------------------------------

struct ConsumerStats {
    std::atomic<uint64_t> events{0};
};

void consumer_loop(EventRing& ring, ConsumerStats& stats, std::atomic<bool>& feed_done, int cpu) {
    pin_current_thread(cpu, "consumer");
    uint64_t n = 0;
    uint64_t checksum = 0;
    for (;;) {
        const std::size_t got = ring.pop_batch([&](const itch::BookEvent& e) {
            // Strategy hook: e.bid_price / e.ask_price / e.bid_qty / e.ask_qty ...
            checksum += e.bid_price ^ e.ask_price;
        }, 256);
        n += got;
        if (got == 0) {
            if (feed_done.load(std::memory_order_acquire) && ring.size_approx() == 0) break;
            itch::cpu_relax();
        }
    }
    stats.events.store(n, std::memory_order_relaxed);
    if (checksum == 0x5EED) std::fputc('\0', stderr);  // keep the hook from being optimised away
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void print_report(const FeedParser& p, const itch::BookEngine& e, const Publisher& pub,
                  uint64_t consumer_events, double seconds) {
    const auto& s = p.stats();
    std::printf("\n=== Session report (%s kernels) ===\n", itch::simd_level_name());
    std::printf("  messages          %" PRIu64 "  (%.2f M msg/s over %.3f s)\n", s.messages,
                static_cast<double>(s.messages) / seconds / 1e6, seconds);
    std::printf("  by type          ");
    for (char t : std::string("SRHYLVWKJhAFECXDUPQBINO"))
        if (const uint64_t n = s.by_type[static_cast<uint8_t>(t)]) std::printf(" %c=%" PRIu64, t, n);
    std::printf("\n  malformed         unknown_type=%" PRIu64 " bad_length=%" PRIu64 " empty=%" PRIu64 "\n",
                s.unknown_type, s.bad_length, s.empty);
    std::printf("  book integrity    unknown_ref=%" PRIu64 " missing_level=%" PRIu64 " overfill=%" PRIu64 "\n",
                e.stats().unknown_ref, e.stats().missing_level, e.stats().overfill);
    std::printf("  crossed updates   %" PRIu64 " (during market hours)\n", pub.crossed_updates());
    std::printf("  live orders       end=%zu peak=%zu (map capacity %zu, rehashes %" PRIu64 ")\n",
                e.orders().size(), pub.peak_live(), e.orders().capacity(), e.orders().stats().rehashes);
    std::printf("  events            published=%" PRIu64 " dropped=%" PRIu64 "\n", consumer_events, pub.dropped());
}

void print_symbol(const itch::SymbolDirectory& dir, const Publisher& pub, const std::string& sym) {
    const auto loc = dir.locate(sym);
    if (!loc) { std::printf("  symbol %s not in directory\n", sym.c_str()); return; }
    const itch::BboSnapshot b = pub.bbo(*loc).load();
    std::printf("  %s (locate %u): bid %u @ %.4f | ask %u @ %.4f\n", sym.c_str(), *loc,
                static_cast<unsigned>(b.bid_qty), b.bid_price / 1e4,
                static_cast<unsigned>(b.ask_qty), b.ask_price / 1e4);
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

int run_replay(const Config& cfg, FeedParser& parser, Publisher& pub) {
    net::ItchFile file;
    if (const auto err = file.open(cfg.replay_path.c_str()); !err.empty()) {
        std::fprintf(stderr, "cannot open %s: %s\n", cfg.replay_path.c_str(), err.c_str());
        return 1;
    }
    std::printf("replaying %s (%.2f GB)\n", cfg.replay_path.c_str(), static_cast<double>(file.size()) / 1e9);

    const uint8_t* buf = file.data();
    const std::size_t len = file.size();
    if (!cfg.latency) {
        // 64 MiB slices so Ctrl-C is honoured mid-file; a record cut by a
        // slice boundary is re-parsed from the start of the next slice.
        constexpr std::size_t kSlice = 64u << 20;
        std::size_t off = 0;

        const auto parse_slice = [&](const uint8_t* p, std::size_t n) -> std::size_t {
            switch (cfg.prefetch) {
                case 8:  return parser.parse_stream_prefetch<8>(p, n);
                case 16: return parser.parse_stream_prefetch<16>(p, n);
                case 32: return parser.parse_stream_prefetch<32>(p, n);
                default: return parser.parse_stream(p, n);
            }
        };
        itch::PerfCounters pmu;
        const itch::PerfCounters::Sample before = pmu.read();
        while (off < len && g_running.load(std::memory_order_relaxed)) {
            const std::size_t n = std::min(kSlice, len - off);
            const std::size_t used = parse_slice(buf + off, n);
            if (used == 0) break;  // trailing partial record
            off += used;
        }
        if (off != len && g_running.load(std::memory_order_relaxed))
            std::fprintf(stderr, "warning: %zu trailing bytes not consumed\n", len - off);
        if (cfg.perf) {
            if (pmu.available()) {
                const auto d = pmu.read() - before;
                if (d.ref_cycles != 0)
                    std::printf("pmu: %.2f instructions/cycle, frequency %.3fx nominal (%" PRIu64 " cycles)\n",
                                d.ipc(), d.frequency_ratio(), d.cycles);
                else
                    std::printf("pmu: %.2f instructions/cycle (%" PRIu64 " cycles); %s\n",
                                d.ipc(), d.cycles, pmu.error().c_str());
            } else {
                std::printf("pmu: unavailable (%s)\n", pmu.error().c_str());
            }
        }
        return 0;
    }

    const double ticks_per_ns = itch::TscClock::calibrate_ticks_per_ns();
    itch::LatencyHistogram hist;
    std::size_t off = 0;
    while (off + 2 <= len && g_running.load(std::memory_order_relaxed)) {
        const std::size_t n = itch::load_be16(buf + off);
        if (off + 2 + n > len) break;
        const uint64_t t0 = itch::TscClock::start();
        parser.parse(buf + off + 2, n);
        const uint64_t t1 = itch::TscClock::stop();
        hist.record(t1 - t0);
        off += 2 + n;
    }
    const auto ns = [&](uint64_t t) { return static_cast<double>(t) / ticks_per_ns; };
    std::printf("per-message latency (fenced RDTSC, includes ~timer overhead): "
                "p50=%.1f ns  p99=%.1f ns  p99.9=%.1f ns  p99.99=%.1f ns  max=%.1f ns\n",
                ns(hist.percentile(50)), ns(hist.percentile(99)), ns(hist.percentile(99.9)),
                ns(hist.percentile(99.99)), ns(hist.max()));
    (void)pub;
    return 0;
}

/// Shared by the UDP-socket and AF_XDP receivers: both expose poll() and
/// packet(i) with a MoldUDP64 payload in .data/.len.
template <class Rx>
int live_loop(Rx& rx, const Config& cfg, FeedParser& parser, Publisher& pub) {
    itch::MoldSequencer seq;
    uint64_t gaps = 0, lost = 0;
    auto last = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_relaxed)) {
        const std::size_t n = rx.poll();
        for (std::size_t i = 0; i < n; ++i) {
            const auto& pkt = rx.packet(i);
            // AF_XDP packets carry the TSC of the poll that received them.
            if constexpr (requires { pkt.rx_tsc; }) pub.set_recv_tsc(pkt.rx_tsc);
            else                                    pub.set_recv_tsc(itch::TscClock::now());
            const auto r = seq.on_packet(pkt.data, pkt.len, [&](const uint8_t* m, std::size_t l, uint64_t) {
                parser.parse(m, l);
            });
            if (r.gap_count) { ++gaps; lost += r.gap_count; }
            if (r.end_of_session) g_running.store(false);
        }
        if (n == 0) itch::cpu_relax();
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(cfg.stats_secs)) {
            std::printf("messages=%" PRIu64 " next_seq=%" PRIu64 " gaps=%" PRIu64 " lost=%" PRIu64 "\n",
                        parser.stats().messages, seq.next_expected(), gaps, lost);
            last = now;
        }
    }
    std::printf("live: next_seq=%" PRIu64 " gaps=%" PRIu64 " lost=%" PRIu64 "\n", seq.next_expected(), gaps, lost);
    return 0;
}

int run_live(const Config& cfg, FeedParser& parser, Publisher& pub) {
    const std::string where = (cfg.mcast_group.empty() ? std::string("*") : cfg.mcast_group) + ":" +
                              std::to_string(cfg.mcast_port);
    if (!cfg.xdp_iface.empty()) {
#if defined(ITCH_HAVE_XDP)
        net::XdpReceiver rx;
        net::XdpReceiver::Options opt;
        opt.ifname      = cfg.xdp_iface;
        opt.queue       = cfg.xdp_queue;
        opt.dst_port    = cfg.mcast_port;
        opt.group       = cfg.mcast_group;
        opt.bpf_object  = cfg.xdp_prog.empty() ? ITCH_XDP_BPF_OBJECT : cfg.xdp_prog;
        opt.native_mode = cfg.xdp_native;
        opt.zero_copy   = cfg.xdp_zero_copy;
        if (const auto err = rx.open(opt); !err.empty()) {
            std::fprintf(stderr, "AF_XDP on %s queue %u: %s\n", cfg.xdp_iface.c_str(), cfg.xdp_queue, err.c_str());
            return 1;
        }
        std::printf("AF_XDP on %s queue %u (%s mode%s), filtering %s\n", cfg.xdp_iface.c_str(), cfg.xdp_queue,
                    cfg.xdp_native ? "driver" : "generic", cfg.xdp_zero_copy ? ", zero-copy" : "", where.c_str());
        const int rc = live_loop(rx, cfg, parser, pub);
        std::printf("xdp: frames=%" PRIu64 " not_udp=%" PRIu64 " fill_starve=%" PRIu64 "\n",
                    rx.stats().frames, rx.stats().not_udp, rx.stats().fill_starve);
        return rc;
#else
        std::fprintf(stderr, "--xdp: this binary was built without AF_XDP support (configure with -DITCH_ENABLE_XDP=ON)\n");
        return 1;
#endif
    }
    net::UdpReceiver rx;
    net::UdpReceiver::Options opt;
    opt.group = cfg.mcast_group;
    opt.port = cfg.mcast_port;
    opt.interface_ip = cfg.iface_ip;
    if (const auto err = rx.open(opt); !err.empty()) {
        std::fprintf(stderr, "cannot open %s: %s\n", where.c_str(), err.c_str());
        return 1;
    }
    std::printf("listening on %s\n", where.c_str());
    return live_loop(rx, cfg, parser, pub);
}

} // namespace

int main(int argc, char** argv) {
    itch::check_cpu_or_exit();
    const Config cfg = parse_args(argc, argv);
    if (cfg.depth_profile) {
        if (cfg.replay_path.empty()) usage(argv[0], 2);
        return run_depth_profile(cfg);
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    auto engine = std::make_unique<itch::BookEngine>(cfg.expected_orders);
    auto ring   = std::make_unique<EventRing>();  // 4 MiB: never on the stack
    itch::SymbolDirectory dir;
    Publisher pub(*engine, *ring, dir);
    Builder builder(*engine, pub);
    FeedParser parser(builder);

    ConsumerStats cstats;
    std::atomic<bool> feed_done{false};
    std::thread consumer(consumer_loop, std::ref(*ring), std::ref(cstats), std::ref(feed_done), cfg.cpu_consumer);
    pin_current_thread(cfg.cpu_feed, "feed");

    const auto t0 = std::chrono::steady_clock::now();
    const int rc = cfg.replay_path.empty() ? run_live(cfg, parser, pub) : run_replay(cfg, parser, pub);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    feed_done.store(true, std::memory_order_release);
    consumer.join();
    if (rc == 0) {
        print_report(parser, *engine, pub, cstats.events.load(), secs);
        if (!cfg.symbol.empty()) print_symbol(dir, pub, cfg.symbol);
    }
    return rc;
}
