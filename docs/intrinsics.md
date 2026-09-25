# Intrinsics in this code base: a kernel-by-kernel tour

Each section covers the problem, the instruction sequence the compiler actually
emits (Clang 17, `-O3`, AT&T syntax, trimmed), the ISA quirk the kernel
works around, and where it should lose to scalar code. Every kernel has a
scalar reference, a randomized equivalence test, and a benchmark, listed at
the end of its section.

Numbers are deliberately left out of this document. They belong to a
specific CPU and must come from native x86 hardware (see the README's
Benchmarking section), never from Rosetta.

---

## 0. Why the default build is 128-bit

The default targets are built with `-march=x86-64-v2`: SSE up to 4.2, no AVX
at all, and compiler auto-vectorization capped at 128 bits. AVX2 is an opt-in
variant built next to them (`feed_handler_avx2`, `bm_itch_avx2`, ...).

The reason is frequency licenses. On Intel server parts the core's frequency
depends on the class of vector instructions it runs:

| License | Instructions | Effect |
|---|---|---|
| 0 | scalar, all 128-bit, *light* 256-bit (integer compare, shuffle, logic, load/store) | none |
| 1 | *heavy* 256-bit (FP, FMA, integer multiply), light 512-bit | lower max frequency |
| 2 | heavy 512-bit | lower still |

Every 256-bit instruction in this code base is light integer work, so under
this model the AVX2 build shouldn't change license. Two things still make it
a latency risk rather than a free win:

- **Upper-lane power gating.** After an idle stretch with no 256-bit
  instructions, the core powers down the upper half of its vector units. The
  first 256-bit instruction after that pays a warm-up window of reduced
  wide-op throughput. Market data is quiet, then bursty, so this lands on the
  first messages of a burst: the ones that matter most.
- **Shared power budget.** Licenses are per core, but turbo headroom comes
  from the package budget, so wide work on some cores can lower the maximum
  frequency of the others.

So the AVX2 build exists to be *measured*, not assumed faster:

- `bench/bm_burst.cpp` idles with `PAUSE` only, then times the first
  messages of every burst separately from the rest, in both builds.
- `PerfCounters` (and `feed_handler --perf`) report cycles / ref-cycles:
  effective frequency relative to nominal.
- `tools/license_check.sh` adds Intel's model-specific
  `core_power.lvl*_turbo_license` and `core_power.throttle` counters.

512-bit AVX-512 is not used at all.

---

## 1. The SIMD that wasn't: the v1 order lookup

v1 looked up an order reference with this "AVX2" loop (kept in
[`bench/legacy/legacy_find_order.hpp`](../bench/legacy/legacy_find_order.hpp)):

```cpp
const size_t s0 = (start + (i+0)*(i+1)/2) & MASK;   // quadratic probing:
const size_t s1 = (start + (i+1)*(i+2)/2) & MASK;   // four slots on up to
const size_t s2 = (start + (i+2)*(i+3)/2) & MASK;   // four different
const size_t s3 = (start + (i+3)*(i+4)/2) & MASK;   // cache lines
__m256i slots = _mm256_set_epi64x(refs[s3], refs[s2], refs[s1], refs[s0]);
__m256i hit   = _mm256_cmpeq_epi64(slots, vref);
```

What it compiles to:

```asm
imull ... ; shrl ; addl ; andl      ; x4: four probe-address computations
vmovq       (%rdi,%r10,8), %xmm2    ; scalar load, line A
vmovq       (%rdi,%r9,8),  %xmm3    ; scalar load, line B
vpunpcklqdq %xmm2, %xmm3, %xmm2
vmovq       (%rdi,%rsi,8), %xmm3    ; scalar load, line C
vmovq       (%rdi,%r8,8),  %xmm4    ; scalar load, line D
vpunpcklqdq %xmm3, %xmm4, %xmm3
vinserti128 $1, %xmm2, %ymm3, %ymm2 ; assemble the vector...
vpcmpeqq    %ymm0, %ymm2, %ymm3     ; ...for the one real SIMD instruction
```

The vector does only the compare. At a load factor of at most 0.5, most keys
sit in the first probe slot, yet every lookup computes four addresses and
touches up to four cache lines. A local `slots_arr[4]` also turned on the
stack protector. **SIMD pays off when the data is contiguous; wrapping
intrinsics around scattered loads doesn't make them contiguous.**

## 2. Swiss-table control groups

[`include/itch/simd/group.hpp`](../include/itch/simd/group.hpp),
[`include/itch/order_map.hpp`](../include/itch/order_map.hpp)

Keep a separate array with one byte per slot: 7 bits of hash ("H2") for a
full slot, `0xFF` for EMPTY, `0x80` for DELETED. A probe loads one *aligned
group* of those bytes and compares them all at once. The inner loop of
`OrderMap::find` in the AVX2 variant (32 slots per group). The default build
runs the same loop on 16-slot groups, with `pcmpeqb`/`pmovmskb` on XMM
registers:

```asm
vmovdqa   (%rdx,%r11), %ymm2        ; 32 control bytes, one aligned load
vpcmpeqb  %ymm0, %ymm2, %ymm3       ; == broadcast(H2)
vpmovmskb %ymm3, %r10d              ; -> 32-bit candidate mask
tzcntq    %r10, %rbx                ; first candidate slot
cmpq      %rsi, (%r11,%rbx,8)       ; full 8-byte key compare
blsrq     %r10, %r10                ; clear lowest bit, next candidate
vpcmpeqb  %ymm1, %ymm2, %ymm2       ; any EMPTY in the group?
vpmovmskb %ymm2, %r10d              ;   -> stop probing
```

A false tag match happens with probability about 1/128 per occupied slot, so
the full key compare almost always hits.

Details worth writing up:

- **`match_empty_or_deleted` is one `pmovmskb`** with no compare, because
  both EMPTY (`0xFF`) and DELETED (`0x80`) have the sign bit set.
- **The SWAR fallback** does the same with `uint64_t` arithmetic, 8 tags at a
  time: `(x - 0x01..01) & ~x & 0x80..80` finds zero bytes of
  `ctrl ^ broadcast(h2)`. A borrow can produce a false positive, but only in
  the byte just above a real match, and the key compare filters it out. The
  EMPTY test must be exact (it ends a probe), so it uses a different identity,
  `ctrl & (ctrl << 1) & 0x80..80`, which is true only for `0xFF`.
- **Fewer tombstones.** Groups are aligned, so erase can write EMPTY instead
  of DELETED whenever the group already has an EMPTY slot. A probe only
  continues past a group with no EMPTY, so no key can live beyond such a
  group. ITCH is dominated by deletes, and this keeps probe chains short
  without periodic rebuilds.
- **Hashing.** One Fibonacci multiply. The group index and H2 both come from
  the top bits of the product, because the low bits of a multiplicative hash
  depend only on the low bits of the key.

**Where it should lose (to be confirmed on hardware):** tiny tables that fit
in L1. There the legacy scalar probe touches one line anyway, and the tag load
adds a dependency before the key compare. `BM_MapFind/*/live:4096` is the row
to check.

Tests: `GroupTest.*` (every group type vs a byte-by-byte reference),
`OrderMapTest.*` (vs `std::unordered_map`, growth, churn).
Benchmarks: `BM_MapFind`, `BM_MapFindMiss`, `BM_MapChurn`.

## 3. Level search: popcount instead of branches

[`include/itch/simd/level_search.hpp`](../include/itch/simd/level_search.hpp)

Each side of a book is an ascending `uint32_t` key array with the best price
at the back (asks are stored as `~price`, so "higher key = better" on both
sides). Order flow clusters at the top of the book, so the search scans from
the back, 8 keys per step. The default build's SSE4.1 kernel, `-march=x86-64-v2`:

```asm
movdqu    -32(%rdi,%rcx,4), %xmm1   ; keys[n-8 .. n-4)
movdqu    -16(%rdi,%rcx,4), %xmm2   ; keys[n-4 .. n)
movdqa    %xmm1, %xmm3
pmaxud    %xmm0, %xmm3              ; unsigned max(key, target)
pcmpeqd   %xmm1, %xmm3              ; key >= target  <=>  max(key, target) == key
movmskps  %xmm3, %eax
...                                 ; same for xmm2 -> r8d
shll      $4, %r8d
orl       %eax, %r8d                ; 8-bit ">= target" mask
popcntl   ...                       ; answer = end - popcount(mask)
```

The AVX2 variant does the same with one `vpmaxud`/`vpcmpeqd`/`vmovmskps` on a
256-bit register.

- **Popcount instead of a scan.** In a sorted chunk the lanes at or above the
  target form a suffix, so the insertion point is `end - popcount(ge_mask)`
  (SSE4.1, AVX2). The SSE2 kernel computes the complementary
  `base + popcount(lt_mask)`. There is no per-lane branch and no `tzcnt` walk.
- **No unsigned 32-bit compare in SSE or AVX2.** There is only the signed
  `pcmpgtd`. Asks stored as `~price` have the top bit set, so a signed compare
  orders them wrongly. SSE4.1 and AVX2 use unsigned max + equality
  (`pmaxud` + `pcmpeqd`). SSE2 lacks `pmaxud`, so the SSE2 path flips the sign
  bit of both operands and uses the signed compare.
  `LevelSearch.UnsignedOrderingAcrossSignBit` pins this down.
- **No scalar tail.** Level arrays always allocate at least 8 keys, so the last
  partial chunk is one full-width load from `keys[0]`, with out-of-range lanes
  masked off by an `AND` with `(1 << n) - 1` (compiled to `bzhi` in the AVX2
  build, which has BMI2).
- **Worst case stays logarithmic.** After 4 chunks (32 levels) the search
  switches to a branchless binary search (Khuong & Morin), so deep inserts
  stay O(log n).

**Where it should lose (to be confirmed on hardware):** uniformly random
depths in deep books. The first 32 levels are scanned before the binary
search starts, so plain `std::lower_bound` should win in the `uniform:1`
benchmark rows. That is why `kLinearChunks` needs tuning against the depth
distribution of the real Nasdaq files.

The chunk count is a build option (`ITCH_LEVEL_LINEAR_CHUNKS`). To choose it,
run `feed_handler --depth-profile` on a real session, which reports how far
from the top level operations land, and the chunk count covering 99% and
99.9% of them. `BM_LevelSearchChunks` sweeps the values.

Tests: `LevelSearch.*`. Benchmarks: `BM_LevelSearch/<variant>/depth/uniform`, `BM_LevelSearchChunks`.

## 3b. Moving level arrays without libc

[`include/itch/simd/block_move.hpp`](../include/itch/simd/block_move.hpp)

Inserting or erasing a level shifts the few levels above it. `std::memmove`
would do it, but glibc chooses its `memmove` at run time from the CPU's
features. On AVX2 or AVX-512 hardware that means a 256- or 512-bit copy loop,
whatever `-march` this code was built with, which would put wide
instructions back on the hot path. The replacement is a 16-byte copy loop that
walks in the safe direction for overlapping shifts:

```asm
LBB1_3:                                  ; shift_up<uint64_t>, top-down
    leaq    -16(%rsi), %rcx
    movups  -8(%rdx,%rsi), %xmm0         ; load 16 bytes
    ## InlineAsm Start                   ; empty asm("" : "+x"(v))
    ## InlineAsm End
    movups  %xmm0, (%rdx,%rsi)           ; store 8 bytes higher
    ...
    ja      LBB1_3
```

The empty `asm` statement emits nothing. It exists because compilers
recognise a plain load/store copy loop and rewrite it as a call to `memmove`,
the exact call this replaces. Passing the loaded value through an opaque
register constraint hides the pattern without constraining scheduling the way
a `"memory"` clobber would. `LevelSide::add` now contains no calls except the
cold `grow()`.

Tests: `BlockMoveTest.*` (every size and position up to 80 elements, against
`std::memmove`, with guard elements).

## 4. Decoding a whole message with byte shuffles

[`include/itch/simd/add_order_decode.hpp`](../include/itch/simd/add_order_decode.hpp)

Every field of an Add Order that the book needs is a byte permutation of the
wire message: big-endian fields become little-endian by reversing their
bytes. `pshufb` is exactly a byte permutation, with two catches:

1. **It cannot cross 16-byte lanes.** Even the 256-bit `vpshufb` is two
   independent 128-bit shuffles. The order reference occupies wire bytes
   11..18 and the price bytes 32..35, so fields straddle chunk boundaries.
2. **An index with bit 7 set writes a zero byte.** This is what makes the fix
   work.

The SSSE3 version splits the input into chunks c0, c1, c2 and builds each
16-byte output half as the OR of per-chunk shuffles. Each mask zeroes the
bytes that belong to other chunks (5 `pshufb` in total). The AVX2 version
broadcasts each chunk into both lanes, so lane 0 builds the low output half
and lane 1 the high half at the same time:

```asm
vbroadcasti128 (%rsi), %ymm0              ; c0 in both lanes
vpshufb        LCPI2_0(%rip), %ymm0, %ymm0
vbroadcasti128 16(%rsi), %ymm1            ; c1 in both lanes
vpshufb        LCPI2_1(%rip), %ymm1, %ymm1
vinserti128    $1, 32(%rsi), %ymm0, %ymm2 ; c2: only lane 1 needs it
vpshufb        LCPI2_2(%rip), %ymm2, %ymm2
vpor           %ymm0, %ymm1, %ymm0
vpor           %ymm2, %ymm0, %ymm0
vmovdqa        %ymm0, (%rdi)              ; all 7 fields, little-endian
```

Clang noticed that c2 is only read by lane 1 and replaced its broadcast with a
single `vinserti128`. The shuffle masks are generated at compile time
(`consteval`) from one table of source bytes, and a test checks that every
output byte comes from exactly one chunk.

**The scalar baseline is stronger than it looks.** For the "naive" per-field
version, Clang merges the timestamp and tracking-number reads into one 8-byte
load and byte-swaps on the stores (`movbe` to memory; this is the x86-64-v3
build, since x86-64-v2 has no MOVBE and the default build uses load + `bswap`):

```asm
movq   3(%rsi), %rcx        ; tracking + timestamp in one load
andq   $-65536, %rdx
movbeq %rdx, 8(%rdi)        ; timestamp, swapped on store
movbew %cx, 26(%rdi)        ; tracking, from the same register
```

Whether the shuffle kernel wins depends on what happens next. If the fields
feed straight into scalar code, the round trip through a 32-byte store can
cost what the shuffles saved. The default build uses the SSSE3 version (five
`pshufb`); the AVX2 one is part of the opt-in variant.

Test: `AddOrderDecode.*`. Benchmark: `BM_AddOrderDecode/<variant>`.

## 5. When not to write intrinsics

[`include/itch/bytes.hpp`](../include/itch/bytes.hpp)

`memcpy` + `__builtin_bswap64` compiles to one `movbeq` in the x86-64-v3
build, and to `mov` + `bswap` in the default x86-64-v2 build, which has no
MOVBE. An intrinsic version would be less portable and no faster. The one
hand-tuned case is the 48-bit timestamp: reading the 8 bytes that end at the
field and masking off the top 16 bits turns a 4-byte + 2-byte load-and-merge
into one 8-byte byte-swapped load + `and`. That trick is safe only because the
timestamp sits at offset 5 of every ITCH message, so the over-read covers
bytes 3..10, inside the message. The precondition is written in the function
name: `load_be48_overread`.

## 6. Memory ordering (not intrinsics, but on the same hot path)

- **`BboCache`** is a seqlock written in the Boehm pattern: relaxed atomic
  field stores between a release fence and a release store of the sequence
  number. Readers use an acquire load, relaxed loads, an acquire fence, then a
  relaxed re-read of the sequence. On x86 all of these are plain `MOV`s; the
  point is that the C++ is free of data races (TSan-clean) and does not rely
  on x86's strong memory model. During development the lock-free tests were
  also compiled for a weakly ordered ARM64 machine, with a stand-in platform
  header outside the repo, and run under TSan there. The repository itself
  builds for x86-64 only.
- **`SpscRing`**: each side keeps a private copy of the other side's index and
  reloads it only when the ring looks full or empty, so in steady state no
  cache line moves between cores for every operation.
- **Spin waits** use `_mm_pause()`, which also avoids the memory-order machine
  clear when the spin loop exits.
