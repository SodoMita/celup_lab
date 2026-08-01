#!/usr/bin/env python3
"""Comparison-sheet generator: MINE (arena/019fbef6, master v4.9.2 core) vs
arena/019fba18 (v4.9.8: mass-conserving depth + erf-gain post-map), plus the
key finding -- the grey wash is the -r6 recipe, not the deblur.

Writes two lossless WebP sheets to images/:
  sheet_artifacts.webp : torture scenes [bilinear|MINE|v4.9.8] -- v4.9.8 ringing
  sheet_bw_art.webp    : BW/grey RECIPE SWEEP [nearest|MINE default|MINE -r1.5|
                         MINE -r6|v4.9.8 -r6] -- default/-r1.5 = clean sharp ink

Usage: python3 make_vs_18_sheets.py [MY_EXE] [V18_EXE]
"""
import subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
MY = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "celup_lab"
V18 = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "celup_18"
OUT = ROOT / "images"
OUT.mkdir(exist_ok=True)
S, N = 4, 96


def lin(x):
    x = x / 255.
    return np.where(x <= .04045, x / 12.92, ((x + .055) / 1.055) ** 2.4)


def pm(a):
    a = np.asarray(a, dtype=np.float32)
    o = np.empty_like(a)
    o[..., :3] = lin(a[..., :3]) * (a[..., 3:4] / 255.)
    o[..., 3] = a[..., 3] / 255.
    return o


def encode_pm(p):
    a = np.clip(p[..., 3:4], 0, 1)
    rgb = np.clip(np.divide(p[..., :3], np.maximum(a, 1e-8)), 0, 1)
    sr = np.where(rgb <= .0031308, 12.92 * rgb, 1.055 * rgb ** (1 / 2.4) - .055)
    return Image.fromarray(np.dstack((np.clip(sr * 255, 0, 255),
                                      a[..., 0] * 255)).astype(np.uint8), 'RGBA')


def down(a):
    return a.reshape(N, S, N, S, 4).mean((1, 3))


def scene(name):
    z = N * S
    yy, xx = np.mgrid[0:z, 0:z]
    if name == 'rings':
        r = np.sqrt((xx - z / 2) ** 2 + (yy - z / 2) ** 2)
        a = np.zeros((z, z, 4), np.uint8); a[:] = (24, 30, 44, 255)
        a[(r % (6 * S)) < (2 * S)] = (245, 200, 70, 255); return pm(a)
    if name == 'corner':
        im = Image.new('RGBA', (z, z), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
        d.polygon([(z//8, z//8), (7*z//8, z//8), (7*z//8, 7*z//8),
                   (z//2, 5*z//8), (z//8, 7*z//8)], fill=(180, 60, 180, 255))
        d.line([(z//8, z//8), (7*z//8, z//8), (7*z//8, 7*z//8),
                (z//2, 5*z//8), (z//8, 7*z//8), (z//8, z//8)],
               fill=(255, 255, 255, 255), width=S)
        return pm(np.asarray(im))
    if name == 'crosshatch':
        a = np.zeros((z, z, 4), np.uint8); a[:] = (250, 250, 252, 255)
        xh = (((xx - yy) % (8 * S)) < S) | (((xx + yy) % (8 * S)) < S)
        a[xh] = (25, 25, 130, 255); return pm(a)
    if name == 'diag':
        im = Image.new('RGBA', (z, z), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
        d.polygon([(0, z), (0, 3*z//4), (z, z//4), (z, z//2)], fill=(20, 210, 255, 255))
        d.line((0, 3*z//4, z, z//4), fill='white', width=2*S)
        return pm(np.asarray(im))
    raise SystemExit(name)


def run(exe, src, scale, recipe):
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "o.webp"
        subprocess.run([str(exe), str(src), str(out), str(scale), *recipe],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return np.asarray(Image.open(out).convert("RGB"), dtype=np.uint8)


def bilinear_up(low_rgb, scale):
    return np.asarray(Image.fromarray(low_rgb).resize(
        (low_rgb.shape[1]*scale, low_rgb.shape[0]*scale), Image.BILINEAR))


def fonts():
    try:
        ft = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 18)
        fc = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 12)
    except Exception:
        ft = fc = ImageFont.load_default()
    return ft, fc


def grid(title, col_labels, rows, cell=256, title_h=44):
    ncol = len(col_labels)
    W = ncol * cell
    H = title_h + sum(cell + 30 for _ in rows)
    canvas = Image.new("RGB", (W, H), (8, 8, 10))
    ft, fc = fonts()
    ImageDraw.Draw(canvas).rectangle([0, 0, W, title_h], fill=(12, 12, 16))
    ImageDraw.Draw(canvas).text((8, 4), title, (255, 255, 255), font=ft)
    for i, c in enumerate(col_labels):
        ImageDraw.Draw(canvas).text((i * cell + 6, 24), c, (200, 230, 255), font=fc)
    y = title_h
    for rtitle, rsub, imgs in rows:
        ImageDraw.Draw(canvas).rectangle([0, y, W, y + 26], fill=(24, 24, 30))
        ImageDraw.Draw(canvas).text((8, y + 5),
                                    rtitle + ("   " + rsub if rsub else ""),
                                    (255, 230, 180), font=fc)
        y += 26
        for i, im in enumerate(imgs):
            t = Image.fromarray(im).resize((cell, cell), Image.NEAREST)
            canvas.paste(t, (i * cell, y))
        y += cell + 4
    return canvas


def main():
    ADB = ["--mode", "autodeblur", "--max-mib", "2048", "-c", "linear",
           "-k", "bspline", "-s", "100", "-D", "remap"]
    # ---- sheet 1: artifacts (torture) ----
    rows = []
    HG = {"rings": "MINE .0037 vs v4.9.8 .0155", "corner": "MINE .0020 vs v4.9.8 .0056",
          "crosshatch": "MINE .0057 vs v4.9.8 .0126", "diag": "MINE .0017 vs v4.9.8 .0124"}
    for name in ("rings", "corner", "crosshatch", "diag"):
        with tempfile.TemporaryDirectory() as td:
            inp = Path(td) / "s.webp"; encode_pm(down(scene(name))).save(inp, lossless=True)
            low_rgb = np.asarray(encode_pm(down(scene(name))).convert("RGB"))
            bil = bilinear_up(low_rgb, S)
            mine = run(MY, inp, S, ADB + ["-r", "1.5"])
            v18 = run(V18, inp, S, ADB + ["-r", "1.5"])
        rows.append((name, "HG " + HG[name], [bil, mine, v18]))
    g1 = grid("ARTIFACTS 4x: v4.9.8 adds severe hourglass/ringing on edges & curves (HG labels)",
              ["bilinear ref", "MINE (v4.9.2 core)", "019fba18 (v4.9.8)"], rows, cell=256)
    g1.save(OUT / "sheet_artifacts.webp", lossless=True)
    print("wrote", OUT / "sheet_artifacts.webp")

    # ---- sheet 2: BW / grey RECIPE SWEEP (the finding) ----
    rows = []
    sm = ROOT / "images" / "poor smiley.webp"
    if sm.exists():
        near = run(MY, sm, 2, ["--mode", "nearest", "--max-mib", "2048"])
        d0 = run(MY, sm, 2, ADB)
        d15 = run(MY, sm, 2, ADB + ["-r", "1.5", "-g", "8"])
        d6 = run(MY, sm, 2, ADB + ["-r", "6", "-g", "64"])
        v6 = run(V18, sm, 2, ADB + ["-r", "6", "-g", "64"])
        rows.append(("poor smiley 2x FULL",
                     "grey/j95: default 3.6%/.45 | -r1.5 11%/.01 | -r6 30%/.21",
                     [near, d0, d15, d6, v6]))
        c = (40, 320, 60, 470)
        rows.append(("smiley detail crop (spike/mouth)",
                     "default & -r1.5 = clean sharp ink;  -r6 = washed grey",
                     [near[c[0]:c[1], c[2]:c[3]], d0[c[0]:c[1], c[2]:c[3]],
                      d15[c[0]:c[1], c[2]:c[3]], d6[c[0]:c[1], c[2]:c[3]],
                      v6[c[0]:c[1], c[2]:c[3]]]))
    W, H = 128, 128; im = Image.new("L", (W, H), 255); d = ImageDraw.Draw(im)
    d.rectangle([10, 10, 118, 118], outline=0, width=3)
    d.line([10, 10, 118, 118], fill=0, width=3); d.line([118, 10, 10, 118], fill=0, width=3)
    d.ellipse([40, 40, 88, 88], outline=0, width=3); d.line([20, 64, 108, 64], fill=0, width=2)
    bw = ROOT / "tests" / "bw_test_src.webp"
    Image.merge("RGBA", [im, im, im, Image.new("L", (W, H), 255)]).save(bw, lossless=True)
    near = run(MY, bw, 4, ["--mode", "nearest", "--max-mib", "2048"])
    d0 = run(MY, bw, 4, ADB)
    d15 = run(MY, bw, 4, ADB + ["-r", "1.5", "-g", "8"])
    d6 = run(MY, bw, 4, ADB + ["-r", "6", "-g", "64"])
    v6 = run(V18, bw, 4, ADB + ["-r", "6", "-g", "64"])
    rows.append(("pure BW test 4x",
                 "default & -r1.5 keep crisp black ink;  -r6 washes it",
                 [near, d0, d15, d6, v6]))
    g2 = grid("BW/GREY RECIPE SWEEP: MINE returns clean sharp ink at default/-r1.5; -r6 over-blurs; v4.9.8 -r6 recovers ink but rings",
              ["nearest ref", "MINE default", "MINE -r1.5 -g8",
               "MINE -r6 -g64", "v4.9.8 -r6 -g64"], rows, cell=360)
    g2.save(OUT / "sheet_bw_art.webp", lossless=True)
    print("wrote", OUT / "sheet_bw_art.webp")


if __name__ == "__main__":
    main()
