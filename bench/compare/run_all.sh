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

for m in parse book book_pf16; do
  run "$bin/parseritch.x86" "$F" $m
  run "$bin/parseritch_avx2.x86" "$F" $m
done
for a in x86 arm; do
  for m in parse overlay book; do run "$bin/itchcpp.$a" "$F" $m; done
  for m in parse book; do run "$bin/cpptrader.$a" "$F" $m; done
  run "$bin/ccooper.$a" "$F" book
done

# Upstream tools, as their authors ship them (own I/O loop and own timer).
if [ -n "${PEERS:-}" ]; then
  for i in $(seq "$REPS"); do
    "$root/build/feed_handler" --replay "$F"               >> "$OUT/native_feed_handler.txt" 2>&1
    "$root/build/feed_handler" --replay "$F" --prefetch 16 >> "$OUT/native_feed_handler_pf16.txt" 2>&1
    for a in x86 arm; do
      for t in itch_handler market_manager; do
        "$PEERS/chronoxor_CppTrader/build-$a/cpptrader-performance-$t" -i "$F" >> "$OUT/native_cpptrader_${t}_$a.txt" 2>&1
      done
    done
  done
fi
echo done >&2
