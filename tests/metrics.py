#!/usr/bin/env python3
"""Smiley 2x metrics: ink ratio, halo veil, darkmean vs NN (ROI rows 240:500)."""
import sys
import numpy as np
from PIL import Image
from scipy import ndimage


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.float32)


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
    return ink, halo, halo_nn, darkmean


if __name__ == "__main__":
    out = sys.argv[1]
    nn = sys.argv[2]
    ink, halo, halo_nn, dm = metrics(out, nn)
    print(f"{out}: ink={ink:.3f} halo={halo:.2f} (nn {halo_nn:.2f}) darkmean={dm:.1f}")
