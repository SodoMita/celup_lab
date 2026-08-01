#!/usr/bin/env python3
"""Generate comprehensive comparison sheets in WebP format with ground truth."""
import subprocess
from pathlib import Path
import numpy as np
from PIL import Image

LAB = Path('/home/user/celup_lab/celup_lab')
OUT = Path('/home/user/celup_lab/comparison_sheets')
OUT.mkdir(exist_ok=True)

IMAGES = {
    'poor_smiley': Path('images/poor smiley.webp'),
    'miya': Path('images/miya_normal.webp'),
    'shapes': Path('tests/shapes64.webp'),
    'diagline': Path('tests/diagline48.webp'),
}

MODES_2X = [
    ('bilinear', []),
    ('cubic', []),
    ('lanczos3', []),
    ('scale2x', []),
    ('xbrz', []),
    ('autodeblur', ['-r', '6', '-s', '100', '-g', '64', '-D', 'remap', '-c', 'linear', '-k', 'bspline']),
]

MODES_4X = [
    ('bilinear', []),
    ('cubic', []),
    ('lanczos3', []),
    ('xbrz', []),
    ('autodeblur', ['-r', '6', '-s', '100', '-g', '64', '-D', 'remap', '-c', 'linear', '-k', 'bspline']),
]

def run_mode(src, out, scale, mode, extra):
    cmd = [str(LAB), str(src), str(out), str(scale), '--mode', mode, '--max-mib', '2048', *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode == 0

def make_sheet(name, src, modes, scale):
    print(f"\n=== {name} ({scale}x) ===")
    cells = []
    labels = ['ground_truth']
    
    # Add ground truth (original image resized to output size for comparison)
    gt = np.asarray(Image.open(src).convert('RGBA'))
    # Resize ground truth to output dimensions using nearest neighbor
    if scale == 2:
        gt_resized = gt.repeat(2, axis=0).repeat(2, axis=1)
    elif scale == 4:
        gt_resized = gt.repeat(4, axis=0).repeat(4, axis=1)
    else:
        gt_resized = gt.repeat(scale, axis=0).repeat(scale, axis=1)
    cells.append(gt_resized)
    
    for mode, extra in modes:
        out = f'/tmp/sheet_{name}_{mode}.webp'
        ok = run_mode(src, out, scale, mode, extra)
        if ok:
            img = np.asarray(Image.open(out).convert('RGBA'))
            cells.append(img)
            labels.append(mode)
            print(f"  ok  {mode}")
        else:
            print(f"  FAIL {mode}")
    
    if cells:
        H = min(c.shape[0] for c in cells)
        cells = [c[:H, :] for c in cells]
        sheet = np.concatenate(cells, axis=1)
        path = OUT / f'{name}_{scale}x.webp'
        Image.fromarray(sheet, 'RGBA').save(path, lossless=True)
        print(f"  wrote {path} ({sheet.shape[1]}x{sheet.shape[0]})")
        return path
    return None

def main():
    # 2x comparisons
    for img_name, img_path in IMAGES.items():
        if img_path.exists():
            make_sheet(img_name, img_path, MODES_2X, 2)
    
    # 4x comparisons (only smaller images)
    for img_name in ['shapes', 'diagline']:
        img_path = IMAGES[img_name]
        if img_path.exists():
            make_sheet(img_name, img_path, MODES_4X, 4)
    
    print("\n=== All sheets generated ===")
    for f in sorted(OUT.glob('*.webp')):
        print(f"  {f.name}")

if __name__ == '__main__':
    main()
