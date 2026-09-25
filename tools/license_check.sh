#!/usr/bin/env bash
# Measure frequency-license behaviour of a command with perf(1).
#
#   tools/license_check.sh <command> [args...]
#
# Example: compare the 128-bit default build with the AVX2 variant on the
# burst benchmark (idle gaps followed by bursts):
#   tools/license_check.sh ./build/bm_itch      --benchmark_filter=BM_Burst
#   tools/license_check.sh ./build/bm_itch_avx2 --benchmark_filter=BM_Burst
#
# Always collected (architectural, any x86 with a PMU):
#   cycles, ref-cycles   -> cycles/ref-cycles = effective frequency / nominal
#   instructions
# Collected when this CPU's perf event list has them (Intel server parts):
#   core_power.lvl0_turbo_license / lvl1 / lvl2   cycles spent in each license
#   core_power.throttle                           cycles stalled by a license change
# The names differ between CPU generations, so they are discovered from
# `perf list` instead of hard-coded. Needs bare metal: VMs rarely expose them.
set -euo pipefail

[[ $# -ge 1 ]] || { sed -n '2,17p' "$0"; exit 2; }
command -v perf >/dev/null 2>&1 || { echo "error: perf not found (linux-tools-\$(uname -r))" >&2; exit 1; }

events="cycles:u,ref-cycles:u,instructions:u"
license=$(perf list --no-desc 2>/dev/null | grep -o -E 'core_power\.[a-z0-9_]+' | sort -u | paste -sd, - || true)
if [[ -n $license ]]; then
    events="$events,$license"
    echo "license events: $license" >&2
else
    echo "note: this CPU/kernel exposes no core_power.* license events; reporting cycles/ref-cycles only" >&2
fi

out=$(mktemp)
trap 'rm -f "$out"' EXIT
rc=0
perf stat -x, -o "$out" -e "$events" -- "$@" || rc=$?

echo
echo "== perf counters"
awk -F, '$1 ~ /^[0-9]+$/ { printf "  %-40s %s\n", $3, $1; v[$3] = $1 }
         END {
           c = v["cycles:u"]; r = v["ref-cycles:u"]; i = v["instructions:u"];
           if (r > 0) printf "  %-40s %.3f\n", "effective / nominal frequency", c / r;
           if (c > 0) printf "  %-40s %.3f\n", "instructions per cycle", i / c;
         }' "$out"
exit $rc
