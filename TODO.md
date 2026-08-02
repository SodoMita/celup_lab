# TODO — merge1 branch

## Completed ✓

### Branch merge
- All 13 feature branches merged into `merge1`
- celup_lab.c rebuilt with all branch features (8090 lines)
- Comparison sheets removed from repo history
- `merge1` branch force-pushed to origin

### Fixes applied
- jinc2_bilateral: pixel-perfect IDENTICAL to 019fbf57
- xBR: replaced stub with real Hyllian xBR, IDENTICAL to 019fbf57
- adaptive parameters: adopted 019fba1b's, IDENTICAL to 019fba1b
- dsdf: 3x3 consensus matches 019fba1b HEAD (known corner gradient trade-off vs e7f487f single-pixel)
- xbrz/xbr/hybrid dispatch: added
- 019fba18 autodeblur_pass port: MSE improved 208→115

### Feature ablation framework
- Added `CELUP_NOHULL` and `CELUP_NOSPECKLE` env vars
- Created `autodeblur_ablate.py` tool (7 metrics × 13 feature sets)
- Full ablation report: ABLATION_REPORT.md

### Soft hull fix (v4.9.4)
- **Problem**: Hard hull clamp was over-restrictive on pixel art (creating speckle by preventing legitimate steepening) but essential on photos
- **Solution**: qconf-gated soft hull — expands hull by `w × qconf × 0.55` toward source colour range
- **Result**: poor_smiley MSE 213.8→210.9, speckle 1.19%→1.03%; cat unchanged at 15.0

### Analytical method comparison
- 019fc1ba analytic: MSE 54.9 (pixel art) / 35.8 (cat) — best compromise
- 019fbf78 compress2x2: MSE 281.7 (pixel art) / 8.7 (cat) — best on photos
- merge1 analytical (analog): MSE 69.2 (pixel art) / 4375.6 (cat) — best on pixel art only

## Remaining items

### 1. Port 019fc1ba analytic as the default method 3
- Currently best compromise: works on both pixel art and photos
- Needs to replace the merge1 analog implementation
- The 019fc1ba version uses a global gradient walk + u-remap, not PCA

### 2. autodeblur vs 019fba18 still differs
- MSE 115 vs 98.5 (surrounding render_soft/upscale_autoblur differences)

### 3. 019fbfb9 branch fails to build

### 4. Regenerate comparison sheets with soft hull

### 5. 18 modes all working
nearest, bilinear, cubic, mitchell, lanczos3, triangle, smooth, adaptive, autodeblur, autoblur, sdf, msdf, dsdf, jinc2_bilateral, jinc2_auto, xbrz, xbr, hybrid
