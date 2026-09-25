#!/usr/bin/env python3
"""aggregate.py OUT_DIR FILE_BYTES -> markdown table (median over reps) on stdout."""
import collections
import re
import statistics
import sys

out, file_bytes = sys.argv[1], int(sys.argv[2])
rows = collections.defaultdict(list)
seen_rep1 = {}
for line in open(f"{out}/results.txt"):
    if "RESULT " not in line:
        continue
    kv = dict(re.findall(r"(\w+)=(\S+)", line))
    # Older parseritch runner builds printed the same lib name for the default
    # and AVX2 binaries; run_all.sh runs default reps first, then AVX2.
    if kv["lib"] == "parseritch":
        key = (kv["mode"], kv["arch"])
        seen = seen_rep1.setdefault(key, 0) + (kv["rep"] == "1")
        seen_rep1[key] = seen
        if seen >= 2:
            kv["lib"] = "parseritch_avx2"
    rows[(kv["lib"], kv["mode"], kv["arch"])].append(kv)

ref_checksum = None
for (lib, mode, arch), rs in rows.items():
    if lib == "parseritch" and mode == "parse":
        ref_checksum = rs[0]["checksum"]

print("| Library | Workload | Arch | Messages | Median time (s) | ns/msg | M msg/s | GiB/s | Peak RSS (MiB) | Working set excl. file (MiB) | Checksum | Reps |")
print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|---:|")
for (lib, mode, arch), rs in sorted(rows.items(), key=lambda x: (x[0][1].split("_")[0] != "parse", x[0][1], statistics.median(float(r["secs"]) for r in x[1]))):
    secs = statistics.median(float(r["secs"]) for r in rs)
    msgs = int(rs[0]["msgs"])
    rss = max(int(r["peak_rss_mib"]) for r in rs)
    ck = rs[0]["checksum"]
    ckm = "n/a" if ck == "0" else ("match" if ck == ref_checksum else "MISMATCH")
    work = rss - file_bytes // 2**20 if arch != "arm64" or lib not in ("itchfeed", "meatpy") else rss
    print(f"| {lib} | {mode} | {arch} | {msgs:,} | {secs:.2f} | {secs*1e9/msgs:.1f} | {msgs/secs/1e6:.1f} | "
          f"{file_bytes/secs/2**30:.2f} | {rss:,} | {work:,} | {ckm} | {len(rs)} |")
