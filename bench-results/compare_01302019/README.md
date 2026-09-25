# Cross-library comparison: Nasdaq 2019-01-30

**Input:** Nasdaq's free full-day file `01302019.NASDAQ_ITCH50` (11.25 GB,
368,366,634 messages).
**Machine:** Apple M5 Pro, 24 GB, macOS.
**Harness:** [`bench/compare`](../../bench/compare).

parseritch is x86-only, so its rows are x86-64 code translated by **Rosetta 2**.
Every peer was built twice: x86_64 (`-march=x86-64-v2`, the same as
parseritch's default) so it runs under Rosetta too, and native arm64. Compare
x86_64 rows with each other; the arm64 rows show what the peers do without
translation. Treat all of this as relative, not as native-x86 numbers.

## Method

- The file is mmapped and every page touched before the clock starts. One
  pass is timed with `steady_clock`; the table shows the median of 3 runs
  (Python: 1 run).
- **parse** decodes every message and folds `locate + timestamp`. The checksum
  must match parseritch's.
- **book** builds books for every symbol for the whole day.
- **Correctness:** on the file cut at 12:00:00 (151M messages) the four C++
  book builders agree exactly on best bid and ask, price and size, for AAPL,
  MSFT, AMZN, SPY, QQQ, TSLA, NVDA and INTC (`noon_books.txt`).
- **Extra memory** = peak RSS minus the 10,725 MiB of the mapped file.

## Results (`after/`, at the commit that added `bench/compare`)

| Library | Workload | Build | ns/msg | M msg/s | Extra memory (MiB) | Note |
|---|---|---|---:|---:|---:|---|
| **Parse** | | | | | | |
| itchcpp | lazy overlay | arm64 | 1.7 | 588.3 | 2 | reads only 2 header fields |
| itchcpp | lazy overlay | x86_64 | 2.7 | 364.3 | 4 | reads only 2 header fields |
| **parseritch** | **`for_each_frame`** | **x86_64** | **3.2** | **317.0** | 5 | header fields, no dispatch |
| itchcpp | eager parse | arm64 | 5.6 | 178.7 | 2 | |
| CppTrader | ITCHHandler | arm64 | 6.9 | 144.2 | 6 | timestamps wrong¹ |
| **parseritch** | **Parser** | **x86_64** | **7.5** | **132.5** | 5 | AVX2 build: 7.4 |
| itchcpp | eager parse | x86_64 | 10.5 | 95.5 | 4 | |
| CppTrader | ITCHHandler | x86_64 | 11.7 | 85.2 | 5 | timestamps wrong¹ |
| itchfeed | pure Python | arm64 | 762.6 | 1.3 | — | `before/` |
| MeatPy | Python | arm64 | 991.5 | 1.0 | — | `before/` |
| **Book** | | | | | | |
| **parseritch** | **BookBuilder, prefetch 16** | **x86_64** | **37.4** | **26.8** | 429 | full depth |
| charles-cooper | aggregate levels | arm64 | 38.8 | 25.8 | 1,683 | no per-order data |
| charles-cooper | aggregate levels | x86_64 | 47.4 | 21.1 | 1,926 | no per-order data |
| **parseritch** | **BookBuilder** | **x86_64** | **78.5** | **12.7** | 429 | full depth |
| itchcpp | BookManager | arm64 | 122.2 | 8.2 | 298 | L3, per-order queues |
| CppTrader | MarketManager | arm64 | 150.9 | 6.6 | 440 | L3, per-order queues |
| itchcpp | BookManager | x86_64 | 159.8 | 6.3 | 291 | L3, per-order queues |
| CppTrader | MarketManager | x86_64 | 217.2 | 4.6 | 443 | L3, per-order queues |
| MeatPy | one symbol | arm64 | — | — | — | raises at 09:30:00.59² |

`after/table.md` has every row, including the AVX2 builds.

¹ CppTrader's `ReadTimestamp` keeps 3 of the 6 timestamp bytes on
little-endian hosts. Its books are unaffected.
² MeatPy raises `ExecutionPriorityException`: it expects executions to hit
the front of the queue.

## Before and after the prefetch change (parseritch, x86_64)

| Workload | `before/` | `after/` |
|---|---:|---:|
| Parser, parse only | 7.3 ns/msg | 7.5 ns/msg (noise) |
| BookBuilder, no prefetch | 77.7 | 78.5 |
| BookBuilder, prefetch 16 | 43.8 | **37.4** |
| BookBuilder AVX2, prefetch 16 | 57.8 | 50.0 |
| `feed_handler`, prefetch 16 (whole pipeline, median wall time) | 28.3 s | 25.6 s |

- The `feed_handler` runs are noisier than the single-threaded harness: its
  feed and consumer threads were 23–30 s from run to run.
- `after/native_feed_handler_old_pf16.txt` is the binary from before the
  change, run in the same session for an A/B comparison.
- AVX2 is slower only because Rosetta 2 translates it. That says nothing about
  native x86.

## Reproduce

```bash
# peers cloned into $PEERS and built into build-x86/ and build-arm/ (see build.sh)
cmake -B build-dev -DCMAKE_BUILD_TYPE=Release && cmake --build build-dev -j
PEERS=$PEERS bench/compare/build.sh
PEERS=$PEERS bench/compare/run_all.sh 01302019.NASDAQ_ITCH50 out 3
python3 bench/compare/aggregate.py out $(stat -f %z 01302019.NASDAQ_ITCH50)
```

`after/` also holds what the top-level README's charts are drawn from:

- `session_{1,2,3}.csv`: `bench/compare/bin/session_profile 01302019.NASDAQ_ITCH50`,
  book-building throughput per 2M-message slice (median slice: 26.9M msg/s);
- `engine.json`: `bm_itch --benchmark_filter=BM_Engine_ --benchmark_repetitions=5`.

```bash
python3 bench/compare/plot_readme.py bench-results/compare_01302019/after docs/img   # needs matplotlib
```
