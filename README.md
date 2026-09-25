# ITCH 5.0 Feed Handler: SIMD Order Books in C++20

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![x86-64](https://img.shields.io/badge/arch-x86--64%20(SSE4.2%20default%2C%20AVX2%20opt--in)-lightgrey.svg)

A Nasdaq TotalView-ITCH 5.0 decoder and full-depth order-book builder for
x86-64. The hot path uses Intel intrinsics where they pay for themselves,
every SIMD kernel ships with a scalar reference that the tests check it
against, and the default build stays at 128-bit vectors on purpose (see
[frequency licenses](#why-the-default-build-is-128-bit)).

It decodes all 23 ITCH 5.0 message types, keeps price-level books for every
symbol from a single global order map, and publishes top-of-book changes
through a seqlock cache and a lock-free SPSC ring. Input is a Nasdaq binary
file, a MoldUDP64 feed on a UDP socket, or a MoldUDP64 feed received through
AF_XDP kernel bypass.

> **Performance numbers are pending.** The previous README quoted figures
> (e.g. "97.6 ns book update, 8x faster than Aquis") that came from a 500K-message
> synthetic corpus, a benchmark that timed a delete + add as one "add", and a
> histogram of per-pass averages. The book those numbers came from also could
> not hold a real trading day (see [what changed](#what-changed-from-v1)). New
> numbers will be published from the free Nasdaq full-day files on native x86
> hardware, using the method in [Benchmarking](#benchmarking).

---

## Where the intrinsics are

| Kernel | File | Variants | Idea |
|---|---|---|---|
| Order lookup (Swiss table) | [`simd/group.hpp`](include/itch/simd/group.hpp), [`order_map.hpp`](include/itch/order_map.hpp) | SWAR (8), **SSE2 (16)**, AVX2 (32) | 1-byte hash tags stored contiguously; one `pcmpeqb` + `pmovmskb` checks a whole group of slots. |
| Price-level search | [`simd/level_search.hpp`](include/itch/simd/level_search.hpp) | scalar, branchless binary, SSE2, **SSE4.1**, AVX2 | Scan 8 sorted keys from the top of book; the insertion point is `end - popcount(ge_mask)`, with no per-lane branches and no scalar tail. |
| Level-array shifts | [`simd/block_move.hpp`](include/itch/simd/block_move.hpp) | **SSE2** | 16-byte overlapping moves instead of libc `memmove`, which picks 256/512-bit copies at run time. |
| Add Order decode | [`simd/add_order_decode.hpp`](include/itch/simd/add_order_decode.hpp) | scalar (load + BSWAP; MOVBE in the AVX2 build), **SSSE3**, AVX2 | The whole 36-byte message is a byte permutation. Works around `pshufb` being unable to cross 16-byte lanes. |
| Big-endian fields | [`bytes.hpp`](include/itch/bytes.hpp) | builtins | `memcpy` + `__builtin_bswap` already compiles to load + `BSWAP` (one `MOVBE` in the AVX2 build): a case where hand-written intrinsics add nothing. |

Bold = what the default 128-bit build runs. [`docs/intrinsics.md`](docs/intrinsics.md)
walks through each kernel: the instruction sequence the compiler emits, the
ISA quirk it works around, and where it should lose to scalar code.

---

## Why the default build is 128-bit

On Intel server parts, wide vector instructions interact with the core's
frequency *license*. Heavy 256-bit and all 512-bit work can lower the
frequency. Even light 256-bit integer work (the only kind here) makes the
core power the upper vector lanes up and down, and the first wide instruction
after an idle gap pays a warm-up window. Market data is quiet, then bursty, so
that cost lands on the first messages of a burst. Frequency transitions are
jitter, and jitter is what a feed handler must not have.

So:

- The default targets are built with `-march=x86-64-v2` (SSE up to 4.2), and
  compiler auto-vectorization is capped at 128 bits
  (`-mprefer-vector-width=128`).
- The AVX2 variant (`-march=x86-64-v3`) is built next to it as separate
  `*_avx2` binaries, to be **measured** against the default, not assumed
  faster:
  ```bash
  ./build/bm_itch      --benchmark_filter=BM_Burst   # idle gaps, then bursts
  ./build/bm_itch_avx2 --benchmark_filter=BM_Burst
  tools/license_check.sh ./build/bm_itch_avx2 --benchmark_filter=BM_Burst
  ```
  `BM_Burst` reports the first messages of each burst separately from the
  rest, plus effective/nominal frequency when a PMU is available.
  `license_check.sh` adds Intel's `core_power.*` license counters.
- The level books don't call libc `memmove` (glibc picks AVX2/AVX-512 copies
  at run time, whatever `-march` says); they use an SSE2 block move.
- AVX-512 is not used anywhere.

---

## Architecture

```
 Nasdaq file (mmap)     MoldUDP64 over UDP (recvmmsg,     MoldUDP64 over AF_XDP
        |               SO_TIMESTAMPING, busy-poll)       (own BPF filter, UMEM)
        |                        |                                 |
        |                        +------------+--------------------+
        |                                     |
        |                  MoldSequencer: gaps, A/B dedup, session changes
        +----------------------+--------------+
                               v
          Parser<Handler>  (header-only template)
            * exact length check per type (kMessageLength)
            * constexpr 256-entry dispatch table on the type byte
            * decodes only the types the handler has an on() overload for
            * optional prefetch lookahead: hint record i+D while parsing i
                               |
                               v
          BookBuilder<Sink>
            |                                   |
            v                                   v
     BookEngine                           Sink (feed_handler: Publisher)
       OrderMap: one global Swiss table     * BboCache[locate]: seqlock
       (ref -> Order, SIMD group probe)     * SpscRing<BookEvent, 65536>
       OrderBook[locate]: two LevelSides           |
       (SoA keys/qty/count, best at back,          v
        SIMD level search, SSE2 shifts)     consumer / strategy thread
```

### Design decisions

- **One global order map.** ITCH order references are unique per day across all
  symbols, so one hash map covers the whole feed. The spec does *not* promise
  they increase (that wording was removed in 2009), so a flat array indexed by
  ref is not safe. The table stays up to 7/8 full, and erase avoids tombstones
  when a group still has an empty slot (proof in `order_map.hpp`).
- **Level books: arrays with the best price at the back.** Keys, quantities and
  order counts sit in separate arrays, so the search reads only 4-byte keys.
  Asks are stored as `~price`, so both sides sort the same way and share one
  search routine. Most activity is at the top of the book, where inserts and
  erases shift only a few elements.
- **Compile-time handlers.** `Parser<Handler>` checks at compile time which
  `on()` overloads the handler has (a `requires` expression). Unhandled
  message types are length-checked and counted, never decoded. No
  `std::function`, no virtual calls.
- **One source of truth for the wire format.** Each message declares its
  `(member, offset)` pairs once. Decode, encode and a `static_assert` that the
  fields tile the message exactly are all generated from that list.
- **Prefetch lookahead.** `parse_stream_prefetch<D>` hints the order-map group
  and book of record *i + D* while parsing record *i*, so the cache misses of
  a full-day session (hundreds of MB of order map) overlap instead of arriving
  one by one. Results are identical to `parse_stream`; the right *D* is
  measured, not guessed (`BM_Parse_Prefetch`).
- **Seqlock top-of-book.** Readers get a consistent snapshot or retry. All
  fields share one cache line. Everything is `std::atomic` with relaxed
  loads and stores, so there is no data race; on x86 these are plain `MOV`s.
- **SPSC ring** with free-running indices, a cached copy of the other side's
  index, 128-byte separation (Intel's adjacent-line prefetcher works on
  128-byte pairs), and batch pop.
- **Memory.** Large tables come from `mmap`, get `MADV_HUGEPAGE`, and are only
  then pre-faulted (`MADV_POPULATE_WRITE`). `MAP_POPULATE` would fault them in
  as 4 KiB pages before the huge-page advice took effect. A book's level
  storage is allocated when its Stock Directory message arrives, before
  trading starts. The hot path takes no first-touch faults and makes no
  allocations in steady state.
- **One ISA per binary.** The default and AVX2 builds are separate binaries,
  not one binary with runtime dispatch. Compiling the same inline functions
  for two ISAs into one binary lets the linker keep either copy for both
  (COMDAT folding), so the wide copy could run on the narrow path. Separate
  binaries rule that out, and match how a feed handler is deployed: onto known
  hardware, measured first. `feed_handler` and `bm_itch` check at startup that
  the CPU has what they were built for.
- **AF_XDP without collateral damage.** The receiver loads its own BPF program,
  which redirects only the configured UDP flow (port, optionally multicast
  group) and passes everything else (ARP, SSH, ICMP, other UDP) to the kernel.
  It strips Ethernet/IPv4/UDP headers with a validating parser, returns frames
  to the kernel by their own addresses, and joins the multicast group on an
  ordinary socket so the NIC and switch deliver it.

---

## Build

Requirements: x86-64, CMake 3.20+, GCC 12+ or Clang 16+. GoogleTest and
Google Benchmark are used from the system if installed, otherwise downloaded
at pinned, hash-checked versions.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # both variants
```

Targets: `feed_handler`, `bm_itch`, `itch_tests` (default, x86-64-v2) and
`feed_handler_avx2`, `bm_itch_avx2`, `itch_tests_avx2` (x86-64-v3), plus
`itch_synth`.

| Option | Default | Meaning |
|---|---|---|
| `ITCH_MARCH` | `x86-64-v2` | ISA of the default targets |
| `ITCH_BUILD_AVX2_VARIANT` | `ON` | also build the `*_avx2` targets |
| `ITCH_PREFER_VECTOR_WIDTH` | `128` | widest vector the compiler may auto-vectorize with |
| `ITCH_LEVEL_LINEAR_CHUNKS` | `4` | 8-key chunks scanned from the top of book before binary search |
| `ITCH_FORCE_SCALAR` | `OFF` | scalar/SWAR kernels everywhere (A/B comparison) |
| `ITCH_SANITIZE` | empty | `address,undefined` or `thread` |
| `ITCH_LTO` | `OFF` | link-time optimisation |
| `ITCH_PGO` / `ITCH_PGO_DATA` | empty | `generate` / `use` + profile location (see `tools/pgo.sh`) |
| `ITCH_BOLT_READY` | `OFF` | link with `--emit-relocs` for `llvm-bolt` (see `tools/bolt.sh`) |
| `ITCH_ENABLE_XDP` | `OFF` | AF_XDP receiver + BPF program (Linux; libbpf, libxdp, clang, pkg-config) |
| `ITCH_WERROR` | `ON` | warnings are errors (`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow ...`) |

**Linux on a Mac.** [`tools/docker/Dockerfile`](tools/docker/Dockerfile) is
the CI toolchain (GCC 13, Clang 18, sanitizer runtimes, libxdp) as an x86-64
container; its header shows how to build and test in it. On Apple Silicon it
runs under Rosetta for Linux: builds, tests, ASan+UBSan and the AF_XDP
self-test work there, but TSan doesn't and timings mean nothing.

**Apple Silicon, natively.** The project is x86-only. On an arm64 Mac, CMake
builds x86_64 and the binaries run under Rosetta 2, which executes SSE and
AVX2 but hides AVX2 from CPUID (the startup check knows and allows it). Use
this for development and correctness testing only: `bm_itch` prints a
warning, and `plot_benchmarks.py` refuses to plot Rosetta results.

---

## Run

```bash
# Synthetic session (no download needed)
./build/itch_synth session.itch --events 5000000
./build/feed_handler --replay session.itch

# Real data: free full-day files from Nasdaq (about 4-5 GB compressed each)
curl -O "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
gunzip -k 01302019.NASDAQ_ITCH50.gz
./build/feed_handler --replay 01302019.NASDAQ_ITCH50 --symbol AAPL
./build/feed_handler --replay 01302019.NASDAQ_ITCH50 --latency         # per-message TSC histogram
./build/feed_handler --replay 01302019.NASDAQ_ITCH50 --prefetch 16 --perf
./build/feed_handler --replay 01302019.NASDAQ_ITCH50 --depth-profile   # choose ITCH_LEVEL_LINEAR_CHUNKS

# Live MoldUDP64: UDP socket (multicast or unicast)...
./build/feed_handler --mcast 233.54.12.111:26477 --iface 10.0.0.5 --cpu-feed 2 --cpu-consumer 4
# ...or AF_XDP on rx queue 3 (build with -DITCH_ENABLE_XDP=ON; needs CAP_NET_ADMIN + CAP_BPF)
sudo ./build/feed_handler --mcast 233.54.12.111:26477 --xdp eth0:3 [--xdp-native] [--xdp-zerocopy]

# Feed a live mode from a file (standard-library Python)
tools/mold_send.py session.itch 127.0.0.1 26477 --pps 20000   # with: feed_handler --port 26477
```

Replay prints a session report that doubles as an end-to-end correctness check:

```
  messages          <n>  (<rate> M msg/s over <t> s)
  by type           S=.. R=.. A=.. F=.. E=.. C=.. X=.. D=.. U=.. P=..
  malformed         unknown_type=0 bad_length=0 empty=0
  book integrity    unknown_ref=0 missing_level=0 overfill=0
  crossed updates   <n> (during market hours)
  live orders       end=<n> peak=<n> (map capacity <n>, rehashes 0)
```

On a complete real session, `unknown_ref`, `missing_level` and `overfill`
must all be 0. Synthetic sessions show crossed books because the generator
lets the mid price drift without executing resting orders.

---

## Testing

`ctest` runs the suite for both variants (88 tests in the default build, 93
in the AVX2 build, which adds the AVX2 kernel cases):

- **Every SIMD kernel vs its scalar reference**: level search over all sizes
  0-130 with duplicates and keys on both sides of the sign bit; control-byte
  groups over 20k random groups; three Add Order decoders over 100k random
  messages; block moves against `std::memmove` at every size and position.
- **Order map vs `std::unordered_map`** for every group width, including growth
  from tiny tables and a million-operation churn at constant size.
- **Full pipeline vs a naive reference engine** (`std::map` books) on a
  300k-event synthetic session, comparing every level of every book at
  checkpoints mid-session; the prefetch pipeline must match plain parsing at
  every lookahead distance.
- **Wire format**: a length table written independently from the spec,
  hand-assembled byte vectors, and a byte-exact round trip for all 23 types.
- **Parser robustness**: wrong lengths rejected before decoding, zero-length
  frames, partial records left unconsumed.
- **Frames**: Ethernet/VLAN/IPv4/UDP parsing, rejection of every truncation,
  fragments, non-IPv4 and non-UDP.
- **Concurrency**: SPSC order and integrity across threads; seqlock readers
  never see a torn snapshot (the writer waits for the readers, so the test
  can't pass vacuously).
- **MoldUDP64**: gaps, A/B duplicates, partial overlap, heartbeats, session
  changes, end of session, malformed packets; plus a UDP loopback test.

**AF_XDP end to end:** `sudo tools/xdp_selftest.sh build` creates a veth
pair and a network namespace, then replays a session into
`feed_handler --xdp`:

- unicast: checks every message arrives, and that ping keeps working while
  the program is attached;
- multicast: sends decoy unicast datagrams on the same port and checks none
  reach the socket.

**CI** (`.github/workflows/ci.yml`):

- builds and tests with GCC 13 and Clang 18 at three ISA levels (default +
  AVX2 variant, SSE2-only, forced scalar);
- runs ASan+UBSan and TSan, and clang-tidy;
- builds with AF_XDP and runs the self-test;
- runs the PGO pipeline, and a benchmark smoke run.

---

## Benchmarking

```bash
./build/bm_itch --benchmark_filter='BM_MapFind|BM_LevelSearch' \
    --benchmark_repetitions=10 --benchmark_report_aggregates_only=true \
    --benchmark_out=results.json --benchmark_out_format=json
python3 tools/plot_benchmarks.py results.json docs/img
ITCH_FILE=01302019.NASDAQ_ITCH50 ./build/bm_itch --benchmark_filter=BM_Replay_File
```

| Benchmark | What one item is |
|---|---|
| `BM_LevelSearch/<variant>/depth/uniform` | one level lookup; near-top (geometric) or uniform depth |
| `BM_LevelSearchChunks/chunks:N` | the same, sweeping the back-scan length |
| `BM_MapFind/<variant>/live` | one hit lookup, including the **legacy** v1 `find_order` (`bench/legacy/`) |
| `BM_MapFindMiss`, `BM_MapChurn` | absent-ref lookup; delete-oldest + insert at constant size |
| `BM_AddOrderDecode/<variant>` | one Add Order decode |
| `BM_Engine_*` | stated per benchmark; "AddThenDelete" counts 2 messages per iteration |
| `BM_Parse_*` | one message, end to end; `PerMessage` reports TSC p50/p99/p99.9 and the timer's own cost |
| `BM_Parse_Prefetch/distance:D` | one message, large live set (~1.5M orders), prefetch lookahead D |
| `BM_Burst/gap_us/burst` | one message after an idle gap; first 8 of each burst vs the rest |
| `BM_Replay_File` | a full real session from `ITCH_FILE` |

Build-level optimisations, each measured on its own against a plain build:

```bash
tools/pgo.sh  [TRAIN_FILE [EVAL_FILE]]     # instrument, train, rebuild (GCC or Clang)
tools/bolt.sh [--lbr] [TRAIN [EVAL]]      # post-link layout with llvm-bolt (Linux)
cmake -B build-lto -DITCH_LTO=ON          # link-time optimisation
```

Train and evaluate on different sessions (different days for real data).

For publishable numbers:

- native x86 Linux on bare metal (cloud VMs are noisy and often hide PMU
  counters);
- `performance` governor, turbo off, the benchmark pinned with `taskset`;
- 10+ repetitions;
- `perf stat -e cycles,instructions,cache-misses,branch-misses`, or
  `tools/license_check.sh`, next to each timing;
- report the CPU model and which build (default or `_avx2`) produced each number.

---

## What changed from v1

The original version (v1, which predates this repository) had these
problems, all fixed:

| v1 | Now |
|---|---|
| 4.05 MiB `OrderBook` x 65536 locates = **259 GiB** allocated and zeroed at startup | One global order map + small lazy per-symbol level arrays |
| README claimed "AVX2 SIMD level search", but the code used a scalar binary search | Real SIMD level search (SSE2/SSE4.1/AVX2) with scalar references |
| AVX2 `find_order` loaded 4 scattered slots with scalar loads, then did 1 vector compare | Swiss-table groups: one aligned load checks 16-32 contiguous tags |
| "Producer caches consumer head" claimed, not implemented | Cached indices on both sides, plus batch pop |
| BBO cache could return torn snapshots | Seqlock |
| `std::function` callbacks on the hot path | Compile-time handler binding |
| No length validation: a short frame caused an out-of-bounds read | Exact per-type length check before decode |
| Zero-length record shifted framing by one byte | Fixed and tested |
| 12 of 23 message handlers were empty stubs; `I` (NOII) was decoded as a non-existent "IPO Allocation"; `F` dropped its MPID | All 23 decoded per spec, one field table per message |
| Live UDP path skipped the MoldUDP64 header | MoldUDP64 sequencer with gap detection, A/B dedup and session changes |
| AF_XDP path loaded no BPF program (redirecting *all* traffic on the queue), passed Ethernet frames to the parser, refilled the wrong frames, never joined the group | Own filtering BPF program, header-validating frame parser, correct frame recycling, multicast join; end-to-end self-test |
| `MAP_HUGETLB` on a file mapping (always fails); `MADV_SEQUENTIAL \| MADV_WILLNEED` (enum OR) | Correct `mmap`/`madvise` usage |
| Symbol lookup hashed unpadded input against padded keys (never matched) | Fixed `SymbolDirectory` |
| `-march=native` everywhere, AVX-512 on the roadmap | 128-bit default, AVX2 as a measured opt-in, no AVX-512 |

---

## Status and next steps

The previous roadmap is done: frequency-aware build defaults and
measurement, the prefetch pipeline, depth profiling for the back-scan length,
PGO/LTO/BOLT pipelines, and the AF_XDP receive path. What needs real
hardware next:

- **Numbers.** Replay the Nasdaq full-day files on bare-metal x86; publish
  throughput and per-message latency for the default and AVX2 builds.
- **Decide AVX2 from data.** `BM_Burst` head-vs-rest latency and
  `license_check.sh` counters on the target CPU.
- **Tune from real sessions.** Set `ITCH_LEVEL_LINEAR_CHUNKS` from
  `--depth-profile`; pick the prefetch distance from `BM_Parse_Prefetch` /
  `--prefetch`.
- **AF_XDP on a real NIC.** The self-test covers generic (SKB) mode on veth.
  Driver mode and zero-copy need a supported NIC; NIC receive timestamps
  through XDP RX metadata (Linux 6.3+) are not wired up yet.
