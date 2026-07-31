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


def step48_src():
    """48x48: vertical step blurred with gaussian sigma=1.5 src px (v4.7
    analytic-deblur ground truth: channels wired to rise/fall oppositely
    so hue-inversion bugs are visible; transect checks in handoff v4.7)."""
    yy, xx = np.mgrid[0:48, 0:48]
    step = (xx >= 24).astype(np.float64)
    from scipy.ndimage import gaussian_filter

    img = np.clip(gaussian_filter(step, 1.5) * 0.7 + 0.15, 0, 1)
    rgba = np.dstack([img, img * 0.5 + 0.3, 1 - img, np.ones_like(img)])
    save("step48_src.webp", (rgba * 255).astype(np.uint8))


def caps48_src():
    """48x48: dark lines with FLAT caps on white (snake-tongue forensics,
    v4.8): horizontal 3px and 2px, vertical 2px, one shallow diagonal,
    one T-junction, blurred sigma=1.2 src px."""
    from scipy.ndimage import gaussian_filter

    lum = np.ones((48, 48), np.float64)
    lum[8:11, 6:30] = 0.10     # horizontal 3px, flat caps at x=6 and x=29
    lum[20:22, 30:44] = 0.15   # horizontal 2px, flat caps
    lum[26:42, 8:10] = 0.12    # vertical 2px, flat caps
    for xx in range(10, 34):   # shallow diagonal 1.5px-ish
        yy = int(round(40 - 0.20 * (xx - 10)))
        lum[yy, xx] = 0.18
        lum[yy + 1, xx] = 0.45
    lum[8:14, 17:20] = 0.10    # T-junction stem over the first line
    lum = gaussian_filter(lum, 1.2)
    rgba = np.dstack([lum, lum * 0.92 + 0.02, lum * 0.85 + 0.05,
                      np.ones_like(lum)])
    save("caps48_src.webp", (np.clip(rgba, 0, 1) * 255).astype(np.uint8))


def twoline48_src():
    """48x48: a 3px dark-red vertical line INSIDE a warm light region,
    plus an independent warm->deep-blue border 6 src px away, blurred
    sigma=1.2 src px -- v4.8 '2 gradients surrounding edge combined' /
    halo forensics: each flank must steepen against its OWN background
    (warm on both sides of the line, warm|blue at the border), never
    the mix of the two, and plateaus must not overshoot."""
    from scipy.ndimage import gaussian_filter

    yy, xx = np.mgrid[0:48, 0:48]
    rgb = np.zeros((48, 48, 4), np.float64)
    rgb[..., 0] = np.where(xx < 24, 0.95, 0.10)   # warm light | deep blue
    rgb[..., 1] = np.where(xx < 24, 0.80, 0.12)
    rgb[..., 2] = np.where(xx < 24, 0.65, 0.55)
    rgb[..., 3] = 1.0
    line = np.abs(xx - 13) < 1.5                  # line inside warm side
    rgb[line, 0] = 0.30
    rgb[line, 1] = 0.10
    rgb[line, 2] = 0.12
    for c in range(3):
        rgb[..., c] = gaussian_filter(rgb[..., c], 1.2)
    save("twoline48_src.webp", (np.clip(rgb, 0, 1) * 255).astype(np.uint8))


def huearc48_src():
    """48x48: horizontal sweep whose colour path ARCS in RGB (saturated
    hue sweep red -> orange -> yellow -> green), blurred sigma=1.0 src px.
    v4.8 'watered away colors' forensics: the deblurred sweep must keep
    the arc (full HSV saturation), not collapse onto the chord between
    the window extremes."""
    from scipy.ndimage import gaussian_filter
    import colorsys

    yy, xx = np.mgrid[0:48, 0:48]
    h = xx / 47.0 * 0.50                        # red->cyan hue arc (fast)
    rgb = np.zeros((48, 48, 4), np.float64)
    for i in range(48):
        for j in range(48):
            r, g, b = colorsys.hsv_to_rgb(h[i, j], 0.85, 0.95)
            rgb[i, j] = (r, g, b, 1.0)
    for c in range(3):
        rgb[..., c] = gaussian_filter(rgb[..., c], 1.0)
    save("huearc48_src.webp", (np.clip(rgb, 0, 1) * 255).astype(np.uint8))


def rampnoise48_src():
    """48x48: smooth vertical ramp + tiny dither noise (+/-2 LSB), blurred
    sigma=1.0 src px -- v4.8 'outstanding pixels at gradient centre'
    forensics: anchored evaluation must pass the dither through at gain 1,
    not amplify it k-fold."""
    from scipy.ndimage import gaussian_filter

    yy, xx = np.mgrid[0:48, 0:48]
    rng = np.random.default_rng(20260731)
    lum = xx / 47.0
    rgb = np.dstack([lum, lum * 0.9 + 0.03, 1.0 - lum * 0.8,
                     np.ones_like(lum)])
    for c in range(3):
        rgb[..., c] = gaussian_filter(rgb[..., c], 1.0)
    rgb[..., 0:3] += rng.integers(-6, 7, (48, 48, 1)).astype(np.float64) / 255.0
    save("rampnoise48_src.webp", (np.clip(rgb, 0, 1) * 255).astype(np.uint8))


def cornerstar48_src():
    """48x48: HARD-edged dark geometry on white -- a filled triangle with
    three acute tips and an L-corner bar with square ends -- barely
    blurred (sigma=0.5 src px): v4.9 'rounded corners + neon glow'
    forensics (the smiley class: pixelated, almost unblurred source).
    Acute tips must stay pointed, flat plateaus must stay flat, and a
    sigma-wide luminance skirt around the dark strokes is a failure.
    Integer half-plane fill: bit-exact on any platform."""
    from scipy.ndimage import gaussian_filter

    yy, xx = np.mgrid[0:48, 0:48]

    def tri(ax, ay, bx, by, cx, cy):
        d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        a = ((by - cy) * (xx - cx) + (cx - bx) * (yy - cy)) / d
        b = ((cy - ay) * (xx - cx) + (ax - cx) * (yy - cy)) / d
        return (a >= 0) & (b >= 0) & (a + b <= 1)

    lum = np.ones((48, 48), np.float64)
    lum[tri(8, 40, 44, 44, 20, 8)] = 0.12     # triangle: tips at (8,40),(44,44),(20,8)
    lum[26:46, 40:43] = 0.15                  # L-corner: vertical bar
    lum[26:29, 30:40] = 0.15                  # ...joined horizontal bar (square ends)
    lum = gaussian_filter(lum, 0.5)
    rgba = np.dstack([lum, lum * 0.94 + 0.03, lum * 0.88 + 0.06,
                      np.ones_like(lum)])
    save("cornerstar48_src.webp", (np.clip(rgba, 0, 1) * 255).astype(np.uint8))


def diagline48_src():
    """64x64: 45-degree dark line (4 px wide, |x-y|<=2) on white,
    gaussian-blurred sigma=1.0 and quantized to 45 gray levels -- the
    smiley class (hard pixelated diagonal, the exact geometry that
    turns into staircase treads when the reconstruction base is too
    crisp).  Deterministic, no RNG.  Gate: tests/check_stairs.py."""
    from scipy.ndimage import gaussian_filter

    yy, xx = np.mgrid[0:64, 0:64]
    lum = np.where(np.abs(xx - yy) <= 2, 0.05, 0.95)
    lum = gaussian_filter(lum, 0.5)
    lum = np.round(lum * 44.0) / 44.0
    rgba = np.dstack([lum, lum * 0.95 + 0.03, lum * 0.90 + 0.06,
                      np.ones_like(lum)])
    save("diagline48_src.webp", (np.clip(rgba, 0, 1) * 255).astype(np.uint8))


if __name__ == "__main__":
    torture_src()
    pixelart_src()
    hline8_src()
    grad8_src()
    thin96()
    parallel96()
    rings96()
    step48_src()
    caps48_src()
    twoline48_src()
    huearc48_src()
    rampnoise48_src()
    cornerstar48_src()
    diagline48_src()
