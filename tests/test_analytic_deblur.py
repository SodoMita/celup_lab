#!/usr/bin/env python3
"""Tests for the ``analytic`` autodeblur deblur method (-D analytic).

The analytic method is a second, opt-in deblur that follows a different
prescription from remap/push: it PRODUCES a full-image gradient field, EDITS
it (narrows every transition by pushing its two plateau colours toward each
other), and SAMPLES from it.  Its -g semantics are INVERTED relative to
remap/push: 1 = maximum deblur (collapse each gradient to one point =
quantize), larger K = LESS deblur, infinity = identity; 0 = auto.

This test pins the contract:
  1. SELECTABLE:  -D analytic runs and reports method=analytic.
  2. INVERTED -g: K=1 is the MAXIMUM (quantize); larger K is LESS deblur, and
     the steepening is monotone in K.
  3. QUANTIZE at K=1: a blurred edge collapses to a near-hard step.
  4. IDENTITY at large K: a very large K leaves the image ~unchanged.
  5. NO RINGING VOCABULARY (invariant #1): output stays inside the source's
     per-channel premultiplied-linear range (0 hull violations).
  6. FLAT-PRESERVING: a flat region is left untouched.

Run from the repo root:  python3 tests/test_analytic_deblur.py
Exit code 0 = all checks pass.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
LAB = HERE.parent / "celup_lab"


def pm(arr):
    a = arr.astype(np.float64) / 255.0
    lin = np.where(a[..., :3] <= .04045, a[..., :3] / 12.92,
                   ((a[..., :3] + .055) / 1.055) ** 2.4)
    o = np.empty_like(a)
    o[..., :3] = lin * a[..., 3:4]
    o[..., 3] = a[..., 3]
    return o


def pm_lum(arr):
    return pm(arr)[..., :3].mean(-1)


def run(src, out, scale, args):
    r = subprocess.run([str(LAB), str(src), str(out), str(scale)] + args,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("celup_lab failed: " + r.stderr.strip())
    return r.stdout + r.stderr


def make_blurred_step(path, w=64, h=64, sigma=2.2):
    """A clean horizontal blurred edge, sRGB-encoded, opaque."""
    z = (np.arange(w) - (w / 2 - 0.5)) / (sigma * 1.6)
    prof = np.clip(0.5 * (1 + np.tanh(z)), 0, 1) ** (1 / 2.2)
    img = np.zeros((h, w, 4), np.uint8)
    for c in range(3):
        img[:, :, c] = (prof * 255).astype(np.uint8)[None, :]
    img[:, :, 3] = 255
    Image.fromarray(img, "RGBA").save(path, lossless=True)


def mid_row(g):
    rng = g.max(1) - g.min(1)
    return g[int(np.argmax(rng))]


def peak_slope(g):
    row = mid_row(g)
    return float(np.abs(np.diff(row)).max())


def width_25_75(g):
    row = mid_row(g)
    lo, hi = row.min(), row.max()
    if hi - lo < 1e-3:
        return 0
    a = int(np.argmax(row >= lo + 0.25 * (hi - lo)))
    b = len(row) - 1 - int(np.argmax(row[::-1] <= lo + 0.75 * (hi - lo)))
    return b - a


def hull_violations(out_arr, src_arr, eps=1.5e-3):
    sp, op = pm(src_arr), pm(out_arr)
    n = 0
    for c in range(4):
        lo, hi = sp[..., c].min(), sp[..., c].max()
        n += int(((op[..., c] < lo - eps) | (op[..., c] > hi + eps)).sum())
    return n


def main():
    if not LAB.exists():
        print("FAIL: ./celup_lab not built")
        return 1
    fails = 0
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        src = td / "step.webp"
        make_blurred_step(src)

        scale = 4
        common = ["--mode", "autodeblur", "-c", "linear", "-k", "bspline",
                  "-r", "2.3", "-s", "100", "-M", "2048"]

        # base (autoblur) for reference
        bout = td / "base.webp"
        run(src, bout, scale, ["--mode", "autoblur", "-c", "linear",
                               "-k", "bspline", "-r", "2.3", "-M", "2048"])
        base = np.asarray(Image.open(bout).convert("RGBA"))
        bslp = peak_slope(pm_lum(base))
        bw = width_25_75(pm_lum(base))

        # 1. selectable + reports method=analytic
        aout = td / "a.webp"
        log = run(src, aout, scale, common + ["-D", "analytic", "-g", "2"])
        if "method=analytic" not in log or "analytic K=2" not in log:
            print("FAIL selectable: -D analytic did not report method=analytic")
            fails += 1
        else:
            print("ok   selectable: -D analytic -> method=analytic reported")

        # 2 + 3 + 4. monotone steepening, quantize at K=1, identity at large K
        ks = ["1.0", "1.3", "1.6", "2.0", "3.0", "6.0", "16.0", "32.0"]
        slopes, widths = {}, {}
        for k in ks:
            o = td / f"a_{k}.webp"
            run(src, o, scale, common + ["-D", "analytic", "-g", k])
            g = pm_lum(np.asarray(Image.open(o).convert("RGBA")))
            slopes[k] = peak_slope(g)
            widths[k] = width_25_75(g)
        # monotone: slope decreases as K increases (1=max -> 32=least)
        mono = all(sloves_ok for sloves_ok in
                   [slopes[ks[i]] >= slopes[ks[i + 1]] - 1e-9
                    for i in range(len(ks) - 1)])
        if not mono:
            print("FAIL monotone: slopes not non-increasing in K:",
                  {k: round(v, 4) for k, v in slopes.items()})
            fails += 1
        else:
            print("ok   monotone: peak slope non-increasing in K "
                  f"(K=1:{slopes['1.0']:.4f} -> K=32:{slopes['32.0']:.4f}, "
                  f"base:{bslp:.4f})")
        # K=1 quantizes: much steeper than base, near-hard step
        if slopes["1.0"] < 5 * bslp or widths["1.0"] > 2:
            print(f"FAIL quantize: K=1 slope {slopes['1.0']:.4f} (<5x base "
                  f"{bslp:.4f}) or 25-75 width {widths['1.0']}px (>2)")
            fails += 1
        else:
            print(f"ok   quantize: K=1 peak slope {slopes['1.0']:.4f} "
                  f"(>=5x base), 25-75 width {widths['1.0']}px (hard step)")
        # large K ~ identity: close to base slope
        if slopes["32.0"] < bslp * 0.9 or slopes["32.0"] > bslp * 1.25:
            print(f"FAIL identity: K=32 slope {slopes['32.0']:.4f} not near "
                  f"base {bslp:.4f}")
            fails += 1
        else:
            print(f"ok   identity: K=32 peak slope {slopes['32.0']:.4f} ~ "
                  f"base {bslp:.4f}")

        # 5. hull invariant on the K=2 output (vs the SOURCE global range)
        a2 = np.asarray(Image.open(td / "a_2.0.webp").convert("RGBA"))
        src_arr = np.asarray(Image.open(src).convert("RGBA"))
        viol = hull_violations(a2, src_arr)
        if viol != 0:
            print(f"FAIL hull: analytic K=2 has {viol} per-channel violations "
                  "outside the source range")
            fails += 1
        else:
            print("ok   hull: analytic K=2 stays inside source per-channel "
                  "range (no ringing vocabulary)")

        # 6. flat-preserving: build an image with a flat grey region and a
        # step, check the flat's std does not increase under analytic.
        fw, fh = 96, 48
        flat = np.full((fh, fw, 4), 0, np.uint8)
        flat[:, :, 0] = 160
        flat[:, :, 1] = 160
        flat[:, :, 2] = 160
        flat[:, :, 3] = 255
        z = (np.arange(48) - 24) / (2.2 * 1.6)
        prof = np.clip(0.5 * (1 + np.tanh(z)), 0, 1) ** (1 / 2.2) * 255
        step_strip = np.zeros((fh, 48, 4), np.uint8)
        for c in range(3):
            step_strip[:, :, c] = prof.astype(np.uint8)[None, :]
        step_strip[:, :, 3] = 255
        both = np.concatenate([flat, step_strip], axis=1)  # 96x96? no: 96+48
        fpath = td / "flatstep.webp"
        Image.fromarray(both, "RGBA").save(fpath, lossless=True)
        fout = td / "flatstep_out.webp"
        run(fpath, fout, scale, common + ["-D", "analytic", "-g", "1.5"])
        fo = pm_lum(np.asarray(Image.open(fout).convert("RGBA")))
        fi = pm_lum(both)
        # flat region is the left half at the OUTPUT resolution
        half = fo.shape[1] // 3  # flat occupies first 1/3
        fstd_in = fi[:, :both.shape[1] // 3].std()
        fstd_out = fo[:, :half].std()
        if fstd_out > fstd_in * 1.5 + 1e-4:
            print(f"FAIL flat: flat-region std rose {fstd_in:.5f} -> "
                  f"{fstd_out:.5f}")
            fails += 1
        else:
            print(f"ok   flat: flat-region std {fstd_in:.5f} -> "
                  f"{fstd_out:.5f} (preserved)")

    print("PASS" if not fails else f"{fails} FAILURES")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
