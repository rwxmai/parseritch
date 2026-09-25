#!/usr/bin/env bash
# run_all.sh FILE OUT_DIR [REPS]   (PEERS=<peer repos> for the native-tool rows)
# Runs every runner REPS times, sequentially, and appends RESULT/BBO lines to
# OUT_DIR/results.txt. Upstream tools' own output goes to OUT_DIR/native_*.txt.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
F="$1"; OUT="$2"; REPS="${3:-3}"
mkdir -p "$OUT"
R="$OUT/results.txt"
bin="$here/bin"

run() {  # run LABEL CMD...
  for i in $(seq "$REPS"); do
    echo ">> $* (rep $i)" >&2
    "$@" 2>>"$OUT/stderr.txt" | grep -E '^(RESULT|BBO)' | sed "s/^/rep=$i /" >> "$R"
  done
}

cat "$F" > /dev/null   # warm the page cache once

for m in parse frames book book_pf16; do
  run "$bin/parseritch.x86" "$F" $m
  run "$bin/parseritch_avx2.x86" "$F" $m
done
for a in x86 arm; do
  for m in parse overlay book; do run "$bin/itchcpp.$a" "$F" $m; done
  for m in parse book; do run "$bin/cpptrader.$a" "$F" $m; done
  run "$bin/ccooper.$a" "$F" book
done

# Upstream tools, as their authors ship them (own I/O loop and own timer).
# OLD_FEED_HANDLER: a pre-change feed_handler to A/B against (optional).
if [ -n "${PEERS:-}" ]; then
  fh="$root/${ITCH_BUILD:-build-dev}/feed_handler"
  for i in $(seq "$REPS"); do
    echo ">> native rep $i" >&2
    if [ -n "${OLD_FEED_HANDLER:-}" ]; then
      "$OLD_FEED_HANDLER" --replay "$F" --prefetch 16 >> "$OUT/native_feed_handler_old_pf16.txt" 2>&1
    fi
    "$fh" --replay "$F"              >> "$OUT/native_feed_handler_default.txt" 2>&1
    "$fh" --replay "$F" --prefetch 0 >> "$OUT/native_feed_handler_pf0.txt" 2>&1
    for a in x86 arm; do
      for t in itch_handler market_manager; do
        "$PEERS/chronoxor_CppTrader/build-$a/cpptrader-performance-$t" -i "$F" >> "$OUT/native_cpptrader_${t}_$a.txt" 2>&1
      done
    done
  done
fi
echo done >&2
