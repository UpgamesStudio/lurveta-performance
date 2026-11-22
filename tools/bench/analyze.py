#!/usr/bin/env python3
"""LURVETA Performance — benchmark analysis (same definitions as FrameStats.java).

  python3 analyze.py --a A_run1.csv A_run2.csv A_run3.csv --b B_run1.csv B_run2.csv B_run3.csv

Per run: average FPS, 1% / 0.1% low (1000 / mean of the slowest 1% / 0.1% frametimes),
frametime p50/p95/p99/max and standard deviation, plus stability: hitches (frames > 100 ms) per
minute, stalls (frames > 1 s) and frozen time (gaps >= 5 s, i.e. the game showed nothing). Across runs: mean ± stdev per metric, and the
difference B-A is only reported as significant when it exceeds the combined run-to-run spread
(|mean_B - mean_A| > stdev_A + stdev_B). A single run or a single peak is never conclusive.
"""
import argparse, csv, math, statistics as st, sys

def load(path):
    with open(path) as f:
        return [float(r["frametime_ms"]) for r in csv.DictReader(f) if float(r["frametime_ms"]) > 0]

def low(sorted_ft, fraction):
    n = max(1, math.ceil(len(sorted_ft) * fraction))
    return 1000.0 / (sum(sorted_ft[-n:]) / n)

def pct(sorted_ft, p):
    return sorted_ft[min(len(sorted_ft) - 1, max(0, math.ceil(p * len(sorted_ft)) - 1))]

def metrics(ft):
    s = sorted(ft)
    total = sum(s)
    return {"avg_fps": 1000.0 * len(s) / total, "low1_fps": low(s, 0.01), "low01_fps": low(s, 0.001),
            "p50_ms": pct(s, .5), "p95_ms": pct(s, .95), "p99_ms": pct(s, .99), "max_ms": s[-1],
            "stdev_ms": st.pstdev(s), "frames": len(s), "seconds": total / 1000,
            "hitches_per_min": sum(1 for x in s if x > 100) / (total / 60000.0),
            "stalls": sum(1 for x in s if x > 1000),
            "frozen_s": sum(x for x in s if x >= 5000) / 1000.0}

def summarize(name, paths):
    runs = [metrics(load(p)) for p in paths]
    print(f"\n== {name} ({len(runs)} runs)")
    for p, m in zip(paths, runs):
        print(f"  {p}: avg {m['avg_fps']:.1f} | 1% {m['low1_fps']:.1f} | 0.1% {m['low01_fps']:.1f} | p99 {m['p99_ms']:.1f} ms"
              f" | hitches/min {m['hitches_per_min']:.1f} | stalls {m['stalls']} | frozen {m['frozen_s']:.0f} s | {m['seconds']:.0f} s")
    agg = {}
    for k in runs[0]:
        vals = [r[k] for r in runs]
        agg[k] = (st.mean(vals), st.stdev(vals) if len(vals) > 1 else float("nan"))
    return agg

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", nargs="+", required=True, help="baseline runs (e.g. Winlator 11.2)")
    ap.add_argument("--b", nargs="+", required=True, help="candidate runs (e.g. LURVETA Performance)")
    a = ap.parse_args()
    A, B = summarize("A", a.a), summarize("B", a.b)
    print("\n== B vs A (mean ± stdev across runs)")
    for k in ["avg_fps", "low1_fps", "low01_fps", "p95_ms", "p99_ms", "stdev_ms", "hitches_per_min", "stalls", "frozen_s"]:
        (ma, sa), (mb, sb) = A[k], B[k]
        diff = mb - ma
        spread = (0 if math.isnan(sa) else sa) + (0 if math.isnan(sb) else sb)
        verdict = "inconclusive (need >= 2 runs each)" if math.isnan(sa) or math.isnan(sb) else ("significant" if abs(diff) > spread else "within run-to-run variance")
        rel = f"{diff / ma * 100:+.1f}%" if ma else "n/a"
        print(f"  {k:15s} A {ma:8.2f} ± {sa:5.2f} | B {mb:8.2f} ± {sb:5.2f} | diff {diff:+8.2f} ({rel}) -> {verdict}")

if __name__ == "__main__":
    main()
