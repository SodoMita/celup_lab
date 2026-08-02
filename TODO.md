# TODO — merge1 branch

## Completed ✓

### Branch merge
- All 13 feature branches merged into `merge1`
- Comparison sheets removed from repo history
- `merge1` branch force-pushed to origin (6 commits)

### All fixes applied
- jinc2_bilateral: IDENTICAL to 019fbf57
- xBR: real Hyllian xBR, IDENTICAL to 019fbf57
- adaptive parameters: IDENTICAL to 019fba1b
- dsdf: 3x3 consensus matches 019fba1b HEAD
- xbrz/xbr/hybrid dispatch: added

### Soft hull fix (v4.9.4)
- qconf-gated hull expansion allows deblur steepening on pixel art
- poor_smiley: MSE 213→176, speckle 1.19%→1.02%; cat unchanged

### Kernel floor restoration (v4.9.3)
- Restored bspline/box/gaussian kernel floors to 019fba18 values
- bspline floor: 1.05→0.7, box: 1.0→0.75, gaussian: 0.7→0.5
- auto_tune now selects bspline (matching 019fba18)
- poor_smiley autodeblur MSE: 210.9→176.4 (matches 019fba18's 178.3)
- cat/pikachu: identical to 019fba18

### 019fba18 autodeblur_pass port
- Full 2003-line autodeblur_pass ported
- Combined with kernel floor fix, merge1 now matches 019fba18 on all images

### Gradient deblur method 4
- Ported 019fc1ba's global gradient walk + u-remap
- Best compromise: poor_smiley MSE 160, cat MSE 40

### Feature ablation framework
- CELUP_NOHULL/CELUP_NOSPECKLE env vars + autodeblur_ablate.py
- Full ablation report: ABLATION_REPORT.md

### Compress2x2 deblur method 5
- Ported 019fbf78's bilinear 2x2 block projection as method 5
- Best on photos (cat MSE 7.7, vs remap 7.9, gradient 32.2)
- Worst on pixel art (poor_smiley MSE 392.8, vs gradient 19.5)
- Blurry corner artifact on pixel art (known trade-off from 019fbf78)
- Bypasses coherence gate and erf-gain post-map for uniform contours
- Simplified trust gate (range + crossing count, no erf RMSE)

## Remaining items

### 1. 019fbfb9 branch fails to build
- Root cause: C code bug — missing braces around `else if` block in CLI parsing
  ```c
  else if (!strcmp(av[i + 1], "analytical"))
    deblur_method = 3;
    deblur_method = 3;  // duplicate, not inside if
    deblur_method = 2;  // always overwrites to 2
  ```
- The branch's analytical_deblur_pass is a thin wrapper calling autodeblur_pass(method=3)
- No unique content beyond what's already in merge1 (method 3 analytical)
- Verdict: no action needed — branch content already fully merged

### 2. Steepness penalty
- merge1: 0.55 (stronger, prevents staircase tracking)
- 019fba18: 0.30 (more permissive, may track source staircase)
- Affects curve selection (stage 2 of auto_tune), not kernel selection
- Current 0.55 is intentional for preventing staircase artifacts

### 3. Consider making gradient method (4) the default for autodeblur
- Best compromise: good on both pixel art and photos
- Current default is auto (proxy picks remap or push)

### 4. 19 modes working (5 deblur methods)
nearest, bilinear, cubic, mitchell, lanczos3, triangle, smooth, adaptive,
autodeblur, autoblur, sdf, msdf, dsdf, jinc2_bilateral, jinc2_auto,
xbrz, xbr, hybrid
+ 5 deblur methods: remap, push, analytical, gradient, compress2x2

## Deblur method comparison (proxy MSE, 2x downscale)

| Method        | poor_smiley | cat     | Notes                                    |
|---------------|-------------|---------|------------------------------------------|
| remap         | 130.2       | 7.9     | Good balance, default for auto           |
| push          | 133.0       | 8.0     | Similar to remap, slightly different     |
| analytical    | 3.5         | 5783.6  | Best on pixel art, catastrophic on photos|
| gradient      | 19.5        | 32.2    | Best compromise                          |
| compress2x2   | 392.8       | 7.7     | Best on photos, worst on pixel art       |
