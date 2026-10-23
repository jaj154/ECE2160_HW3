#!/usr/bin/env python3
"""
analyze_mechanism.py -- turn the -Q attribution columns into mechanism evidence.

usage: python3 analyze_mechanism.py [results_dir]

Reads every samples/*.csv that has the attribution columns (runs made with -Q) and answers
the two causal questions behind the Critter predictability study:

  Q1. Are late wake-ups caused by waiting for the CPU (scheduler contention)?
      For every sample later than 1 ms, compare the lateness with the time the sampler
      thread spent runnable-but-not-running during that sleep (runqueue wait, from
      /proc/thread-self/schedstat).  wait/latency close to 1 means the lateness IS scheduler
      wait; close to 0 means the thread was late for another reason (timer, interrupt, ...).

  Q2. Are sensor-read stalls caused by waiting for the CPU or by blocking on something else?
      Each read longer than 1 ms is split into CPU time (CLOCK_THREAD_CPUTIME_ID), runqueue
      wait (schedstat) and the remainder, which is time the thread was neither running nor
      runnable, i.e. BLOCKED (lock, device, I/O).  Voluntary vs involuntary context switches
      during the read give the same split independently: a voluntary switch means the thread
      itself went to sleep (blocked); an involuntary switch means it was preempted.

Writes <results_dir>/mechanism_summary.csv and plots/mechanism_*.png.
"""
import csv
import os
import sys

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

LATE_NS = 1_000_000   # "late" and "stall" threshold: 1 ms


def load(path):
    with open(path) as f:
        header = f.readline().strip().split(",")
    if "sleep_wait_ns" not in header:
        return None
    d = np.loadtxt(path, delimiter=",", skiprows=1, ndmin=2)
    return {name: d[:, i] for i, name in enumerate(header)}


def analyze(label, c):
    r = {"label": label, "samples": len(c["lat_ns"])}
    lat, wait = c["lat_ns"], c["sleep_wait_ns"]
    late = lat > LATE_NS
    r["late_n"] = int(late.sum())
    if late.sum():
        share = wait[late] / lat[late]
        r["late_wait_share_median"] = float(np.median(share))
        r["late_wait_ge80pct"] = float(100 * (share >= 0.8).mean())
        r["late_time_explained_pct"] = float(100 * wait[late].sum() / lat[late].sum())
        r["late_median_us"] = float(np.median(lat[late]) / 1000)
        r["late_p90_us"] = float(np.percentile(lat[late], 90) / 1000)
        r["late_in_3p5_4p5ms_pct"] = float(100 * ((lat[late] > 3.5e6) & (lat[late] < 4.5e6)).mean())
    work = c["work_ns"]
    r["reads"] = int((work > 0).sum())
    if r["reads"]:
        st = work > LATE_NS
        r["stall_n"] = int(st.sum())
        r["stall_pct"] = float(100 * st.mean())
        r["read_p99_us"] = float(np.percentile(work, 99) / 1000)
        r["read_max_us"] = float(work.max() / 1000)
        if st.sum():
            cpu, w = c["rd_cpu_ns"][st], c["rd_wait_ns"][st]
            tot = work[st]
            blocked = np.clip(tot - cpu - w, 0, None)
            s = tot.sum()
            r["stall_cpu_pct"] = float(100 * cpu.sum() / s)
            r["stall_wait_pct"] = float(100 * w.sum() / s)
            r["stall_blocked_pct"] = float(100 * blocked.sum() / s)
            r["stall_ge1_vcsw_pct"] = float(100 * (c["rd_vcsw"][st] > 0).mean())
            r["stall_ge1_ivcsw_pct"] = float(100 * (c["rd_ivcsw"][st] > 0).mean())
    return r


def main():
    rdir = sys.argv[1] if len(sys.argv) > 1 else "results"
    sdir = os.path.join(rdir, "samples")
    rows = []
    for fn in sorted(os.listdir(sdir)):
        if not fn.endswith(".csv"):
            continue
        c = load(os.path.join(sdir, fn))
        if c is not None:
            rows.append(analyze(fn[:-4], c))
    if not rows:
        sys.exit("no runs with attribution columns; run with -Q (e.g. ./run_experiments.sh mech)")

    keys = []
    for r in rows:
        for k in r:
            if k not in keys:
                keys.append(k)
    with open(os.path.join(rdir, "mechanism_summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)

    print("\nQ1. Late wake-ups (> 1 ms): how much of the lateness is runqueue wait?")
    print(f"{'run':30s}{'late n':>7s}{'median wait/lat':>17s}{'>=80% wait':>12s}{'time explained':>16s}{'median late us':>16s}{'in 3.5-4.5 ms':>15s}")
    for r in rows:
        if r["late_n"]:
            print(f"{r['label']:30s}{r['late_n']:7d}{r['late_wait_share_median']:17.2f}{r['late_wait_ge80pct']:11.0f}%"
                  f"{r['late_time_explained_pct']:15.0f}%{r['late_median_us']:16.0f}{r['late_in_3p5_4p5ms_pct']:14.0f}%")
        else:
            print(f"{r['label']:30s}{0:7d}   (no wake-up later than 1 ms)")

    sens = [r for r in rows if r.get("reads")]
    if sens:
        print("\nQ2. Sensor reads: how often does a read stall (> 1 ms), and what is a stall made of?")
        print(f"{'run':30s}{'stall %':>9s}{'stall n':>9s}{'p99 us':>10s}{'max us':>10s}{'CPU':>7s}{'wait':>7s}{'BLOCKED':>9s}{'>=1 vol':>9s}{'>=1 invol':>10s}")
        for r in sens:
            if r.get("stall_n"):
                print(f"{r['label']:30s}{r['stall_pct']:9.2f}{r['stall_n']:9d}{r['read_p99_us']:10.0f}{r['read_max_us']:10.0f}"
                      f"{r['stall_cpu_pct']:6.0f}%{r['stall_wait_pct']:6.0f}%{r['stall_blocked_pct']:8.0f}%"
                      f"{r['stall_ge1_vcsw_pct']:8.0f}%{r['stall_ge1_ivcsw_pct']:9.0f}%")
            else:
                print(f"{r['label']:30s}{0:9.2f}{0:9d}{r['read_p99_us']:10.0f}{r['read_max_us']:10.0f}   (no read longer than 1 ms)")

    pdir = os.path.join(rdir, "plots")
    os.makedirs(pdir, exist_ok=True)
    stalls = [r for r in sens if r.get("stall_n")]
    if stalls:
        fig, ax = plt.subplots(figsize=(7, 0.45 * len(stalls) + 1.6))
        y = np.arange(len(stalls))
        cpu = [r["stall_cpu_pct"] for r in stalls]
        wt = [r["stall_wait_pct"] for r in stalls]
        bl = [r["stall_blocked_pct"] for r in stalls]
        ax.barh(y, cpu, color="#4c9a4c", label="CPU time")
        ax.barh(y, wt, left=cpu, color="#d98c2b", label="waiting for a CPU (runqueue)")
        ax.barh(y, bl, left=np.array(cpu) + np.array(wt), color="#7d3c98", label="blocked (not runnable)")
        ax.set_yticks(y)
        ax.set_yticklabels([r["label"].replace("mech-", "") for r in stalls], fontsize=8)
        ax.set_xlabel("share of time inside stalled sensor reads (%)")
        ax.set_xlim(0, 100)
        ax.legend(fontsize=8, loc="upper center", bbox_to_anchor=(0.5, -0.18), ncol=3, frameon=False)
        fig.tight_layout()
        fig.savefig(os.path.join(pdir, "mechanism_sensor_stalls.png"), dpi=150)
        plt.close(fig)

    q = [r for r in rows if r["label"].startswith("mech-q_c") and r["late_n"]]
    if q:
        q.sort(key=lambda r: int(r["label"].split("c")[-1]))
        fig, ax = plt.subplots(figsize=(5.5, 3.3))
        n = [int(r["label"].split("c")[-1]) for r in q]
        ax.plot(n, [r["late_median_us"] / 1000 for r in q], "o-", label="median late wake-up")
        ax.plot(n, [r["late_p90_us"] / 1000 for r in q], "s--", label="90th percentile of late wake-ups")
        ax.set_xlabel("compute threads sharing the CPU")
        ax.set_ylabel("lateness of late wake-ups (ms)")
        ax.set_xscale("log", base=2)
        ax.set_xticks(n)
        ax.set_xticklabels([str(i) for i in n])
        ax.grid(alpha=.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(os.path.join(pdir, "mechanism_quantum_vs_threads.png"), dpi=150)
        plt.close(fig)
    print(f"\nWrote {os.path.join(rdir, 'mechanism_summary.csv')} and plots in {pdir}")


if __name__ == "__main__":
    main()
