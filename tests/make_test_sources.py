#!/usr/bin/env python3
"""Regenerate the celup_lab test fixture images (kept out of git).

Run: python3 tests/make_test_sources.py
Writes lossless WebP inputs into tests/.  The faithful-metric evaluators
(evaluate_upscalers.py / hourglass_metric.py / test_celup3.py) generate
their own inputs internally and do not need these files; the fixtures here
are the manual-torture and 8x-regression sources referenced by the README.
"""
from pathlib import Path

import numpy as np
from PIL import Image

OUT = Path(__file__).resolve().parent


def save(name, rgba):
    Image.fromarray(rgba.astype(np.uint8), "RGBA").save(OUT / name, lossless=True)
    print("wrote", OUT / name)


def torture_src():
    """Left (x<64): 1px green/pink checker.  Right (x>=64): 8x8-tiled dashed
    crosshatch (the hourglass tile 80 41 22 14 08 14 22 41, bit0 = tile col0)."""
    y, x = np.mgrid[0:96, 0:96]
    tile = np.array([0x80, 0x41, 0x22, 0x14, 0x08, 0x14, 0x22, 0x41],
                    np.uint8)
    right = (tile[y % 8] >> (7 - ((x - 64) % 8).astype(np.uint8))) & 1
    idx = np.where(x < 64, np.where((x + y) % 2 == 0, 2, 1),
                   np.where(right == 1, 0, 3))
    pal = np.array([[20, 20, 120, 255], [30, 90, 30, 255],
                    [240, 220, 240, 255], [250, 250, 250, 255]], np.uint8)
    save("torture_src.webp", pal[idx])


def pixelart_src():
    """48x48: red/navy 1px checker bg, yellow 16x16 square at (16,16),
    3px-wide cyan 45-degree main diagonal on top."""
    y, x = np.mgrid[0:48, 0:48]
    idx = np.where((x + y) % 2 == 0, 0, 2)
    idx = np.where((x >= 16) & (x <= 31) & (y >= 16) & (y <= 31), 3, idx)
    idx = np.where(np.abs(x - y) <= 1, 1, idx)
    pal = np.array([[16, 16, 32, 255], [80, 200, 255, 255],
                    [200, 60, 60, 255], [250, 220, 80, 255]], np.uint8)
    save("pixelart_src.webp", pal[idx])


def hline8_src():
    """96x96: AA 2px horizontal line rows 20-21 + 1px line row 50 (AA row 51),
    gentle sine diagonal near row 74, red bar rows 10-14."""
    img = np.full((96, 96, 4), (255, 255, 255, 255), np.uint8)
    img[20, :] = (140, 140, 140, 255)
    img[21, :] = (40, 40, 40, 255)
    img[50, :] = (40, 40, 40, 255)
    img[51, :] = (140, 140, 140, 255)
    for xx in range(96):
        yy = int(70 + 10 * np.sin(xx / 14.0))
        img[yy, xx] = (60, 60, 60, 255)
    img[10:15, 10:60] = (200, 30, 30, 255)
    save("hline8_src.webp", img)


def grad8_src():
    """96x64: vertical red/blue gradient with a green bar rows 30-33."""
    g = np.zeros((64, 96, 4), np.uint8)
    for yy in range(64):
        t = yy / 63.0
        g[yy, :, 0] = int(255 * t)
        g[yy, :, 2] = int(255 * (1 - t))
        g[yy, :, 1] = 80
        g[yy, :, 3] = 255
    g[30:34, :, 1] = 255
    save("grad8_src.webp", g)


def thin96():
    """96x96: shallow 1px anti-aliased diagonal line (staircase torture)."""
    img = np.full((96, 96, 4), (240, 238, 230, 255), np.uint8)
    for xx in range(96):
        yy = int(round(72 - 0.35 * xx))
        img[yy, xx] = (90, 80, 120, 255)
        img[yy - 1, xx] = (170, 165, 180, 255)
    save("thin96.webp", img)


def parallel96():
    """96x96: two parallel shallow 1px diagonal lines."""
    img = np.full((96, 96, 4), (248, 245, 240, 255), np.uint8)
    for xx in range(96):
        for k in (0, 1):
            yy = int(round(30 + 12 * k + 0.18 * xx))
            img[yy, xx] = (70, 60, 90, 255)
    save("parallel96.webp", img)


def rings96():
    """96x96: hard concentric rings (Nyquist rises toward the outside)."""
    yy, xx = np.mgrid[0:96, 0:96]
    r = np.sqrt((xx - 48.0) ** 2 + (yy - 48.0) ** 2)
    v = ((np.sin(r * 0.9) > 0.25) * 255).astype(np.uint8)
    save("rings96.webp", np.dstack([v, v, v, np.full_like(v, 255)]))


if __name__ == "__main__":
    torture_src()
    pixelart_src()
    hline8_src()
    grad8_src()
    thin96()
    parallel96()
    rings96()
