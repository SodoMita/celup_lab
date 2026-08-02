# TODO — merge1 branch

## Completed ✓

### Branch merge
- All 13 feature branches merged into `merge1`
- Comparison sheets removed from repo history
- `merge1` branch force-pushed to origin

### All fixes applied
- jinc2_bilateral: IDENTICAL to 019fbf57
- xBR: real Hyllian xBR, IDENTICAL to 019fbf57
- adaptive parameters: IDENTICAL to 019fba1b
- dsdf: 3x3 consensus matches 019fba1b HEAD
- xbrz/xbr/hybrid dispatch: added
- 019fba18 autodeblur_pass port: MSE 208→115

### Soft hull fix (v4.9.4)
- qconf-gated hull expansion: pixel art gets wider hull, photos stay tight
- poor_smiley: MSE 213→211, speckle 1.19%→1.03%; cat unchanged

### Gradient deblur method 4
- Ported 019fc1ba's global gradient walk + u-remap as method 4 (-D gradient)
- Best compromise: poor_smiley MSE 160, cat MSE 40
- Both analytical implementations now available:
  - `-D analytical` (method 3): PCA analog, best on pixel art (MSE 69), worst on photos (MSE 4375)
  - `-D gradient` (method 4): global gradient walk, good on both (MSE 160/40)

### Feature ablation framework
- CELUP_NOHULL/CELUP_NOSPECKLE env vars + autodeblur_ablate.py
- Full ablation report: ABLATION_REPORT.md

## What's next

### 1. Autodeblur base render mismatch
- 019fba18 selects bspline kernel (sigma 0.50), merge1 selects triangle (sigma 0.50)
- This causes the base render to differ, leading to different autodeblur output
- 019fba18 MSE 178.3 vs merge1 MSE 210.9 (with same autodeblur_pass)
- Need to investigate why auto_tune selects different kernels

### 2. Port 019fbf78 compress2x2 as method 5
- Inside autodeblur_pass with bypassed safety gates
- Best on photos (MSE 8.7 on cat), worst on pixel art (MSE 281.7)
- Would need to be a separate function or method==3 branch inside autodeblur_pass

### 3. 019fbfb9 branch fails to build

### 4. 19 modes working (4 deblur methods)
nearest, bilinear, cubic, mitchell, lanczos3, triangle, smooth, adaptive, autodeblur, autoblur, sdf, msdf, dsdf, jinc2_bilateral, jinc2_auto, xbrz, xbr, hybrid
+ 4 deblur methods: remap, push, analytical, gradient
