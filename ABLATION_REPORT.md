# Autodeblur Feature Ablation Report

## Setup
- Binary: merge1 branch (8081 lines, rebuilt with CELUP_NOHULL/CELUP_NOSPECKLE support)
- 13 feature sets tested, each disabling one feature group
- 3 test images: poor_smiley (256×256, hard pixel art), cat (400×400, photo), pikachu (831×474, illustration)
- Scale: 2×
- Metrics: MSE vs source (2× downsample then up), PSNR, SSIM, speckle fraction, ringing score, staircase score, halo score

## Comparison Sheet
https://tmpfiles.org/dl/wIwpRBrIDzvh/ablate_full_sheet.webp

## Key Findings

### poor_smiley (hard pixel art — the stress test)

| Feature Set          | MSE   | PSNR  | SSIM  | Speck% | Ring  | Stair | Halo  |
|----------------------|-------|-------|-------|--------|-------|-------|-------|
| baseline             | 213.8 | 24.83 | .9551 | 1.19%  | 18.67 | .1597 | 7.034 |
| no_amp               | 213.8 | 24.83 | .9550 | 1.20%  | 18.76 | .1600 | 7.068 |
| no_terrace           | 213.7 | 24.83 | .9552 | 1.22%  | 18.38 | .1548 | 6.808 |
| no_dip               | 213.8 | 24.83 | .9551 | 1.19%  | 18.67 | .1597 | 7.034 |
| no_peel              | 213.8 | 24.83 | .9551 | 1.19%  | 18.67 | .1595 | 6.770 |
| **no_z**             | **384.2** | **22.29** | **.9231** | 1.54%  | 23.90 | .1583 | 9.178 |
| **no_hull**          | 208.9 | 24.93 | .9571 | **0.89%** | **14.14** | **.1461** | **5.660** |
| no_speckle           | 213.8 | 24.83 | .9551 | 1.19%  | 18.67 | .1597 | 7.034 |
| no_trust             | 209.9 | 24.91 | .9553 | 1.12%  | 17.53 | .1483 | 7.049 |
| tight_trust          | 214.9 | 24.81 | .9545 | 1.29%  | 19.58 | .1636 | 7.041 |
| method_push          | 221.1 | 24.69 | .9538 | 1.24%  | 19.80 | .1598 | 6.451 |
| method_remap         | 213.8 | 24.83 | .9551 | 1.19%  | 18.67 | .1597 | 7.034 |
| **method_analytical**| **69.2** | **29.73** | **.9691** | **0.75%** | **0.50** | **.0384** | 7.923 |

### cat (photo — smooth content)

| Feature Set          | MSE   | PSNR  | SSIM  | Speck% | Ring  | Stair | Halo  |
|----------------------|-------|-------|-------|--------|-------|-------|-------|
| baseline             | 15.0  | 36.36 | .9669 | 0.12%  | 7.25  | .0550 | 4.896 |
| **no_hull**          | **516.4** | **21.00** | **.5571** | **16.3%** | **38.67** | **.2093** | **17.53** |
| tight_trust          | 13.9  | 36.69 | .9674 | 0.11%  | 7.18  | .0530 | 4.875 |
| method_analytical    | 4375.6| 11.72 | .2737 | 1.18%  | 9.95  | .0924 | 5.285 |

### pikachu (illustration — soft edges)

| Feature Set          | MSE   | PSNR  | SSIM  | Speck% | Ring  | Stair | Halo  |
|----------------------|-------|-------|-------|--------|-------|-------|-------|
| baseline             | 14.4  | 36.55 | .9838 | 0.23%  | 1.06  | .0506 | 0.166 |
| **no_hull**          | **78.0** | **29.21** | **.9409** | 1.30%  | 1.85  | .0640 | 0.496 |
| method_analytical    | 83.0  | 28.94 | .9844 | 0.16%  | 0.00  | .0096 | 3.763 |

## Analysis

### Feature Impact Rankings (by effect on MSE)

1. **Hull clamp (CELUP_NOHULL)** — **CRITICAL**
   - Removing it on cat: MSE 15→516 (+34x), SSIM .97→.56, speckle 0.12%→16.3%
   - The hull clamp is the single most important safety feature. Without it, the erf model extrapolates wildly beyond observed colours.
   - On poor_smiley it slightly *helps* MSE (213→209) because pixel art has narrow colour ranges that the hull restricts.

2. **erf-gain map (CELUP_NOZ)** — **MAJOR**
   - Removing on poor_smiley: MSE 213→384 (+80%), SSIM .96→.92
   - The erf-gain map is essential for pixel art quality. On photos it barely matters (cat: 15→13.5, slightly better without).

3. **Analytical method (method 3)** — **BEST for pixel art, WORST for photos**
   - poor_smiley: MSE 69.2 (3× better than baseline), near-zero ringing
   - cat: MSE 4375 (290× worse!), SSIM .27 — catastrophic on photos
   - pikachu: MSE 83 (5.7× worse), but SSIM .984 (actually better than baseline)
   - The analytical method is image-type dependent; needs a photo detector to gate it.

4. **Trust gates (CDG)** — **MODERATE**
   - Disabling (CDG=0,1): slight improvement on poor_smiley (MSE 213→210)
   - Tightening (CDG=0.01,0.05): slight degradation (MSE 214.9)
   - Default trust gates are well-tuned for pixel art.

5. **no_amp, no_dip, no_peel, no_speckle, no_terrace** — **NEGLIGIBLE**
   - All produce <1% MSE change on poor_smiley
   - These features fire rarely on the test images and have minimal effect

### Method comparison
- **remap (method 1)**: Best for pixel art (MSE 213.8, auto-selected)
- **push (method 2)**: Slightly worse (MSE 221.1)
- **analytical (method 3)**: Best for pixel art (MSE 69.2), catastrophic for photos

## Recommendations

1. **Hull clamp is essential** — never disable. It's the #1 safety feature preventing runaway extrapolation.

2. **erf-gain map is essential for pixel art** — keep enabled. It's the #2 feature by impact.

3. **Analytical method should be auto-gated** — add a photo detector (e.g., qconf threshold or class map edge density) and only use method 3 when qconf > 0.5 (hard-quantized source).

4. **no_amp/no_dip/no_peel are dead code paths** on these test images — they only fire on specific edge cases (narrow lines, etc.). More specialized test images needed.

5. **Trust gates are well-calibrated** — the default values (0.03, 0.10) are close to optimal.

## Available Env Vars for Ablation

| Env Var | Effect | Default |
|---------|--------|---------|
| `CELUP_NOAMP=1` | Disable narrow-feature amplitude restoration | Off |
| `CELUP_NOTER=1` | Disable terrace cleanup | Off |
| `CELUP_NODIP=1` | Disable dip/line-class claim | Off |
| `CELUP_NOPEEL=1` | Disable contour peeling | Off |
| `CELUP_NOZ=1` | Disable erf-gain map | Off |
| `CELUP_NOHULL=1` | Disable hull clamp (DANGEROUS) | Off |
| `CELUP_NOSPECKLE=1` | Disable speckle suppression in base | Off |
| `CDG=lo,hi` | Override trust gates (default: 0.03,0.10) | 0.03,0.10 |
| `-D remap/push/analytical` | Force deblur method | auto |

## Tool

```bash
python3 autodeblur_ablate.py image1.webp image2.webp --scale 2 --output-dir results/ --save-images
```

---

## Soft Hull Fix (v4.9.4)

### Problem
The hard hull clamp was the #1 safety feature (preventing runaway extrapolation on photos) but was over-restrictive on pixel art (qconf=1.0). On poor_smiley, removing the hull actually improved all metrics (MSE 213→209, speckle 1.19%→0.89%), meaning the hull was creating speckle by clamping the deblur's legitimate steepening.

### Solution: qconf-gated soft hull
Expand the hull proportionally to `w × qconf × 0.55`:
- On pixel art (qconf=1.0): hull expands by up to 55% of the gap between the observed range and the source range, proportional to deblur confidence
- On photos (qconf≈0): hull stays tight (no expansion), preserving the critical safety behavior

### Results

| Image | Hard Hull MSE | Soft Hull MSE | Hard Hull Speck% | Soft Hull Speck% |
|-------|--------------|---------------|-------------------|------------------|
| poor_smiley | 213.8 | **210.9** | 1.19% | **1.03%** |
| cat | 15.0 | **15.0** | 0.12% | 0.12% |
| pikachu | 14.4 | **14.3** | 0.23% | 0.23% |

Pixel art gets improved quality, photos are unchanged.

### Analytical Method Comparison

| Method | poor_smiley MSE | cat MSE | Notes |
|--------|----------------|---------|-------|
| 019fc1ba analytic | 54.9 | 35.8 | Separate function, global gradient walk + u-remap |
| 019fbf78 compress2x2 | 281.7 | 8.7 | Inside autodeblur_pass, per-pixel erf steepening |
| merge1 analytical (analog) | 69.2 | 4375.6 | PCA power-sigmoid, best on pixel art, worst on photos |
| merge1 remap (baseline) | 210.9 | 15.0 | Best balance |

The 019fc1ba analytic is the best compromise: decent on both pixel art and photos.
The 019fbf78 compress2x2 is best on photos but worst on pixel art.
The merge1 analytical (analog) is best on pixel art but catastrophic on photos.
