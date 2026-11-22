#!/usr/bin/env python3
"""Checks tools/bench/analyze.py metrics on synthetic frame logs with known answers."""
import os, sys, tempfile, importlib.util
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("analyze", os.path.join(here, "..", "bench", "analyze.py"))
analyze = importlib.util.module_from_spec(spec); spec.loader.exec_module(analyze)

def write(frametimes):
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
    f.write("t_ms,frametime_ms\n"); t = 0
    for ft in frametimes:
        t += ft; f.write(f"{t:.3f},{ft:.3f}\n")
    f.close(); return f.name

fails = 0
def check(cond, msg):
    global fails
    if not cond: print("FAIL:", msg); fails += 1

# 990 frames of 16.667 ms + 9 frames of 150 ms + 1 frame of 6000 ms (a 6 s freeze)
ft = [16.667] * 990 + [150.0] * 9 + [6000.0]
m = analyze.metrics(analyze.load(write(ft)))
total = sum(ft)
check(abs(m["avg_fps"] - 1000 * len(ft) / total) < 1e-6, "avg_fps")
check(abs(m["low1_fps"] - 1000 / ((150 * 9 + 6000) / 10)) < 1e-6, "1% low = 1000 / mean of slowest 10 frames")
check(abs(m["low01_fps"] - 1000 / 6000) < 1e-6, "0.1% low = slowest frame")
check(m["stalls"] == 1, "one stall")
check(abs(m["frozen_s"] - 6.0) < 1e-6, "6 s frozen")
check(abs(m["hitches_per_min"] - 10 / (total / 60000)) < 1e-6, "10 hitches")
check(m["p50_ms"] == 16.667, "median")
print("ALL ANALYZE TESTS PASSED" if not fails else f"{fails} FAILURES")
sys.exit(1 if fails else 0)
