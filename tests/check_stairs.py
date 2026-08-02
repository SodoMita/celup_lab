#!/usr/bin/env python3
"""v4.9.1 staircase gate: 45-degree line, staircase detection on the
FINAL image (the check the user asked for after the v4.9 rejection:
the smiley showed stair treads with the decoupled crisp base; -r 6
was "the minimal blur when stairs are no longer visible").

Fixture tests/diagline48_src.webp is a 45 deg, 4 px wide dark line on
white (blurred sigma=0.5, quantized to 45 levels -- the hard-pixelated
smiley class; the source itself is pixel art, so any output that
re-quantizes the edge onto the output lattice produces treads).

Method: per output row, locate the sub-pixel mid-level crossing x_e(y)
of the line's dark flank (the flank is TRACKED row-to-row so the two
flanks of the line never get mixed up), robust-fit a straight line,
then measure:
  * treadrun: longest run of consecutive rows whose crossing moves
    less than 0.06 px (a horizontal tread pinned to the lattice);
  * jump95: 95th-percentile adjacent-row residual step |r(y)-r(y-1)|
    (riser height; a smooth anti-aliased diagonal drifts ~1 px/row
    with near-zero residual jitter);
  * res95 (informational): 95th-percentile |residual| to the fit.

Checked configurations:
  1. SHIP2X: the user's smiley recipe (autodeblur 2x -r 6 -s 100 -g 64
     -D remap) MUST PASS -- calibrated: jump95 = 0.10 (v4.9.8).
  2. SHIP4X: the miya recipe (4x -r 2.3 -s 100 -g 16) MUST PASS --
     calibrated: jump95 = 0.02.
  3. PROBE: a deliberately crisp reproducer (autodeblur 2x -r 0.5,
     steepness 64) MUST BE FLAGGED.  If the probe ever "passes", the
     detector is toothless and the gate itself fails.  Calibrations
     follow the renderer's morphology: v4.8 measured ship .11 / probe
     .69 (threshold .45); v4.9.8's erf-gain post-map removes the
     mid-value wash band, so mid-crossing jitter shrinks across the
     board -- ship2x .098, probe .36 with treads still visible --
     and the threshold is recalibrated to .30 (ship margin ~3x).

Run from the repo root:  python3 tests/check_stairs.py
Exit code 0 = ship configs smooth, probe staircased.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
LAB = HERE.parent / "celup_lab"
SRC = HERE / "diagline48_src.webp"

SHIP2 = ["--mode", "autodeblur", "--max-mib", "1048", "-c", "linear",
         "-k", "bspline", "-r", "6", "-s", "100", "-g", "64", "-D", "remap"]
SHIP4 = ["--mode", "autodeblur", "--max-mib", "1048", "-c", "linear",
         "-k", "bspline", "-r", "2.3", "-s", "100", "-g", "16", "-D", "remap"]
PROBE = ["--mode", "autodeblur", "--max-mib", "1048", "-c", "linear",
         "-k", "bspline", "-r", "0.5", "-s", "100", "-g", "64", "-D", "remap"]
PROBE_SCALE = 2  # crisp 2x reproducer: visible treads, jump95 ~ .36

TREADRUN_MAX = 3      # output rows pinned to one crossing position
JUMP95_MAX = 0.30     # output px adjacent-row residual step (p95)
MIN_ROWS = 0.55       # fraction of rows the tracked flank must cover


def crossings(path):
    """Sub-pixel mid-level crossing candidates per output row."""
    g = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    g = g.mean(2) / 255.0
    h, w = g.shape
    cand = []
    for y in range(h):
        r = g[y]
        lo, hi = r.min(), r.max()
        out = []
        if hi - lo >= .15:
            mid = .5 * (lo + hi)
            xs = np.where((r[:-1] < mid) != (r[1:] < mid))[0]
            for x0 in xs:
                r0, r1 = r[x0], r[x0 + 1]
                if abs(r1 - r0) > 1e-6:
                    out.append(x0 + (mid - r0) / (r1 - r0))
        cand.append(out)
    # Track ONE flank: seed at the crossing nearest the image diagonal,
    # then always take the candidate nearest the previous row (the two
    # flanks are 16+ px apart, the edge moves ~1 px/row -> no jumping).
    xe = np.full(h, np.nan)
    exp = np.arange(h) * (w - 1) / max(h - 1, 1)
    besty, bestd = -1, 1e9
    for y in range(h):
        for v in cand[y]:
            d0 = abs(v - exp[y])
            if d0 < bestd:
                bestd, besty = d0, y
    if besty < 0:
        return xe
    xe[besty] = min(cand[besty], key=lambda v: abs(v - exp[besty]))
    for y in range(besty + 1, h):
        if cand[y]:
            xe[y] = min(cand[y], key=lambda v: abs(v - xe[y - 1]))
    for y in range(besty - 1, -1, -1):
        if cand[y]:
            xe[y] = min(cand[y], key=lambda v: abs(v - xe[y + 1]))
    return xe


def metrics(path):
    xe = crossings(path)
    h = xe.shape[0]
    m = int(h * .06)
    ys = np.arange(h)[m:h - m]
    xx = xe[m:h - m]
    ok = ~np.isnan(xx)
    ys, xx = ys[ok], xx[ok]
    if xx.size < h * MIN_ROWS:
        return None
    # Robust line fit: inliers within 2 px after two refits, then the
    # longest gap-free segment (protects against a derailed tracker on
    # extremely low-contrast rows).
    for _ in range(2):
        cf = np.polyfit(ys, xx, 1)
        inl = np.abs(xx - np.polyval(cf, ys)) < 2.0
        if inl.sum() > 8:
            cf = np.polyfit(ys[inl], xx[inl], 1)
    inl = np.abs(xx - np.polyval(cf, ys)) < 2.0
    seg, cur = [], []
    for y, x, v in zip(ys, xx, inl):
        if v and (not cur or y - cur[-1][0] <= 3):
            cur.append((y, x))
        else:
            if len(cur) > len(seg):
                seg = cur
            cur = [(y, x)] if v else []
    if len(cur) > len(seg):
        seg = cur
    if len(seg) < h * MIN_ROWS * (1 - 2 * .06):
        return None
    ys2 = np.array([a for a, _ in seg])
    xx2 = np.array([b for _, b in seg])
    cf = np.polyfit(ys2, xx2, 1)
    res = xx2 - np.polyval(cf, ys2)
    d = np.abs(np.diff(res))
    run = best = 0
    for i in range(1, len(ys2)):
        if ys2[i] == ys2[i - 1] + 1 and abs(xx2[i] - xx2[i - 1]) < .06:
            run += 1
        else:
            best = max(best, run)
            run = 0
    best = max(best, run) + 1
    return (best, float(np.percentile(d, 95)) if d.size else 0.,
            float(d.max()) if d.size else 0.,
            float(np.percentile(np.abs(res), 95)), len(ys2), h)


def verdict(m):
    tread, j95, jmax, r95, rows, h = m
    ok = tread <= TREADRUN_MAX and j95 <= JUMP95_MAX
    return ok, (f"treadrun={tread} (<= {TREADRUN_MAX})  jump95={j95:.3f} "
                f"(<= {JUMP95_MAX})  jumpmax={jmax:.3f}  res95={r95:.3f} "
                f" rows={rows}/{h}")


def main():
    if not SRC.exists():
        print(f"FAIL: {SRC.name} missing (run tests/make_test_sources.py)")
        return 1
    if not LAB.exists():
        print("FAIL: ./celup_lab not built")
        return 1
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        jobs = [("ship2x", 2, SHIP2, True),
                ("ship4x", 4, SHIP4, True),
                ("compress2x2_g1", 2, ["--mode", "autodeblur", "--max-mib", "1048", "-c", "linear", "-k", "bspline", "-r", "6", "-D", "compress2x2", "-g", "1"], True),
                ("compress2x2_g5", 2, ["--mode", "autodeblur", "--max-mib", "1048", "-c", "linear", "-k", "bspline", "-r", "6", "-D", "compress2x2", "-g", "5"], True),
                ("probe-crisp", PROBE_SCALE, PROBE, False)]
        for name, scale, recipe, must_pass in jobs:
            out = Path(td) / f"{name}.webp"
            r = subprocess.run([str(LAB), str(SRC), str(out), str(scale),
                                *recipe], capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL {name}: celup_lab exited {r.returncode}\n"
                      f"{r.stderr.strip()}")
                fails += 1
                continue
            m = metrics(out)
            if m is None:
                print(f"FAIL {name}: no usable edge crossings")
                fails += 1
                continue
            ok, desc = verdict(m)
            if must_pass:
                print(f"{'ok  ' if ok else 'FAIL'} {name}: {desc}")
                if not ok:
                    fails += 1
            else:
                print(f"{'ok  ' if not ok else 'FAIL'} {name}: {desc}  "
                      f"({'staircase flagged as expected' if not ok else 'NOT flagged -- detector toothless'})")
                if ok:
                    fails += 1
    print("PASS" if fails == 0 else f"{fails} FAILURES")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
