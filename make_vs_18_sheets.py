#!/usr/bin/env python3
"""Comparison-sheet generator: MY autodeblur (branch arena/019fbef6, master
v4.9.2 core) vs arena/019fba18 (v4.9.8: mass-conserving depth + erf-gain
post-map).  Produces two labeled PNG grids so the artifact vs grey/ink
trade-off is visible at a glance.

  sheet_artifacts.png : torture scenes (rings/corner/crosshatch/diag) x
                        [bilinear | MINE | v4.9.8]  -- shows v4.9.8's added
                        hourglass/ringing energy on edges and curves.
  sheet_bw_art.png    : the user's BW/grey content (smiley user recipe, pure-BW
                        test, miya user recipe) x [nearest | MINE | v4.9.8] --
                        shows v4.9.8 recovering ink (less grey) while MINE
                        stays cleaner.

Usage: python3 make_vs_18_sheets.py [MY_EXE] [V18_EXE]
"""
import os, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent
MY = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "celup_lab"
V18 = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "celup_18"
OUT = ROOT / "docs"
OUT.mkdir(exist_ok=True)
S, N = 4, 96  # torture: 4x, 96-cell truth


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
        td = Path(td); out = td / "o.webp"
        subprocess.run([str(exe), str(src), str(out), str(scale), *recipe],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return np.asarray(Image.open(out).convert("RGB"), dtype=np.uint8)


def bilinear_up(low_rgb, scale):
    return np.asarray(Image.fromarray(low_rgb).resize(
        (low_rgb.shape[1]*scale, low_rgb.shape[0]*scale), Image.BILINEAR))


def label(img, text, sub=""):
    w, h = img.size
    bar = Image.new("RGB", (w, 26 + (14 if sub else 0)), (20, 20, 24))
    try:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 14)
        fs = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
    except Exception:
        f = fs = ImageFont.load_default()
    ImageDraw.Draw(bar).text((6, 3), text, (255, 255, 255), font=f)
    if sub:
        ImageDraw.Draw(bar).text((6, 19), sub, (170, 200, 255), font=fs)
    return Image.new("RGB", (w, bar.size[1] + h))
    # (compose handled by caller)


def stack_label(text, sub, img_arr):
    img = Image.fromarray(img_arr)
    w, h = img.size
    bh = 28 + (14 if sub else 0)
    bar = Image.new("RGB", (w, bh), (20, 20, 24))
    try:
        f = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 14)
        fs = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11)
    except Exception:
        f = fs = ImageFont.load_default()
    ImageDraw.Draw(bar).text((6, 3), text, (255, 255, 255), font=f)
    if sub:
        ImageDraw.Draw(bar).text((6, 20), sub, (180, 210, 255), font=fs)
    canvas = Image.new("RGB", (w, bh + h))
    canvas.paste(bar, (0, 0)); canvas.paste(img, (0, bh))
    return canvas


def grid(title, col_labels, rows, cell=256, title_h=40):
    ncol = len(col_labels)
    # each row: [title, sub, [imgs...]]
    nrow = len(rows)
    W = ncol * cell
    H = title_h + sum(cell + 42 for _ in rows)
    canvas = Image.new("RGB", (W, H), (8, 8, 10))
    try:
        ft = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 20)
        fc = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 13)
    except Exception:
        ft = fc = ImageFont.load_default()
    ImageDraw.Draw(canvas).text((10, 8), title, (255, 255, 255), font=ft)
    y = title_h
    for rtitle, rsub, imgs in rows:
        cw = cell
        # row label band across the row top
        ImageDraw.Draw(canvas).rectangle([0, y, W, y + 28], fill=(24, 24, 30))
        ImageDraw.Draw(canvas).text((10, y + 6), rtitle + ("   " + rsub if rsub else ""),
                                    (255, 230, 180), font=fc)
        y += 28
        # resize each img to cell
        for i, im in enumerate(imgs):
            t = Image.fromarray(im).resize((cell, cell), Image.NEAREST)
            canvas.paste(t, (i * cell, y))
        y += cell + 14
    # column header band
    ImageDraw.Draw(canvas).rectangle([0, 0, W, title_h], fill=(12, 12, 16))
    ImageDraw.Draw(canvas).text((10, 8), title, (255, 255, 255), font=ft)
    for i, c in enumerate(col_labels):
        ImageDraw.Draw(canvas).text((i * cell + 8, 0), c, (255, 255, 255), font=ft)
    return canvas


def main():
    ADB = ["--mode", "autodeblur", "--max-mib", "2048", "-c", "linear",
           "-k", "bspline", "-s", "100", "-D", "remap"]
    # ---- sheet 1: artifacts (torture) ----
    rows = []
    HG = {"rings": ("MINE .0037", "v4.9.8 .0155"), "corner": ("MINE .0020", "v4.9.8 .0056"),
          "crosshatch": ("MINE .0057", "v4.9.8 .0126"), "diag": ("MINE .0017", "v4.9.8 .0124")}
    for name in ("rings", "corner", "crosshatch", "diag"):
        truth = scene(name)
        low_rgb = (encode_pm(down(truth)).convert("RGB"))
        low_rgb = np.asarray(low_rgb)
        with tempfile.TemporaryDirectory() as td:
            inp = Path(td) / "s.webp"; encode_pm(down(truth)).save(inp, lossless=True)
            bil = bilinear_up(low_rgb, S)
            mine = run(MY, inp, S, ADB + ["-r", "1.5"])
            v18 = run(V18, inp, S, ADB + ["-r", "1.5"])
        rows.append((name, "HG " + HG[name][0] + "  vs  " + HG[name][1], [bil, mine, v18]))
    g1 = grid("ARTIFACTS: torture scenes 4x (hourglass energy -- v4.9.8 adds severe ringing on edges/curves)",
              ["bilinear ref", "MINE (master v4.9.2 core)", "019fba18 (v4.9.8)"], rows, cell=256)
    g1.save(OUT / "sheet_artifacts.png")
    print("wrote", OUT / "sheet_artifacts.png")

    # ---- sheet 2: BW / grey test (art) ----
    rows = []
    # smiley user recipe
    sm = ROOT / "images" / "poor smiley.webp"
    if sm.exists():
        mine = run(MY, sm, 2, ADB + ["-r", "6", "-g", "64"])
        v18 = run(V18, sm, 2, ADB + ["-r", "6", "-g", "64"])
        near = run(MY, sm, 2, ["--mode", "nearest", "--max-mib", "2048"])
        rows.append(("poor smiley 2x -r6 -g64", "grey: MINE 30.9%  vs  v4.9.8 8.0%",
                     [near, mine, v18]))
    # pure BW test
    W, H = 128, 128; im = Image.new("L", (W, H), 255); d = ImageDraw.Draw(im)
    d.rectangle([10, 10, 118, 118], outline=0, width=3)
    d.line([10, 10, 118, 118], fill=0, width=3); d.line([118, 10, 10, 118], fill=0, width=3)
    d.ellipse([40, 40, 88, 88], outline=0, width=3); d.line([20, 64, 108, 64], fill=0, width=2)
    bw = ROOT / "tests" / "bw_test_src.webp"
    Image.merge("RGBA", [im, im, im, Image.new("L", (W, H), 255)]).save(bw, lossless=True)
    mine = run(MY, bw, 4, ADB + ["-r", "6", "-g", "64"])
    v18 = run(V18, bw, 4, ADB + ["-r", "6", "-g", "64"])
    near = run(MY, bw, 4, ["--mode", "nearest", "--max-mib", "2048"])
    rows.append(("pure BW test 4x -r6 -g64", "ink<128: MINE 0.0%  vs  v4.9.8 17.7%",
                 [near, mine, v18]))
    # miya user recipe (crop face)
    mi = ROOT / "images" / "miya_normal.webp"
    if mi.exists():
        mine = run(MY, mi, 4, ADB + ["-r", "2.3", "-g", "8", "--max-mib", "4096"])
        v18 = run(V18, mi, 4, ADB + ["-r", "2.3", "-g", "8", "--max-mib", "4096"])
        near = run(MY, mi, 4, ["--mode", "nearest", "--max-mib", "4096"])
        rows.append(("miya 4x -r2.3 -g8 (full)", "", [near, mine, v18]))
    g2 = grid("BW / GREY TEST: ink recovery vs cleanness (v4.9.8 recovers ink, MINE stays clean)",
              ["nearest ref", "MINE (master v4.9.2 core)", "019fba18 (v4.9.8)"], rows, cell=300)
    g2.save(OUT / "sheet_bw_art.png")
    print("wrote", OUT / "sheet_bw_art.png")


if __name__ == "__main__":
    main()
