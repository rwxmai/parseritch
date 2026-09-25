#!/usr/bin/env bash
# Profile-guided optimisation for feed_handler: instrument, train on a replay,
# rebuild with the profile, and compare against a plain build.
#
#   tools/pgo.sh [TRAIN_FILE [EVAL_FILE]]
#
# Without files, two synthetic sessions with different seeds are generated, so
# the evaluation is not the training input. For real results, train and
# evaluate on Nasdaq files from *different days*.
#
# Environment: CC/CXX (compiler pair; default cc/c++), BUILD_ROOT (default
# build-pgo), RUNS (timed runs per build, default 5), CMAKE_ARGS (extra
# configure flags, e.g. "-DITCH_LTO=ON").
set -euo pipefail

ROOT=${BUILD_ROOT:-build-pgo}
RUNS=${RUNS:-5}
CXX=${CXX:-c++}
CC=${CC:-cc}
SRC=$(cd "$(dirname "$0")/.." && pwd)
# Prefer Ninja when installed; an explicit CMAKE_GENERATOR wins.
if [[ -z ${CMAKE_GENERATOR:-} ]] && command -v ninja >/dev/null 2>&1; then export CMAKE_GENERATOR=Ninja; fi
mkdir -p "$ROOT"
ROOT=$(cd "$ROOT" && pwd)

is_clang() { "$CXX" --version 2>/dev/null | grep -qi clang; }

find_profdata() {
    for tool in llvm-profdata llvm-profdata-18 llvm-profdata-19 llvm-profdata-17; do
        command -v "$tool" >/dev/null 2>&1 && { echo "$tool"; return; }
    done
    if command -v xcrun >/dev/null 2>&1 && xcrun -f llvm-profdata >/dev/null 2>&1; then
        echo "xcrun llvm-profdata"; return
    fi
    echo "error: Clang PGO needs llvm-profdata on PATH" >&2
    exit 1
}

configure() {  # dir, extra args...
    local dir=$1; shift
    CC=$CC CXX=$CXX cmake -S "$SRC" -B "$dir" -DCMAKE_BUILD_TYPE=Release \
        -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_BENCHMARKS=OFF -DITCH_BUILD_AVX2_VARIANT=OFF \
        ${CMAKE_ARGS:-} "$@" >/dev/null
}

# This script owns BUILD_ROOT: start from clean build trees so no stale
# objects, generator choice or .gcda/.profraw files leak into the comparison.
rm -rf "$ROOT/plain" "$ROOT/pgo" "$ROOT/profile"

echo "== plain build"
configure "$ROOT/plain"
cmake --build "$ROOT/plain" --target feed_handler itch_synth >/dev/null

TRAIN=${1:-$ROOT/train.itch}
EVAL=${2:-$ROOT/eval.itch}
[[ -f $TRAIN ]] || "$ROOT/plain/itch_synth" "$TRAIN" --events 5000000 --seed 1
[[ -f $EVAL ]]  || "$ROOT/plain/itch_synth" "$EVAL"  --events 5000000 --seed 2

# Instrument and optimise in the SAME build directory: GCC names .gcda files
# after the object paths, so a separate "use" directory would find nothing.
PGO_DIR=$ROOT/pgo
PROFILE_DIR=$ROOT/profile
mkdir -p "$PROFILE_DIR"

echo "== instrumented build"
configure "$PGO_DIR" -DITCH_PGO=generate -DITCH_PGO_DATA="$PROFILE_DIR"
cmake --build "$PGO_DIR" --target feed_handler >/dev/null

echo "== training run on $TRAIN"
if is_clang; then
    LLVM_PROFILE_FILE="$PROFILE_DIR/fh-%p.profraw" "$PGO_DIR/feed_handler" --replay "$TRAIN" >/dev/null
    PROFDATA=$(find_profdata)
    $PROFDATA merge -o "$PROFILE_DIR/merged.profdata" "$PROFILE_DIR"/*.profraw
    USE_DATA=$PROFILE_DIR/merged.profdata
else
    "$PGO_DIR/feed_handler" --replay "$TRAIN" >/dev/null
    USE_DATA=$PROFILE_DIR
fi

echo "== optimised build"
configure "$PGO_DIR" -DITCH_PGO=use -DITCH_PGO_DATA="$USE_DATA"
cmake --build "$PGO_DIR" --target feed_handler >/dev/null

rate() {  # binary -> median M msg/s over $RUNS runs on $EVAL
    for _ in $(seq "$RUNS"); do
        "$1" --replay "$EVAL" | sed -n 's/.*(\([0-9.]*\) M msg\/s.*/\1/p'
    done | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

echo "== evaluation on $EVAL ($RUNS runs each, median)"
plain=$(rate "$ROOT/plain/feed_handler")
pgo=$(rate "$PGO_DIR/feed_handler")
echo "plain: $plain M msg/s"
echo "pgo:   $pgo M msg/s"
awk -v a="$plain" -v b="$pgo" 'BEGIN { printf "pgo/plain: %.3fx\n", b / a }'

if [[ "$(uname -s)" == "Darwin" && "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" == "1" ]]; then
    echo "note: x86 code ran under Rosetta 2, so the comparison above is not meaningful" >&2
fi
