#!/usr/bin/env bash
# Post-link layout optimisation of feed_handler with LLVM BOLT (Linux only).
#
#   tools/bolt.sh [--lbr] [TRAIN_FILE [EVAL_FILE]]
#
# Profiles are collected with BOLT's own instrumentation by default, which
# works anywhere (VMs, containers). With --lbr they come from `perf record`
# with last-branch-record sampling instead: cheaper and more accurate, but it
# needs bare-metal Intel (or AMD with BRS) and perf permissions.
#
# Environment: CC/CXX, BUILD_ROOT (default build-bolt), RUNS (default 5),
# BOLT (llvm-bolt binary), PERF2BOLT, CMAKE_ARGS. Ubuntu: apt install bolt-18.
set -euo pipefail

LBR=0
if [[ ${1:-} == "--lbr" ]]; then LBR=1; shift; fi
ROOT=${BUILD_ROOT:-build-bolt}
RUNS=${RUNS:-5}
SRC=$(cd "$(dirname "$0")/.." && pwd)
# Prefer Ninja when installed; an explicit CMAKE_GENERATOR wins.
if [[ -z ${CMAKE_GENERATOR:-} ]] && command -v ninja >/dev/null 2>&1; then export CMAKE_GENERATOR=Ninja; fi
mkdir -p "$ROOT"
ROOT=$(cd "$ROOT" && pwd)

[[ "$(uname -s)" == "Linux" ]] || { echo "error: BOLT needs Linux/ELF" >&2; exit 1; }
pick() { for t in "$@"; do command -v "$t" >/dev/null 2>&1 && { echo "$t"; return; }; done; echo ""; }
BOLT=${BOLT:-$(pick llvm-bolt llvm-bolt-18 llvm-bolt-19)}
PERF2BOLT=${PERF2BOLT:-$(pick perf2bolt perf2bolt-18 perf2bolt-19)}
[[ -n $BOLT ]] || { echo "error: llvm-bolt not found (Ubuntu: apt install bolt-18)" >&2; exit 1; }
# Run BOLT through its real path: it finds its instrumentation runtime
# relative to the path it was invoked by, and distro wrappers such as
# /usr/bin/llvm-bolt-18 are symlinks into /usr/lib/llvm-18/bin.
BOLT=$(readlink -f "$(command -v "$BOLT")")
[[ -z $PERF2BOLT ]] || PERF2BOLT=$(readlink -f "$(command -v "$PERF2BOLT")")

rm -rf "$ROOT/build"  # this script owns BUILD_ROOT; always a clean build

echo "== build with relocations (-Wl,--emit-relocs)"
CC=${CC:-cc} CXX=${CXX:-c++} cmake -S "$SRC" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release \
    -DITCH_BOLT_READY=ON -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_BENCHMARKS=OFF \
    -DITCH_BUILD_AVX2_VARIANT=OFF ${CMAKE_ARGS:-} >/dev/null
cmake --build "$ROOT/build" --target feed_handler itch_synth >/dev/null
BIN=$ROOT/build/feed_handler

TRAIN=${1:-$ROOT/train.itch}
EVAL=${2:-$ROOT/eval.itch}
[[ -f $TRAIN ]] || "$ROOT/build/itch_synth" "$TRAIN" --events 5000000 --seed 1
[[ -f $EVAL ]]  || "$ROOT/build/itch_synth" "$EVAL"  --events 5000000 --seed 2

FDATA=$ROOT/profile.fdata
rm -f "$FDATA"
if [[ $LBR == 1 ]]; then
    [[ -n $PERF2BOLT ]] || { echo "error: perf2bolt not found" >&2; exit 1; }
    echo "== LBR profile (perf record -j any,u)"
    perf record -e cycles:u -j any,u -o "$ROOT/perf.data" -- "$BIN" --replay "$TRAIN" >/dev/null
    "$PERF2BOLT" -p "$ROOT/perf.data" -o "$FDATA" "$BIN"
else
    echo "== instrumentation profile"
    "$BOLT" "$BIN" -instrument -instrumentation-file="$FDATA" -o "$ROOT/feed_handler.instr" >/dev/null
    "$ROOT/feed_handler.instr" --replay "$TRAIN" >/dev/null
fi

echo "== optimise layout"
if ! "$BOLT" "$BIN" -o "$ROOT/feed_handler.bolt" -data="$FDATA" \
        -reorder-blocks=ext-tsp -reorder-functions=hfsort -split-functions -split-all-cold \
        -dyno-stats >"$ROOT/bolt.log" 2>&1; then
    cat "$ROOT/bolt.log" >&2
    echo "error: llvm-bolt failed (log: $ROOT/bolt.log)" >&2
    exit 1
fi
grep -E "taken branches|executed forward" "$ROOT/bolt.log" | head -8 || true

rate() {
    for _ in $(seq "$RUNS"); do
        "$1" --replay "$EVAL" | sed -n 's/.*(\([0-9.]*\) M msg\/s.*/\1/p'
    done | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}
echo "== evaluation on $EVAL ($RUNS runs each, median)"
base=$(rate "$BIN")
bolt=$(rate "$ROOT/feed_handler.bolt")
echo "plain: $base M msg/s"
echo "bolt:  $bolt M msg/s"
awk -v a="$base" -v b="$bolt" 'BEGIN { printf "bolt/plain: %.3fx\n", b / a }'
