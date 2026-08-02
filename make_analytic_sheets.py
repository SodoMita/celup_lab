#!/usr/bin/env python3
"""WebP comparison sheets for the opt-in `-D analytic` autodeblur deblur method.

Matches the style other arena/* branches ship: lossless WebP mosaics in
comparison_sheets/, one column per variant with a text label strip.

Sheets:
  sheet_analytic_methods_<img>.webp  analytic vs base/remap/push on real art
  sheet_analytic_ksweep_<fix>.webp   analytic -g sweep (inverted semantics:
                                     K=1 quantize -> large K ~ identity)
  sheet_analytic_corners.webp        analytic vs remap on the corner/glow
                                     forensics fixture (hull/tip/width/glow)

Run from the repo root:  python3 make_analytic_sheets.py
Requires the ./celup_lab binary (built) and pillow/numpy.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
LAB = HERE / "celup_lab"
OUT = HERE / "comparison_sheets"
IMG = HERE / "images"
FIX = HERE / "tests"
OUT.mkdir(exist_ok=True)

# shared recipe fragments (the autoblur/autodeblur family shares -k/-c/-r/-s)
ART = ["-c", "linear", "-k", "bspline", "-M", "4096"]


def run(src, out, scale, mode, extra):
    cmd = [str(LAB), str(src), str(out), str(scale), "--mode", mode] + list(extra)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr.strip().splitlines()[-1] if r.stderr.strip() else "error")
    img = np.asarray(Image.open(out).convert("RGBA"))
    # last stderr line is usually the "Done:" / method report
    info = ""
    for ln in reversed(r.stderr.strip().splitlines()):
        if "method=" in ln or "analytic K=" in ln or "selected" in ln:
            info = ln
            break
    return img, info


def synth_blurred_step(path, w=96, h=96, sigma=2.4):
    """A clean horizontal blurred edge (sRGB, opaque) for the K-sweep."""
    z = (np.arange(w) - (w / 2 - 0.5)) / (sigma * 1.6)
    prof = np.clip(0.5 * (1 + np.tanh(z)), 0, 1) ** (1 / 2.2)
    img = np.zeros((h, w, 4), np.uint8)
    for c in range(3):
        img[:, :, c] = (prof * 255).astype(np.uint8)[None, :]
    img[:, :, 3] = 255
    Image.fromarray(img, "RGBA").save(path, lossless=True)


def font(size):
    try:
        return ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)
    except Exception:
        return ImageFont.load_default()


def text_size(d, s, fnt):
    b = d.textbbox((0, 0), s, font=fnt)
    return b[2] - b[0], b[3] - b[1]


def make_sheet(name, cells, labels, infos=None):
    """cells: list of HxWx4 uint8 arrays (or None).  labels: list[str]."""
    infos = infos or [""] * len(cells)
    anyc = next((c for c in cells if c is not None), None)
    if anyc is None:
        print(f"SKIP {name}: no successful cells")
        return None
    # Display cap: downscale cells (lanczos) so the mosaic never exceeds the
    # WebP 16383px limit and stays a readable contact sheet.
    CELL_MAX_W = 1400
    H0, W0 = anyc.shape[0], anyc.shape[1]
    disp_w = min(W0, CELL_MAX_W)
    disp_h = max(1, int(round(H0 * disp_w / W0)))
    H, W = disp_h, disp_w
    label_h = 34
    gap = 6
    bg = (235, 235, 238, 255)
    fnt = font(15)
    fntb = font(16)

    total_w = W * len(cells) + gap * (len(cells) - 1)
    total_h = label_h + H
    mosaic = np.full((total_h, total_w, 4), 0, np.uint8)
    mosaic[..., 0] = bg[0]
    mosaic[..., 1] = bg[1]
    mosaic[..., 2] = bg[2]
    mosaic[..., 3] = 255
    sheet = Image.fromarray(mosaic, "RGBA")
    d = ImageDraw.Draw(sheet)

    x = 0
    for cell, lbl, info in zip(cells, labels, infos):
        # label strip
        d.rectangle([x, 0, x + W, label_h], fill=(28, 28, 32, 255))
        d.text((x + 6, 3), lbl, font=fntb, fill=(255, 255, 255, 255))
        if info:
            d.text((x + 6, 20), info, font=fnt, fill=(170, 200, 255, 255))
        if cell is not None:
            ci = Image.fromarray(cell, "RGBA")
            if ci.size != (W, H):
                ci = ci.resize((W, H), Image.LANCZOS)
            sheet.paste(ci, (x, label_h))
        x += W + gap

    path = OUT / f"{name}.webp"
    sheet.save(path, lossless=True)
    print(f"  wrote {path}  ({sheet.size[0]}x{sheet.size[1]})")
    return path


def methods_sheet(name, src, scale, specs):
    print(f"\n=== {name} ({Path(src).name} @ {scale}x) ===")
    cells, labels, infos = [], [], []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for label, mode, extra in specs:
            out = td / f"{abs(hash(label))}.webp"
            img, info = run(src, out, scale, mode, extra)
            cells.append(img)
            labels.append(label)
            infos.append(info if img is not None else (info or "FAIL"))
            print(f"  {'ok ' if img is not None else 'FAIL'} {label}")
    return make_sheet(name, cells, labels, infos)


def main():
    if not LAB.exists():
        sys.exit("FAIL: ./celup_lab not built")
    smiley = IMG / "poor smiley.webp"
    cat = IMG / "cat.webp"
    fem = IMG / "femlineart.webp"
    cornerstar = FIX / "cornerstar48_src.webp"
    step = FIX / "step48_src.webp"

    # ---- 1. analytic vs base / remap / push on real art ----
    if smiley.exists():
        # user-style smiley recipe: -r 6 (heavy blur hides stairs). analytic -g
        # is inverted: 1.3 strong, 1.6 moderate.
        specs = [
            ("bilinear", "bilinear", ART),
            ("autoblur -r6", "autoblur", ART + ["-r", "6"]),
            ("adeblur remap", "autodeblur",
             ART + ["-r", "6", "-s", "100", "-g", "16", "-D", "remap"]),
            ("adeblur push", "autodeblur",
             ART + ["-r", "6", "-s", "100", "-g", "16", "-D", "push"]),
            ("adeblur analytic g1.3", "autodeblur",
             ART + ["-r", "6", "-s", "100", "-g", "1.3", "-D", "analytic"]),
            ("adeblur analytic g1.6", "autodeblur",
             ART + ["-r", "6", "-s", "100", "-g", "1.6", "-D", "analytic"]),
        ]
        methods_sheet("sheet_analytic_methods_smiley", smiley, 2, specs)

    if cat.exists():
        specs = [
            ("bilinear", "bilinear", ART),
            ("autoblur", "autoblur", ART + ["-r", "1.5"]),
            ("adeblur remap", "autodeblur",
             ART + ["-r", "1.5", "-s", "100", "-g", "8", "-D", "remap"]),
            ("adeblur analytic g2", "autodeblur",
             ART + ["-r", "1.5", "-s", "100", "-g", "2", "-D", "analytic"]),
            ("adeblur analytic g3", "autodeblur",
             ART + ["-r", "1.5", "-s", "100", "-g", "3", "-D", "analytic"]),
        ]
        methods_sheet("sheet_analytic_methods_cat", cat, 4, specs)

    if fem.exists():
        specs = [
            ("bilinear", "bilinear", ART),
            ("autoblur", "autoblur", ART + ["-r", "1.5"]),
            ("adeblur remap", "autodeblur",
             ART + ["-r", "1.5", "-s", "100", "-g", "8", "-D", "remap"]),
            ("adeblur analytic g2", "autodeblur",
             ART + ["-r", "1.5", "-s", "100", "-g", "2", "-D", "analytic"]),
        ]
        methods_sheet("sheet_analytic_methods_femlineart", fem, 2, specs)

    # ---- 2. analytic -g sweep (inverted semantics) ----
    # clean synthetic blurred step: K=1 quantize -> large K identity
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        clean = td / "cleanstep.webp"
        synth_blurred_step(clean)
        specs = [("autoblur base", "autoblur", ART + ["-r", "2.3"])]
        for K in ("1.0", "1.3", "1.6", "2.0", "3.0", "6.0", "16.0"):
            specs.append((f"analytic K={K}", "autodeblur",
                          ART + ["-r", "2.3", "-s", "100", "-g", K, "-D", "analytic"]))
        methods_sheet("sheet_analytic_ksweep_step", clean, 4, specs)

    if cornerstar.exists():
        # the corner/glow forensics fixture: analytic (0 hull viol) vs remap
        specs = [
            ("autoblur base", "autoblur", ART + ["-r", "2.3"]),
            ("adeblur remap g16", "autodeblur",
             ART + ["-r", "2.3", "-s", "100", "-g", "16", "-D", "remap"]),
            ("analytic g1.3", "autodeblur",
             ART + ["-r", "2.3", "-s", "100", "-g", "1.3", "-D", "analytic"]),
            ("analytic g1.6", "autodeblur",
             ART + ["-r", "2.3", "-s", "100", "-g", "1.6", "-D", "analytic"]),
            ("analytic g1.0(quant)", "autodeblur",
             ART + ["-r", "2.3", "-s", "100", "-g", "1.0", "-D", "analytic"]),
        ]
        methods_sheet("sheet_analytic_corners", cornerstar, 4, specs)

    print("\nAll sheets written to", OUT)


if __name__ == "__main__":
    main()
