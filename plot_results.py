#!/usr/bin/env python3
"""
plot_results.py -- turn critter_predict output into plots and a ranking.

usage: python3 plot_results.py [results_dir]

Writes to <results_dir>/plots:
  group_<g>_bars.png      median / p99 / p99.9 / max wake-up latency per run
  group_<g>_tail.png      exceedance curves: fraction of samples at least x late
  timeseries/<label>.png  sleeploop-style plot of latency vs. sample number
  impact_ranking.png      how far each factor group moved the p99.9 latency
"""
import csv
import os
import sys

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def short(label):
    return label.split("-", 1)[1] if "-" in label else label


def group_of(label):
    return label.split("-", 1)[0]


def load_lat_us(rdir, label):
    path = os.path.join(rdir, "samples", label + ".csv")
    if not os.path.exists(path):
        return None
    lat = np.loadtxt(path, delimiter=",", skiprows=1, usecols=1, ndmin=1) / 1000.0
    return np.clip(lat, 0.01, None)  # log axes need positive values


def main():
    rdir = sys.argv[1] if len(sys.argv) > 1 else "results"
    summ = os.path.join(rdir, "summary.csv")
    if not os.path.exists(summ):
        sys.exit(f"no {summ}; run ./run_experiments.sh first")

    with open(summ) as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.exit("summary.csv is empty")

    pdir = os.path.join(rdir, "plots")
    os.makedirs(os.path.join(pdir, "timeseries"), exist_ok=True)

    groups = {}
    for r in rows:
        groups.setdefault(group_of(r["label"]), []).append(r)

    def num(r, k):
        return float(r[k])

    # ------------------------------------------------------------ text table
    hdr = (f"{'run':38s} {'policy':14s} {'median':>8s} {'p99':>8s} "
           f"{'p99.9':>8s} {'max':>9s} {'miss%':>7s} {'Hz':>8s} {'invcsw':>7s}")
    print(hdr)
    print("-" * len(hdr))
    for g, rs in groups.items():
        for r in rs:
            miss = 100.0 * num(r, "misses") / num(r, "samples")
            print(f"{r['label']:38s} {r['policy']:14s} {num(r,'lat_median_us'):8.1f} "
                  f"{num(r,'lat_p99_us'):8.1f} {num(r,'lat_p999_us'):8.1f} "
                  f"{num(r,'lat_max_us'):9.1f} {miss:7.3f} {num(r,'achieved_hz'):8.1f} "
                  f"{int(num(r,'nivcsw')):7d}")
        print()

    metrics = [("lat_median_us", "median"), ("lat_p99_us", "p99"),
               ("lat_p999_us", "p99.9"), ("lat_max_us", "max")]

    # ------------------------------------------------------- bars per group
    for g, rs in groups.items():
        x = np.arange(len(rs))
        w = 0.2
        fig, ax = plt.subplots(figsize=(max(6, 1.4 * len(rs) + 2), 4.8))
        for k, (col, lab) in enumerate(metrics):
            ax.bar(x + (k - 1.5) * w, [max(num(r, col), 0.1) for r in rs], w, label=lab)
        deadlines = {r["deadline_us"] for r in rs}
        if len(deadlines) == 1:
            ax.axhline(float(deadlines.pop()), color="red", ls="--", lw=1,
                       label="soft deadline")
        ax.set_yscale("log")
        ax.set_xticks(x)
        ax.set_xticklabels([short(r["label"]) for r in rs], rotation=30, ha="right")
        ax.set_ylabel("wake-up latency (µs, log scale)")
        ax.set_title(f'Group "{g}": how late the Critter sample wakes up')
        ax.legend(fontsize=8)
        ax.grid(axis="y", which="both", alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(pdir, f"group_{g}_bars.png"), dpi=130)
        plt.close(fig)

    # ------------------------------------------------- tail curves per group
    for g, rs in groups.items():
        fig, ax = plt.subplots(figsize=(7.5, 4.8))
        plotted = 0
        for r in rs:
            lat = load_lat_us(rdir, r["label"])
            if lat is None or lat.size == 0:
                continue
            s = np.sort(lat)
            exceed = 1.0 - np.arange(s.size) / s.size
            ax.step(s, exceed, where="post", label=short(r["label"]))
            plotted += 1
        if not plotted:
            plt.close(fig)
            continue
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.set_xlabel("wake-up latency (µs)")
        ax.set_ylabel("fraction of samples at least this late")
        ax.set_title(f'Group "{g}": latency tail (further right = less predictable)')
        ax.grid(which="both", alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(os.path.join(pdir, f"group_{g}_tail.png"), dpi=130)
        plt.close(fig)

    # ------------------------------------------- sleeploop-style time series
    for r in rows:
        lat = load_lat_us(rdir, r["label"])
        if lat is None or lat.size == 0:
            continue
        fig, ax = plt.subplots(figsize=(9, 3.6))
        ax.plot(lat, ".", ms=2)
        ax.axhline(num(r, "deadline_us"), color="red", ls="--", lw=1, label="soft deadline")
        ax.set_yscale("log")
        ax.set_xlabel("sample number")
        ax.set_ylabel("wake-up latency (µs)")
        ax.set_title(f"{r['label']}  (median {num(r,'lat_median_us'):.0f} µs, "
                     f"p99.9 {num(r,'lat_p999_us'):.0f} µs, max {num(r,'lat_max_us'):.0f} µs)")
        ax.legend(fontsize=8, loc="upper right")
        fig.tight_layout()
        fig.savefig(os.path.join(pdir, "timeseries", f"{r['label']}.png"), dpi=110)
        plt.close(fig)

    # ------------------------------------------------------- impact ranking
    impacts = []
    for g, rs in groups.items():
        if len(rs) < 2:
            continue
        p = [max(num(r, "lat_p999_us"), 0.1) for r in rs]
        lo, hi = min(p), max(p)
        impacts.append((g, hi / lo, rs[p.index(lo)]["label"], rs[p.index(hi)]["label"]))
    impacts.sort(key=lambda t: t[1])

    if impacts:
        fig, ax = plt.subplots(figsize=(7.5, 0.6 * len(impacts) + 1.8))
        ax.barh([i[0] for i in impacts], [i[1] for i in impacts])
        ax.set_xscale("log")
        ax.set_xlabel("worst / best p99.9 latency within the group (×, log scale)")
        ax.set_title("Which factor moves Critter predictability the most?")
        ax.grid(axis="x", which="both", alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(pdir, "impact_ranking.png"), dpi=130)
        plt.close(fig)

        print("Impact ranking (spread of p99.9 latency within each group):")
        for g, ratio, best, worst in reversed(impacts):
            print(f"  {g:8s} {ratio:8.1f}x   best: {best:34s} worst: {worst}")
        print("\nCaveat: groups use different baseline loads, so treat this as a first")
        print("ordering and confirm it with the per-group tail plots and repeated runs.")

    print(f"\nPlots written to {pdir}")


if __name__ == "__main__":
    main()
