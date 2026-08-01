#!/usr/bin/env python3
"""autodeblur regression harness -- a numeric fingerprint of the autodeblur
mode on the torture fixtures, so every code diff is measurable against a
fixed reference.  Run from the repo root:

    python3 tests/autodeblur_regression.py            # print + write fingerprint
    python3 tests/autodeblur_regression.py --ref FILE # compare to a saved one

It DOES NOT pass/fail (use check_corners.py / check_stairs.py / test_scales.py
for the gates); it prints a table and writes autodeblur_regression.txt so a
diff's effect on every measured axis is one-glance obvious.  Combined with the
gates it gives both the "must stay green" (invariants) and the "did it help"
(improvement) pictures.

Metrics per fixture (premultiplied-linear RGBA unless noted):
  mae_vs_base   autodeblur vs the autoblur BASE on the same fixture (how much
                the deblur moved pixels -- expect small, sharpness-driven)
  sharp_ratio   mean strong-edge slope(deblur) / mean strong-edge slope(base)
                (>1 = deblur steepens; ==1 = inert there)
  hull_viol     count of output colour px outside the local 3x3 source envelope
                (+-1.5e-3 eps) -- MUST be ~0 (invariant #1: no ringing vocab)
  jump95        45-degree staircase adjacent-row residual step on diagline48
  hf_noise      HF std on rampnoise48 flats (diffusion-salt behaviour)
  hue_keep      mean saturation(huearc48) deblur / base (hue-arc not collapsed)
"""
import argparse, json, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
LAB = HERE.parent / "celup_lab"
SRC = HERE
RECIPE = ["--mode", "autodeblur", "--max-mib", "2048", "-c", "linear",
          "-k", "bspline", "-s", "100", "-D", "remap"]
BASE = ["--mode", "autoblur", "--max-mib", "2048", "-c", "linear", "-k", "bspline"]


def lin(x):
    x = x / 255.
    return np.where(x <= .04045, x / 12.92, ((x + .055) / 1.055) ** 2.4)


def pm(arr):
    o = np.empty_like(arr, dtype=np.float64)
    o[..., :3] = lin(arr[..., :3]) * (arr[..., 3:4] / 255.)
    o[..., 3] = arr[..., 3] / 255.
    return o


def run(out, src, scale, recipe):
    r = subprocess.run([str(LAB), str(src), str(out), str(scale), *recipe],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("celup_lab failed: %s" % r.stderr.strip())
    return np.asarray(Image.open(out).convert("RGBA"), dtype=np.float64)


def mae(a, b):
    pa, pb = pm(a), pm(b)
    h = min(pa.shape[0], pb.shape[0])
    w = min(pa.shape[1], pb.shape[1])
    pa, pb = pa[2:h - 2, 2:w - 2], pb[2:h - 2, 2:w - 2]
    return float(np.abs(pa - pb).mean())


def edge_slope(pmimg):
    """mean |luma gradient| at strong-edge pixels (Sobel, premultiplied luma)."""
    g = pmimg[..., :3].mean(-1)
    gy = np.zeros_like(g)
    gx = np.zeros_like(g)
    gy[1:-1] = (g[2:] - g[:-2]) * .5
    gx[:, 1:-1] = (g[:, 2:] - g[:, :-2]) * .5
    mag = np.sqrt(gx * gx + gy * gy)
    thr = np.percentile(mag, 92)
    strong = mag > thr
    return float(mag[strong].mean()) if strong.any() else 0.0


def sharp_ratio(deblur, base):
    pd, pb = pm(deblur), pm(base)
    return edge_slope(pd) / max(edge_slope(pb), 1e-9)


def hull_violations(deblur, src, scale):
    """deblur output px outside the SOURCE global per-channel range (the
    strongest "no ringing vocabulary" check -- invariant #1: a deblur may
    never invent a colour the source did not contain).  The C hull clamp
    bounds output to the base render's local window, which is itself a
    subset of the source range, so this MUST be 0.  Nonzero = real
    overshoot/halo = a gate was broken."""
    s = pm(np.asarray(Image.open(src).convert("RGBA"), dtype=np.float64))
    pd = pm(deblur)
    n = 0
    for c in range(4):
        lo, hi = s[..., c].min(), s[..., c].max()
        eps = 1.5e-3
        n += int(((pd[..., c] < lo - eps) | (pd[..., c] > hi + eps)).sum())
    return n


def jump95_diag(path):
    """reuse the check_stairs.py crossing tracker (compact copy)."""
    g = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64).mean(2) / 255.0
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
                if abs(r[x0 + 1] - r[x0]) > 1e-6:
                    out.append(x0 + (mid - r[x0]) / (r[x0 + 1] - r[x0]))
        cand.append(out)
    xe = np.full(h, np.nan)
    exp = np.arange(h) * (w - 1) / max(h - 1, 1)
    besty, bestd = -1, 1e9
    for y in range(h):
        for v in cand[y]:
            if abs(v - exp[y]) < bestd:
                bestd, besty = abs(v - exp[y]), y
    if besty < 0:
        return 0.0
    xe[besty] = min(cand[besty], key=lambda v: abs(v - exp[besty]))
    for y in range(besty + 1, h):
        if cand[y]:
            xe[y] = min(cand[y], key=lambda v: abs(v - xe[y - 1]))
    for y in range(besty - 1, -1, -1):
        if cand[y]:
            xe[y] = min(cand[y], key=lambda v: abs(v - xe[y + 1]))
    m = int(h * .06)
    ys = np.arange(h)[m:h - m]
    xx = xe[m:h - m]
    ok = ~np.isnan(xx)
    ys, xx = ys[ok], xx[ok]
    if xx.size < 16:
        return 0.0
    cf = np.polyfit(ys, xx, 1)
    inl = np.abs(xx - np.polyval(cf, ys)) < 2.0
    cf = np.polyfit(ys[inl], xx[inl], 1)
    res = xx - np.polyval(cf, ys)
    d = np.abs(np.diff(res))
    return float(np.percentile(d, 95)) if d.size else 0.0


def hf_noise(pmimg):
    """HF std (laplacian of luma) on rampnoise48 -- lower = cleaner flats."""
    g = pmimg[..., :3].mean(-1)
    lap = (4 * g[1:-1, 1:-1] - g[:-2, 1:-1] - g[2:, 1:-1] -
           g[1:-1, :-2] - g[1:-1, 2:])
    return float(lap.std())


def hue_sat(pmimg):
    rgb = np.clip(pmimg[..., :3], 0, 1)
    mx = rgb.max(-1)
    mn = rgb.min(-1)
    sat = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0.0)
    return float(sat[mx > 1e-4].mean()) if (mx > 1e-4).any() else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", help="saved fingerprint JSON to diff against")
    ap.add_argument("--write", default="autodeblur_regression.txt",
                    help="output file (default autodeblur_regression.txt)")
    args = ap.parse_args()
    if not LAB.exists():
        sys.exit("FAIL: ./celup_lab not built")
    fixtures = {
        "step48": (SRC / "step48_src.webp", 4, ["-r", "1.5"]),
        "cornerstar48": (SRC / "cornerstar48_src.webp", 4, ["-r", "2.3", "-g", "16"]),
        "huearc48": (SRC / "huearc48_src.webp", 4, ["-r", "1.5"]),
        "rampnoise48": (SRC / "rampnoise48_src.webp", 4, ["-r", "1.5"]),
        "twoline48": (SRC / "twoline48_src.webp", 4, ["-r", "1.5"]),
        "caps48": (SRC / "caps48_src.webp", 4, ["-r", "1.5"]),
    }
    diag_src = SRC / "diagline48_src.webp"
    fp = {"_meta": "autodeblur regression fingerprint"}
    lines = ["%-14s %10s %10s %10s" % ("fixture", "mae_base", "sharp_rt", "hull_viol")]
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for name, (src, scale, extra) in fixtures.items():
            if not src.exists():
                lines.append("%-14s (missing %s)" % (name, src.name))
                continue
            dec = run(td / "d.webp", src, scale, RECIPE + extra)
            base = run(td / "b.webp", src, scale, BASE + extra)
            m = {"mae_vs_base": mae(dec, base),
                 "sharp_ratio": sharp_ratio(dec, base),
                 "hull_viol": hull_violations(dec, src, scale)}
            fp[name] = m
            lines.append("%-14s %10.5f %10.3f %10d" %
                         (name, m["mae_vs_base"], m["sharp_ratio"], m["hull_viol"]))
        if diag_src.exists():
            d4 = run(td / "diag4.webp", diag_src, 4, RECIPE + ["-r", "2.3", "-g", "16"])
            d4p = pm(d4)
            fp["diagline48"] = {"jump95": jump95_diag(td / "diag4.webp"),
                                "hue_sat": hue_sat(pm(run(td / "dh.webp", SRC / "huearc48_src.webp", 4, RECIPE + ["-r", "1.5"])))}
            fp["diagline48"]["hue_sat"] = None
            lines.append("%-14s %10s %10s %10s  jump95=%.3f" %
                         ("diagline48(2.3,g16)", "-", "-", "-", fp["diagline48"]["jump95"]))
            # hue-arc saturation retention
            hb = hue_sat(pm(run(td / "hb.webp", SRC / "huearc48_src.webp", 4, BASE + ["-r", "1.5"])))
            hd = hue_sat(pm(run(td / "hd.webp", SRC / "huearc48_src.webp", 4, RECIPE + ["-r", "1.5"])))
            fp["huearc48"]["hue_sat_base"] = hb
            fp["huearc48"]["hue_sat_deblur"] = hd
            fp["huearc48"]["hue_keep"] = hd / max(hb, 1e-6)
            lines.append("huearc48 keep = %.3f (deblur %.4f / base %.4f)" %
                         (fp["huearc48"]["hue_keep"], hd, hb))
            fp["rampnoise48"]["hf_noise"] = hf_noise(pm(run(td / "rn.webp", SRC / "rampnoise48_src.webp", 4, RECIPE + ["-r", "1.5"])))
            lines.append("rampnoise48 hf_noise = %.5f" % fp["rampnoise48"]["hf_noise"])
    out = "\n".join(lines) + "\n"
    print(out)
    Path(args.write).write_text(out + "\n" + json.dumps(fp, indent=1) + "\n")
    if args.ref and Path(args.ref).exists():
        ref = json.loads(Path(args.ref).read_text())
        print("--- diff vs %s ---" % args.ref)
        for k in fp:
            if k == "_meta" or k not in ref:
                continue
            print("%-14s %s -> %s" % (k, ref.get(k), fp[k]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
