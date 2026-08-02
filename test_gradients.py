#!/usr/bin/env python3
"""Test gradient handling of xBR modes."""
import subprocess
import sys
from pathlib import Path
import numpy as np
from PIL import Image

LAB = Path('/home/user/celup_lab/celup_lab')
OUT = Path('/home/user/celup_lab/comparison_sheets')
OUT.mkdir(exist_ok=True)

# Test images with gradients
IMAGES = {
    'grad8': Path('tests/grad8_src.webp'),
    'rampnoise': Path('tests/rampnoise48_src.webp'),
    'huearc': Path('tests/huearc48_src.webp'),
}

MODES = [
    'bilinear',
    'cubic',
    'lanczos3',
    'xbr',
    'xbrz',
    'super_xbr',
    'pbcc_xbr',
]

SCALE = 2

def run_mode(src, out, scale, mode):
    cmd = [str(LAB), str(src), str(out), str(scale), '--mode', mode, '--max-mib', '2048']
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0

def make_sheet(name, src, modes, scale):
    print(f"\n=== {name} ({scale}x) ===")
    cells = []
    labels = []
    
    for mode in modes:
        out = f'/tmp/gradient_test_{name}_{mode}.webp'
        ok = run_mode(src, out, scale, mode)
        if ok:
            img = np.asarray(Image.open(out).convert('RGBA'))
            cells.append(img)
            labels.append(mode)
            print(f"  ok  {mode}")
        else:
            print(f"  FAIL {mode}")
            cells.append(np.zeros((64, 64, 4), dtype=np.uint8))
            labels.append(f"{mode} [FAIL]")
    
    if cells:
        H = min(c.shape[0] for c in cells)
        cells = [c[:H, :] for c in cells]
        sheet = np.concatenate(cells, axis=1)
        path = OUT / f'gradient_{name}_{scale}x.webp'
        Image.fromarray(sheet, 'RGBA').save(path, lossless=True)
        print(f"  wrote {path} ({sheet.shape[1]}x{sheet.shape[0]})")
        return path
    return None

def main():
    for img_name, img_path in IMAGES.items():
        if img_path.exists():
            make_sheet(img_name, img_path, MODES, SCALE)
    
    print("\n=== All gradient tests generated ===")

if __name__ == '__main__':
    main()
