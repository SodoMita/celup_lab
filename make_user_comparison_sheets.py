#!/usr/bin/env python3
"""Comparison sheets for the four real user test images.

Each sheet stacks the upscaled result of every mode in one row.  The four
inputs are the user's own assets from images/:

  miya face   miya_normal.webp cropped to the (320x300) face region
              (the same crop tests/make_miya_fixtures.py uses)
  smiley      "poor smiley.webp" (256x256 hard pixelated face)
  cat         cat.webp (400x400)
  pikachu     pikachu.webp (474x831 cartoon)  -- the "1 other image"

For each image the low-res source is upscaled by celup_lab at the chosen
scale with every listed mode; the first row is the nearest (xN) source
reference.  A combined sheet tiles all four.  Everything is written to
comparison_sheets/ (gitignored) as lossless WebP.

Usage: python3 make_user_comparison_sheets.py
"""
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
EXE = ROOT / "celup_lab"
SHEET_DIR = ROOT / "comparison_sheets"
SHEET_DIR.mkdir(exist_ok=True)

# (label, --mode, extra args)  -- bilinear first as the soft reference.
MODES = (
    ("nearest",    "nearest",    ()),
    ("bilinear",   "bilinear",   ()),
    ("cubic",      "cubic",      ()),
    ("mitchell",   "mitchell",   ()),
    ("lanczos3",   "lanczos3",   ()),
    ("jinc2_bil",  "jinc2_bilateral", ()),
    ("superxbr",   "superxbr",   ()),
    ("adaptive",   "adaptive",   ()),
    ("autodeblur", "autodeblur", ()),
    ("sdf",        "sdf",        ()),
)
MODE_LABELS = [m[0] for m in MODES]

# image key -> (path, crop box or None, scale)
IMAGES = {
    "miya_face": ("images/miya_normal.webp", (240, 280, 560, 580), 2),
    "smiley":    ("images/poor smiley.webp", None,                 4),
    "cat":       ("images/cat.webp",         None,                 2),
    "pikachu":   ("images/pikachu.webp",     None,                 2),
}


def checker_composite(img):
    a = np.asarray(img.convert("RGBA"), dtype=np.uint16)
    h, w = a.shape[:2]
    yy, xx = np.indices((h, w))
    bg = np.where(((xx // 12 + yy // 12) & 1)[..., None],
                  (210, 210, 210), (246, 246, 246)).astype(np.uint16)
    alpha = a[:, :, 3:4]
    rgb = (a[:, :, :3] * alpha + bg * (255 - alpha) + 127) // 255
    return Image.fromarray(rgb.astype(np.uint8), "RGB")


def run_mode(src, out, scale, mode, extra):
    cmd = [str(EXE), str(src), str(out), str(scale),
           "--mode", mode, "--max-mib", "4096", *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip()[-300:] if r.returncode else ""


def load_source(path, box):
    im = Image.open(path)
    if im.mode != "RGBA":
        im = im.convert("RGBA")
    if box:
        im = im.crop(box)
    return im


def build_sheet(key, src, scale, td):
    out_imgs = []
    failures = {}
    for label, mode, extra in MODES:
        out = td / f"{key}_{label}.webp"
        ok, err = run_mode(src, out, scale, mode, extra)
        if ok:
            out_imgs.append((label, Image.open(out).convert("RGBA"), None))
        else:
            out_imgs.append((label, None, err))
    # reference = nearest upscale
    ref = out_imgs[0]
    W, H = ref[1].size
    n = len(out_imgs)
    # Layout: one row per mode, each row = [label column] + [image].  Label
    # column is ~200 px tall enough to hold the image height with a small gap.
    gap = 6
    row_h = H + gap
    col_w = 210
    sheet = Image.new("RGB", (col_w + W, row_h * n), "white")
    d = ImageDraw.Draw(sheet)
    d.rectangle((0, 0, col_w, row_h * n), fill=(248, 248, 250))
    d.rectangle((0, 0, col_w, row_h * n), outline=(150, 150, 150))
    for i, (label, img, err) in enumerate(out_imgs):
        y = row_h * i
        d.rectangle((4, y + 6, col_w - 4, y + H - 6), fill=(238, 240, 245))
        d.text((10, y + 9), label, fill="black")
        if img is None:
            d.text((10, y + 16), "FAIL", fill="red")
            continue
        sheet.paste(checker_composite(img), (col_w, y))
        d.rectangle((col_w, y, col_w + W - 1, y + H - 1), outline=(170, 170, 170))
    return sheet, failures


def main():
    if not EXE.exists():
        raise SystemExit(f"Build {EXE} first")
    panels = []
    with __import__("tempfile").TemporaryDirectory() as td:
        td = Path(td)
        for key, (path, box, scale) in IMAGES.items():
            src = load_source(ROOT / path, box)
            # Save the working source (post-crop) so sheets are reproducible.
            src_path = td / f"{key}_src.webp"
            src.save(src_path, lossless=True)
            sheet, fails = build_sheet(key, src_path, scale, td)
            if fails:
                for lbl, err in fails.items():
                    print(f"  [{key}/{lbl}] FAIL: {err}")
            sheet_path = SHEET_DIR / f"sheet_{key}.webp"
            try:
                sheet.save(sheet_path, lossless=True)
            except ValueError:
                # Sheet exceeds WebP's 16383px dimension limit (very tall
                # sources at high scale); fall back to a large-format PNG.
                sheet_path = sheet_path.with_suffix(".png")
                sheet.save(sheet_path)
            panels.append((key, sheet))
            print(f"wrote {sheet_path} ({sheet.size[0]}x{sheet.size[1]})")

        # Combined sheet: each image's panel is a scaled-down copy stacked.
        W = 1200  # common thumbnail width across the four panels
        thumbs = []
        for key, panel in panels:
            th = panel.copy()
            th.thumbnail((W, 1200))
            thumbs.append((key, th))
        row_gap = 46
        total_h = sum(t.size[1] for _, t in thumbs) + row_gap * len(thumbs)
        combined = Image.new("RGB", (W, total_h), "white")
        d = ImageDraw.Draw(combined)
        y = 0
        for key, th in thumbs:
            d.text((6, y + 3), f"{key}  (x{IMAGES[key][2]})", fill=(20, 20, 40))
            combined.paste(th, (0, y + 22))
            y += th.size[1] + row_gap
        combined_path = SHEET_DIR / "sheet_all_four.webp"
        try:
            combined.save(combined_path, lossless=True)
        except ValueError:
            combined_path = SHEET_DIR / "sheet_all_four.png"
            combined.save(combined_path)
        print(f"wrote {combined_path} ({combined.size[0]}x{combined.size[1]})")


if __name__ == "__main__":
    sys.exit(main())
