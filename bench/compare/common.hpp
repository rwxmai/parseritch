#pragma once
// Shared by every C++ runner in bench/compare: load the whole ITCH file into
// memory before the clock starts (so disk / page-cache effects are excluded
// for every library alike), time one pass with steady_clock, print one
// machine-readable RESULT line.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cmp {

struct Buffer {
    uint8_t*    data = nullptr;
    std::size_t size = 0;
};

inline Buffer load(const char* path) {
    // mmap + touch every page: the file's page-cache pages *are* the buffer,
    // so a 9 GB day isn't held twice (page cache + private copy).
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) { std::perror(path); std::exit(1); }
    struct stat st {};
    ::fstat(fd, &st);
    Buffer b;
    b.size = static_cast<std::size_t>(st.st_size);
    void* p = ::mmap(nullptr, b.size, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); std::exit(1); }
    ::close(fd);
    ::madvise(p, b.size, MADV_WILLNEED);
    b.data = static_cast<uint8_t*>(p);
    volatile uint8_t sink = 0;
    for (std::size_t off = 0; off < b.size; off += 4096) sink = sink + b.data[off];
    (void)sink;
    return b;
}

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

using Clock = std::chrono::steady_clock;

inline double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

inline long peak_rss_mib() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return ru.ru_maxrss / (1024 * 1024);  // bytes on macOS
#else
    return ru.ru_maxrss / 1024;           // KiB on Linux
#endif
}

inline const char* arch() {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "arm64";
#else
    return "?";
#endif
}

inline void result(const char* lib, const char* mode, uint64_t msgs, double secs, std::size_t bytes,
                   uint64_t checksum, const std::string& extra = {}) {
    std::printf("RESULT lib=%s mode=%s arch=%s msgs=%llu secs=%.6f ns_per_msg=%.3f mmsg_s=%.3f gib_s=%.3f "
                "checksum=%llu peak_rss_mib=%ld%s%s\n",
                lib, mode, arch(), static_cast<unsigned long long>(msgs), secs, secs * 1e9 / double(msgs),
                double(msgs) / secs / 1e6, double(bytes) / secs / double(1ull << 30),
                static_cast<unsigned long long>(checksum), peak_rss_mib(), extra.empty() ? "" : " ",
                extra.c_str());
    std::fflush(stdout);
}

/// Symbols whose top of book every book runner prints at the end, so the
/// books can be cross-checked (run them on the noon-truncated file).
inline const std::vector<std::string>& digest_symbols() {
    static const std::vector<std::string> s{"AAPL", "MSFT", "AMZN", "SPY", "QQQ", "TSLA", "NVDA", "INTC"};
    return s;
}

inline void print_bbo(const char* lib, const std::string& sym, uint64_t bid_px, uint64_t bid_qty,
                      uint64_t ask_px, uint64_t ask_qty) {
    std::printf("BBO lib=%s sym=%s bid=%llu@%.4f ask=%llu@%.4f\n", lib, sym.c_str(),
                static_cast<unsigned long long>(bid_qty), double(bid_px) / 1e4,
                static_cast<unsigned long long>(ask_qty), double(ask_px) / 1e4);
}

/// Symbol -> locate from the Stock Directory ('R') messages, for runners
/// whose library has no symbol directory. Runs after the timed region.
inline int locate_of(const Buffer& b, const std::string& sym) {
    char padded[8];
    std::memset(padded, ' ', 8);
    std::memcpy(padded, sym.data(), std::min<std::size_t>(sym.size(), 8));
    std::size_t off = 0;
    while (off + 2 <= b.size) {
        const std::size_t n = be16(b.data + off);
        const uint8_t* m = b.data + off + 2;
        if (n == 39 && m[0] == 'R' && std::memcmp(m + 11, padded, 8) == 0) return be16(m + 1);
        off += 2 + n;
    }
    return -1;
}

} // namespace cmp
