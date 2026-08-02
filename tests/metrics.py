#!/usr/bin/env python3
"""Smiley 2x metrics: ink ratio, halo veil, darkmean vs NN (ROI rows 240:500),
plus the GRAY TEST: fraction of pixels that are neither ink-win nor paper-win.
The source is an (almost) binary drawing: ~99.2% of pixels sit within 24/255
of pure black/white.  A correct deblur must return the plateaus; surviving
mid-gray is mush/veil by definition."""
import sys
import numpy as np
from PIL import Image
from scipy import ndimage

GRAY_LO, GRAY_HI = 24.0, 232.0  # tolerance around the two plateaus (8-bit lum)


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.float32)


def gray_fraction(lum):
    return ((lum > GRAY_LO) & (lum < GRAY_HI)).mean()


def metrics(path, nn_path, roi=(240, 500)):
    a, nn = load(path), load(nn_path)
    r0, r1 = roi
    a, nn = a[r0:r1], nn[r0:r1]
    lum_a = a.mean(2)
    lum_n = nn.mean(2)
    mask = lum_n < 128
    ink = (lum_a[mask] < 128).mean() if mask.any() else 0.0
    darkmean = lum_a[mask].mean() if mask.any() else 255.0
    d2 = ndimage.binary_dilation(mask, iterations=2)
    d4 = ndimage.binary_dilation(mask, iterations=4)
    band = d4 & ~d2
    # halo = mean darkness (255 - lum) in the skirt band, minus NN's own
    halo = (255.0 - lum_a[band]).mean()
    halo_nn = (255.0 - lum_n[band]).mean()
    return ink, halo, halo_nn, darkmean, gray_fraction(lum_a), gray_fraction(lum_n)


if __name__ == "__main__":
    out = sys.argv[1]
    nn = sys.argv[2]
    ink, halo, halo_nn, dm, gf, gf_nn = metrics(out, nn)
    print(f"{out}: ink={ink:.3f} halo={halo:.2f} (nn {halo_nn:.2f}) "
          f"darkmean={dm:.1f} gray={gf * 100:.2f}% (nn {gf_nn * 100:.2f}%)")
