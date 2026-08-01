#!/usr/bin/env python3
"""Generate a comparison sheet: grid of upscaled results for each input image
and each mode, all at the same scale.  Outputs a single WebP mosaic."""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

LAB = Path(__file__).resolve().parent / "celup_lab"
OUT = Path(__file__).resolve().parent / "comparison_sheets"
OUT.mkdir(exist_ok=True)

SCALE = 2
# User's recipes
RECIPES = {
    "smiley-user": ["-r", "6", "-s", "100", "-g", "64", "-D", "remap", "-c", "linear", "-k", "bspline"],
    "miya-user":   ["-r", "2.3", "-s", "100", "-g", "16", "-D", "remap", "-c", "linear", "-k", "bspline"],
}
MODES = ["autodeblur", "autoblur", "cubic", "mitchell", "lanczos3", "adaptive", "nearest"]

INPUTS = {
    "poor_smiley": Path("tests/poor_smiley.webp"),
    "miya_facehalf": Path("tests/miya_facehalf.webp"),
}

def run_one(src, out, scale, extra_args):
    cmd = [str(LAB), str(src), str(out), str(scale),
           "--mode", "autodeblur", "--max-mib", "2048", *extra_args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  FAIL: {r.stderr.strip()[:200]}")
        return False
    return True

def main():
    images = []  # list of (label, np.array HxWx4)
    labels = []
    
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for name, src in INPUTS.items():
            if not src.exists():
                print(f"skip {name}: source missing")
                continue
            # Reference: bilinear baseline
            ref_out = td / f"{name}_bilinear.webp"
            cmd = [str(LAB), str(src), str(ref_out), str(SCALE),
                   "--mode", "bilinear", "--max-mib", "2048"]
            subprocess.run(cmd, capture_output=True)
            ref = np.asarray(Image.open(ref_out).convert("RGBA"))
            images.append(ref)
            labels.append(f"{name} / bilinear (ref)")
            
            for recipe_name, recipe_args in RECIPES.items():
                for mode in MODES:
                    out = td / f"{name}_{recipe_name}_{mode}.webp"
                    ok = run_one(src, out, SCALE, ["--mode", mode, *recipe_args])
                    if ok:
                        img = np.asarray(Image.open(out).convert("RGBA"))
                        images.append(img)
                        labels.append(f"{name} / {recipe_name} / {mode}")
                    else:
                        # Use reference as placeholder
                        images.append(ref)
                        labels.append(f"{name} / {recipe_name} / {mode} [FAIL]")

    # Build mosaic: rows = inputs, cols = modes*recipes + ref
    n_per_input = 1 + len(RECIPES) * len(MODES)  # ref + recipe*mode
    n_inputs = len(INPUTS)
    
    # Find common height (all 2x of same source -> same size)
    H = images[0].shape[0]
    W = images[0].shape[1]
    
    # Layout: each input gets a row of n_per_input images
    mosaic_h = H * n_inputs + 20 * (n_inputs - 1) + 40 * n_inputs  # + label rows
    mosaic_w = W * n_per_input + 10 * (n_per_input - 1)
    
    mosaic = np.full((mosaic_h, mosaic_w, 4), 200, dtype=np.uint8)
    
    for i, name in enumerate(INPUTS.keys()):
        row_start = i * (H + 40 + 20) + 40
        col = 0
        for j, (img, lbl) in enumerate(zip(images, labels)):
            if not lbl.startswith(name):
                continue
            # Draw label above
            # (simplified: just place image)
            h, w = img.shape[:2]
            mosaic[row_start:row_start+h, col:col+w] = img[:H, :W]
            col += W + 10
    
    # Simpler: just concatenate horizontally per input, stack vertically
    rows = []
    idx = 0
    for name in INPUTS.keys():
        row_imgs = []
        for j, (img, lbl) in enumerate(zip(images, labels)):
            if lbl.startswith(name + " /"):
                row_imgs.append(img[:H, :W])
        if row_imgs:
            rows.append(np.concatenate(row_imgs, axis=1))
    
    if rows:
        # Pad rows to same width
        max_w = max(r.shape[1] for r in rows)
        padded = []
        for r in rows:
            if r.shape[1] < max_w:
                pad = np.full((r.shape[0], max_w - r.shape[1], 4), 200, dtype=np.uint8)
                r = np.concatenate([r, pad], axis=1)
            padded.append(r)
        mosaic = np.concatenate(padded, axis=0)
        Image.fromarray(mosaic, "RGBA").save(OUT / "sheet_v493.webp", lossless=True)
        print(f"wrote {OUT / 'sheet_v493.webp'} ({mosaic.shape[1]}x{mosaic.shape[0]})")
        
        # Also save per-input sheets
        for i, name in enumerate(INPUTS.keys()):
            row_imgs = []
            for j, (img, lbl) in enumerate(zip(images, labels)):
                if lbl.startswith(name + " /"):
                    row_imgs.append(img[:H, :W])
            if row_imgs:
                sheet = np.concatenate(row_imgs, axis=0)
                Image.fromarray(sheet, "RGBA").save(OUT / f"sheet_{name}_v493.webp", lossless=True)
                print(f"wrote {OUT / f'sheet_{name}_v493.webp'}")

if __name__ == "__main__":
    main()
