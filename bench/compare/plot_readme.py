#!/usr/bin/env python3
"""Charts for the README's Performance section.

    plot_readme.py RESULTS_DIR OUT_DIR

RESULTS_DIR holds results.txt (run_all.sh), session_*.csv (session_profile)
and engine.json (bm_itch --benchmark_filter=BM_Engine_ --benchmark_out=...).
Writes perf_overview.png, perf_session.png and perf_vendors.png into OUT_DIR.
"""
import csv
import glob
import json
import re
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

src, out = sys.argv[1], sys.argv[2]

BG, PANEL, GRID, FG, MUTED = "#0d1117", "#0d1117", "#30363d", "#e6edf3", "#8b949e"
OURS, PEER, ACCENT = "#56d364", "#6e7681", "#f0883e"
plt.rcParams.update({
    "figure.facecolor": BG, "axes.facecolor": PANEL, "savefig.facecolor": BG,
    "axes.edgecolor": GRID, "axes.labelcolor": FG, "text.color": FG,
    "xtick.color": MUTED, "ytick.color": MUTED, "grid.color": GRID,
    "font.family": "DejaVu Sans Mono", "font.size": 10,
})


def medians():
    runs = {}
    for line in open(f"{src}/results.txt"):
        if "RESULT " not in line:
            continue
        kv = dict(re.findall(r"(\w+)=(\S+)", line))
        runs.setdefault((kv["lib"], kv["mode"], kv["arch"]), []).append(float(kv["ns_per_msg"]))
    return {k: statistics.median(v) for k, v in runs.items()}


m = medians()
x86 = "x86_64"

# ---- overview: per-message cost and throughput, same machine, same file -----
book = [("parseritch\n(prefetch 16)", m[("parseritch", "book_pf16", x86)], True),
        ("parseritch", m[("parseritch", "book", x86)], True),
        ("charles-cooper*", m[("ccooper", "book", x86)], False),
        ("itchcpp", m[("itchcpp", "book", x86)], False),
        ("CppTrader", m[("cpptrader", "book", x86)], False)]
parse = [("parseritch\nfor_each_frame", m[("parseritch", "frames", x86)], True),
         ("parseritch\nParser", m[("parseritch", "parse", x86)], True),
         ("itchcpp", m[("itchcpp", "parse", x86)], False),
         ("CppTrader", m[("cpptrader", "parse", x86)], False)]

fig, axes = plt.subplots(1, 2, figsize=(13, 4.6), gridspec_kw={"width_ratios": [5, 4]})
fig.suptitle("Nasdaq TotalView-ITCH 5.0, full day 2019-01-30 (368M messages): time per message",
             color=FG, fontsize=12, y=0.99)
for ax, rows, title in ((axes[0], book, "Order books, every symbol"), (axes[1], parse, "Parse only")):
    names = [r[0] for r in rows][::-1]
    vals = [r[1] for r in rows][::-1]
    cols = [OURS if r[2] else PEER for r in rows][::-1]
    bars = ax.barh(names, vals, color=cols, height=0.6)
    top = max(vals)
    for b, v in zip(bars, vals):
        ax.text(b.get_width() + top * 0.015, b.get_y() + b.get_height() / 2,
                f"{v:.1f} ns  ({1e3 / v:.1f}M msg/s)", va="center", color=FG, fontsize=9)
    ax.set_xlim(0, top * 1.55)
    ax.set_title(title, color=FG, fontsize=11, loc="left")
    ax.set_xlabel("ns per message (lower is better)")
    ax.grid(axis="x", alpha=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
axes[0].text(0, -0.2, "* size per price level only (no per-order state)", transform=axes[0].transAxes,
             color=MUTED, fontsize=8)
fig.tight_layout()
fig.savefig(f"{out}/perf_overview.png", dpi=160)

# ---- throughput across the trading day ---------------------------------------
per_slice = {}
for path in sorted(glob.glob(f"{src}/session_*.csv")):
    for row in csv.DictReader(open(path)):
        per_slice.setdefault(float(row["session_time_s"]), []).append(float(row["mmsg_s"]))
t = sorted(per_slice)
rate = [statistics.median(per_slice[k]) for k in t]
hours = [s / 3600 for s in t]

fig, ax = plt.subplots(figsize=(13, 4.2))
ax.plot(hours, rate, color=OURS, lw=1.6, marker="o", ms=2.5)
avg = statistics.median(rate)
ax.axhline(avg, color=ACCENT, ls="--", lw=1, label=f"median slice: {avg:.1f}M msg/s")
for h, label in ((9.5, "open 09:30"), (16.0, "close 16:00")):
    ax.axvline(h, color=MUTED, ls=":", lw=1)
    ax.text(h + 0.08, max(rate) * 1.02, label, color=MUTED, fontsize=8)
ax.set_title("Book-building throughput across the session (2M-message slices, prefetch 16)",
             color=FG, fontsize=11, loc="left")
ax.set_xlabel("session time (hour of day, ET)")
ax.set_ylabel("M messages / s")
ax.set_ylim(0, max(rate) * 1.12)
ax.set_xlim(min(hours) - 0.2, max(hours) + 0.2)
ax.grid(alpha=0.6)
ax.legend(loc="lower right", facecolor=BG, edgecolor=GRID, labelcolor=FG)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
fig.tight_layout()
fig.savefig(f"{out}/perf_session.png", dpi=160)

# ---- published vendor figures next to ours, on one time scale -----------------
# (label, ns, what it measures, kind). Vendor numbers are as published, not
# reproduced; sources are listed in the README table.
FPGA, SOFT = "#58a6ff", ACCENT
vendors = [
    ("parseritch: header-only walk", m[("parseritch", "frames", x86)], "compute per msg, replay", "ours"),
    ("parseritch: parse", m[("parseritch", "parse", x86)], "compute per msg, replay", "ours"),
    ("Exegy + AMD (STAC-T0)", 13.9, "tick-to-trade, network I/O only", "fpga"),
    ("Fractal FPGA ITCH parser", 24.8, "parse: last byte in -> decoded", "fpga"),
    ("parseritch: full-depth books", m[("parseritch", "book_pf16", x86)], "compute per msg, replay", "ours"),
    ("Exegy Nexus", 350, "< 350 ns, respond to aggregated data", "fpga"),
    ("NovaSparks NovaTick", 750, "ITCH + books, wire -> FPGA core", "fpga"),
    ("NovaSparks NovaTick", 1250, "ITCH + books, wire -> server memory", "fpga"),
    ("Tickerplant (open source)", 2455, "ITCH wire -> book, p50", "soft"),
    ("Redline InRush 3 (STAC-T1)", 5200, "tick-to-trade, mean", "soft"),
    ("Aquis matching engine", 17000, "<= 17 us port-to-port, p99.99", "soft"),
]
colour = {"ours": OURS, "fpga": FPGA, "soft": SOFT}
vendors.sort(key=lambda r: r[1])

fig, ax = plt.subplots(figsize=(13, 5.8))
names = [r[0] for r in vendors][::-1]
vals = [r[1] for r in vendors][::-1]
bars = ax.barh(range(len(vals)), vals, color=[colour[r[3]] for r in vendors][::-1], height=0.62)
ax.set_yticks(range(len(vals)), names)
ax.set_xscale("log")
ax.set_xlim(1, 1e6)
ax.set_xticks([1, 10, 100, 1e3, 1e4, 1e5, 1e6], ["1 ns", "10 ns", "100 ns", "1 us", "10 us", "100 us", "1 ms"])
for b, r in zip(bars, vendors[::-1]):
    v = r[1]
    shown = (f"{v / 1000:g} us" if v >= 1000 else f"{v:g} ns") if r[3] != "ours" else f"{v:.1f} ns"
    ax.text(v * 1.12, b.get_y() + b.get_height() / 2, f"{shown}   {r[2]}", va="center", color=FG, fontsize=8.5)
ax.set_title("parseritch next to published figures (vendors' own numbers, not reproduced)",
             color=FG, fontsize=11, loc="left")
ax.set_xlabel("time, log scale (lower is better within the same kind of measurement)")
ax.grid(axis="x", alpha=0.6, which="major")
ax.set_axisbelow(True)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
from matplotlib.patches import Patch  # noqa: E402

ax.legend(handles=[Patch(color=OURS, label="parseritch: compute per message (no network)"),
                   Patch(color=FPGA, label="FPGA / hardware: includes the network path"),
                   Patch(color=SOFT, label="software / matching engine: includes the network path")],
          loc="upper right", facecolor=BG, edgecolor=GRID, labelcolor=FG, fontsize=8.5)
fig.tight_layout()
fig.savefig(f"{out}/perf_vendors.png", dpi=160)

# ---- engine microbenchmarks (printed for the README table) --------------------
for b in json.load(open(f"{src}/engine.json"))["benchmarks"]:
    if b.get("aggregate_name") == "median":
        per_item = 1e9 / b["items_per_second"]
        print(f"{b['run_name']}: {b['real_time']:.1f} ns/iter, {per_item:.1f} ns/msg, "
              f"{b['items_per_second'] / 1e6:.1f}M msg/s")
