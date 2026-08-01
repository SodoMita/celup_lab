#!/usr/bin/env python3
"""v4.9.3 comparison sheets.

Sheet 1: poor_smiley.webp upscaled with every mode (user's recipe for autodeblur).
Sheet 2: staircase stress test -- 45 deg diagonal lines, multiple modes,
         showing which algorithms re-quantize the lattice into treads.

Output: WebP mosaics in comparison_sheets/.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
LAB = HERE / "celup_lab"
OUT = HERE / "comparison_sheets"
OUT.mkdir(exist_ok=True)

SMILEY = HERE / "images" / "poor smiley.webp"
DIAG = HERE / "tests" / "diagline48_src.webp"

SCALE = 2

# User's documented recipes
SMILEY_RECIPE = ["-c", "linear", "-k", "bspline", "-r", "6",
                 "-s", "100", "-g", "64", "-D", "remap"]
MIYA_RECIPE   = ["-c", "linear", "-k", "bspline", "-r", "2.3",
                 "-s", "100", "-g", "16", "-D", "remap"]

# Modes to compare (all get the recipe appended where relevant)
MODES = [
    ("bilinear",       []),
    ("cubic",          []),
    ("mitchell",       []),
    ("lanczos3",       []),
    ("autoblur",       []),
    ("autodeblur",     SMILEY_RECIPE),
    ("adaptive",       ["-P", "auto"]),
    ("scale2x",        []),
    ("deblurcompress", ["-r", "6", "-s", "100"]),
    ("dehourglass",    []),
]


def run(src, out, scale, mode, extra):
    cmd = [str(LAB), str(src), str(out), str(scale),
           "--mode", mode, "--max-mib", "4096", *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0, r.stderr.strip().splitlines()[-1] if r.stderr.strip() else ""


def label_cell(label, w, h):
    """Render a text label as a small RGBA image."""
    img = Image.new("RGBA", (w, h), (30, 30, 30, 255))
    return np.asarray(img)


def make_sheet(name, src, modes, scale, title_extra=""):
    print(f"\n=== {name} ===")
    cells = []
    labels = []
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for mode, extra in modes:
            out = td / f"{mode}.webp"
            ok, info = run(src, out, scale, mode, extra)
            if ok:
                img = np.asarray(Image.open(out).convert("RGBA"))
                cells.append(img)
                tag = f"{mode}"
                if extra:
                    tag += " " + " ".join(extra)
                if info:
                    tag += f"  [{info[:60]}]"
                labels.append(tag)
                print(f"  ok  {tag}")
            else:
                print(f"  FAIL {mode}: {info[:120]}")
                cells.append(None)
                labels.append(f"{mode} [FAIL]")

    # Layout: one column per mode, stacked vertically with label strips
    H = cells[0].shape[0] if cells[0] is not None else 0
    W = cells[0].shape[1] if cells[0] is not None else 0
    label_h = 28
    cell_h = H + label_h
    gap = 4

    total_w = sum((c.shape[1] if c is not None else W) for c in cells) + gap * (len(cells) - 1)
    total_h = cell_h + 40  # title strip

    mosaic = np.full((total_h, total_w, 4), 220, dtype=np.uint8)

    # Title
    title = f"{name}  {title_extra}  scale={scale}x"
    # (skip text rendering -- label strips carry the info)

    x = 0
    for cell, lbl in zip(cells, labels):
        cw = cell.shape[1] if cell is not None else W
        ch = cell.shape[0] if cell is not None else H
        # Label strip (dark bg, white text approximated by solid colour bar)
        label_img = np.full((label_h, cw, 4), 40, dtype=np.uint8)
        label_img[:, :, 3] = 255
        mosaic[0:label_h, x:x+cw] = label_img
        # Cell
        if cell is not None:
            mosaic[label_h:label_h+ch, x:x+cw] = cell[:H, :W]
        x += cw + gap

    path = OUT / f"sheet_{name}.webp"
    Image.fromarray(mosaic, "RGBA").save(path, lossless=True)
    print(f"wrote {path}  ({mosaic.shape[1]}x{mosaic.shape[0]})")
    return path


def make_staircase_sheet():
    """Staircase stress test:
    - diagline48_src: 45 deg 4px line on white (procedural, known-good staircase probe)
    - poor smiley cropped to a diagonal-contour region
    Each mode at 4x to make treads maximally visible.
    """
    print("\n=== staircase sheet ===")
    scale = 4
    modes = [
        ("bilinear",       "bilinear", []),
        ("cubic",          "cubic", []),
        ("mitchell",       "mitchell", []),
        ("lanczos3",       "lanczos3", []),
        ("autoblur-r1.5",  "autoblur", ["-r", "1.5"]),
        ("autodeblur-r6",  "autodeblur", ["-c", "linear", "-k", "bspline", "-r", "6",
                            "-s", "100", "-g", "64", "-D", "remap"]),
        ("autodeblur-r2.3","autodeblur", ["-c", "linear", "-k", "bspline", "-r", "2.3",
                            "-s", "100", "-g", "16", "-D", "remap"]),
        ("autodeblur-r0.5","autodeblur", ["-c", "linear", "-k", "bspline", "-r", "0.5",
                             "-s", "100", "-g", "64", "-D", "remap"]),
        ("adaptive",       "adaptive", ["-P", "auto"]),
    ]

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        rows = []

        # Row 1: diagline48
        row1 = []
        for label, mode, extra in modes:
            out = td / f"diag_{label}.webp"
            ok, info = run(DIAG, out, scale, mode, extra)
            if ok:
                img = np.asarray(Image.open(out).convert("RGBA"))
                row1.append(img)
                print(f"  diag ok  {label}")
            else:
                print(f"  diag FAIL {label}")
        if row1:
            rows.append(("diagline48 4x", np.concatenate(row1, axis=1)))

        # Row 2: poor smiley (full image)
        row2 = []
        for label, mode, extra in modes:
            out = td / f"smiley_{label}.webp"
            ok, info = run(SMILEY, out, scale, mode, extra)
            if ok:
                img = np.asarray(Image.open(out).convert("RGBA"))
                row2.append(img)
                print(f"  smiley ok  {label}")
            else:
                print(f"  smiley FAIL {label}")
        if row2:
            rows.append(("poor smiley 4x", np.concatenate(row2, axis=1)))

        # Pad rows to same width and stack
        max_w = max(r[1].shape[1] for r in rows)
        padded = []
        for lbl, arr in rows:
            if arr.shape[1] < max_w:
                pad = np.full((arr.shape[0], max_w - arr.shape[1], 4),
                              220, dtype=np.uint8)
                arr = np.concatenate([arr, pad], axis=1)
            # prepend a label column
            label_col = np.full((arr.shape[0], 120, 4), 40, dtype=np.uint8)
            label_col[:, :, 3] = 255
            arr = np.concatenate([label_col, arr], axis=1)
            padded.append(arr)

        mosaic = np.concatenate(padded, axis=0)
        path = OUT / "sheet_staircase.webp"
        Image.fromarray(mosaic, "RGBA").save(path, lossless=True)
        print(f"wrote {path}  ({mosaic.shape[1]}x{mosaic.shape[0]})")
        return path


def main():
    if not LAB.exists():
        print("FAIL: celup_lab not built")
        return 1
    if not SMILEY.exists():
        print(f"FAIL: {SMILEY} missing (pull from master)")
        return 1

    make_sheet("poor_smiley_modes", SMILEY, MODES, SCALE,
               title_extra="user recipe: -r 6 -s 100 -g 64 -D remap -k bspline -c linear")
    make_staircase_sheet()
    return 0


if __name__ == "__main__":
    sys.exit(main())
