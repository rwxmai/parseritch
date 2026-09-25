#!/usr/bin/env python3
"""Plot kernel comparisons from a Google Benchmark JSON file.

    ./build/bm_itch --benchmark_out=results.json --benchmark_out_format=json \
                    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
    python3 tools/plot_benchmarks.py results.json docs/img

Benchmarks are named BM_<Family>/<variant>/<arg>... . For every family, this
draws one grouped bar chart of ns/op: one group per argument set, one bar per
variant. With repetitions, the median is plotted and the error bar spans
median +/- stddev.

Refuses to plot results recorded under Rosetta 2 (the benchmark binary tags
them), because translated timings say nothing about the x86 kernels.
"""

import argparse
import collections
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def load(path):
    with open(path) as f:
        data = json.load(f)
    return data["context"], data["benchmarks"]


def ns(b):
    scale = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}[b.get("time_unit", "ns")]
    return b["real_time"] * scale


def collect(benchmarks):
    """family -> args -> variant -> {'median': ns, 'stddev': ns}"""
    out = collections.defaultdict(lambda: collections.defaultdict(dict))
    for b in benchmarks:
        if b.get("error_occurred") or b.get("skipped"):
            continue
        agg = b.get("aggregate_name")
        if agg not in (None, "median", "stddev"):
            continue
        name = b.get("run_name", b["name"])
        parts = name.split("/")
        if len(parts) < 2:
            continue
        family, variant, args = parts[0], parts[1], "/".join(parts[2:]) or "-"
        entry = out[family][args].setdefault(variant, {"median": None, "stddev": 0.0})
        if agg == "stddev":
            entry["stddev"] = ns(b)
        else:
            entry["median"] = ns(b)
    return out


def plot_family(family, groups, simd_level, out_dir):
    variants = sorted({v for g in groups.values() for v in g})
    arg_sets = list(groups.keys())
    width = 0.8 / max(len(variants), 1)
    fig, ax = plt.subplots(figsize=(max(6.0, 1.6 * len(arg_sets) + 2), 4.2))
    for i, v in enumerate(variants):
        xs, ys, es = [], [], []
        for j, a in enumerate(arg_sets):
            e = groups[a].get(v)
            if e and e["median"] is not None:
                xs.append(j + (i - (len(variants) - 1) / 2) * width)
                ys.append(e["median"])
                es.append(e["stddev"])
        ax.bar(xs, ys, width, yerr=es, capsize=2, label=v)
    ax.set_xticks(range(len(arg_sets)))
    ax.set_xticklabels(arg_sets, rotation=20, ha="right", fontsize=8)
    ax.set_ylabel("ns / op (lower is better)")
    ax.set_title(f"{family}  [{simd_level} build]")
    ax.grid(axis="y", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    path = os.path.join(out_dir, f"{family}.png")
    fig.savefig(path, dpi=150)
    plt.close(fig)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results")
    ap.add_argument("out_dir")
    ap.add_argument("--families", nargs="*", help="only these BM_ families")
    args = ap.parse_args()

    ctx, benchmarks = load(args.results)
    if "WARNING" in ctx and "Rosetta" in str(ctx["WARNING"]):
        sys.exit("refusing to plot: results were recorded under Rosetta 2 translation")
    os.makedirs(args.out_dir, exist_ok=True)
    simd_level = ctx.get("itch_simd_level", "?")
    for family, groups in sorted(collect(benchmarks).items()):
        if args.families and family not in args.families:
            continue
        print(plot_family(family, groups, simd_level, args.out_dir))


if __name__ == "__main__":
    main()
