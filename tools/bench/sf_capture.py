#!/usr/bin/env python3
"""LURVETA Performance — frame-time capture over adb (works for ANY app, e.g. Winlator vs LURVETA).

Uses `dumpsys SurfaceFlinger --latency <layer>`, which returns the last ~127 frames of a layer
(column 2 = actual present time in ns). The script polls it, merges the samples and writes
frames.csv in the same format as LURVETA's in-app recorder (t_ms,frametime_ms), so both apps are
measured with the SAME method.

  python3 sf_capture.py --list                       # list layers, pick the game surface
  python3 sf_capture.py --layer "SurfaceView[com.winlator/...]#0" --seconds 120 --out A_run1.csv
"""
import argparse, subprocess, sys, time

def adb(*args):
    return subprocess.run(["adb", "shell", *args], capture_output=True, text=True).stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--layer")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--out", default="frames.csv")
    a = ap.parse_args()
    if a.list:
        print(adb("dumpsys", "SurfaceFlinger", "--list")); return
    if not a.layer:
        sys.exit("--layer is required (use --list)")
    adb("dumpsys", "SurfaceFlinger", "--latency-clear", a.layer)
    presents, end = set(), time.time() + a.seconds
    while time.time() < end:
        lines = adb("dumpsys", "SurfaceFlinger", "--latency", a.layer).strip().splitlines()[1:]
        for line in lines:
            cols = line.split()
            if len(cols) == 3:
                t = int(cols[1])
                if 0 < t < (1 << 62): presents.add(t)   # INT64_MAX marks pending frames
        time.sleep(0.5)                                  # 127 frames buffer: safe up to ~250 FPS
    ts = sorted(presents)
    if len(ts) < 2: sys.exit("no frames captured (wrong layer? game paused?)")
    with open(a.out, "w") as f:
        f.write("t_ms,frametime_ms\n")
        for prev, cur in zip(ts, ts[1:]):
            f.write(f"{(cur - ts[0]) / 1e6:.3f},{(cur - prev) / 1e6:.3f}\n")
    print(f"{len(ts) - 1} frames -> {a.out}")

if __name__ == "__main__":
    main()
