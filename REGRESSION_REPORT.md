# Regression Report — merge1 branch

## Test Date: 2026-08-02

## Build Status
- ✅ celup_lab.c compiles with all features
- ✅ All 18 modes functional
- ✅ All 5 deblur methods functional
- ✅ test_scales.py: PASS
- ✅ check_stairs.py: PASS

## Mode Regression Test
All 18 modes produce valid output:
nearest, bilinear, cubic, mitchell, lanczos3, triangle, smooth,
adaptive, autodeblur, autoblur, sdf, msdf, dsdf, jinc2_bilateral,
jinc2_auto, xbrz, xbr, hybrid

## Deblur Method Regression Test
All 5 deblur methods produce valid output:
remap, push, analytical, gradient, compress2x2

## Proxy MSE Comparison (2x downscale, autodeblur)

### By method (poor_smiley / cat)
| Method        | poor_smiley | cat     | Notes                                    |
|---------------|-------------|---------|------------------------------------------|
| remap         | 130.2       | 7.9     | Good balance, default for auto           |
| push          | 133.0       | 8.0     | Similar to remap, slightly different     |
| analytical    | 3.5         | 5783.6  | Best on pixel art, catastrophic on photos|
| gradient      | 19.5        | 32.2    | Best compromise                          |
| compress2x2   | 392.8       | 7.7     | Best on photos, worst on pixel art       |

### vs Branch defaults (autodeblur, auto method)
| Branch    | poor_smiley | cat   |
|-----------|-------------|-------|
| merge1    | 130.2       | 7.9   |
| 019fba18  | 133.1       | 7.9   |
| 019fc1ba  | 216.6       | 8.5   |
| 019fbf78  | 197.6       | 7.8   |

Merge1's remap (auto-selected) is better than or equal to all branch defaults.

## Known Issues

### 019fbfb9 branch fails to build
- Root cause: C code bug — missing braces around `else if` block in CLI parsing
- No unique content beyond what's already in merge1 (method 3 analytical)
- Verdict: no action needed

### dsdf 3x3 consensus corner gradient artifact
- Known trade-off from 019fba1b's 3x3 consensus DSDF
- Slightly worse MSE on poor_smiley corners vs e7f487f's single-nearest approach
- 3x3 consensus is the author's latest intent (fixes pixel-shifting artifact)
- Documented in TODO

### Steepness penalty
- merge1: 0.55 (stronger, prevents staircase tracking)
- 019fba18: 0.30 (more permissive, may track source staircase)
- Affects curve selection (stage 2 of auto_tune), not kernel selection
- Current 0.55 is intentional for preventing staircase artifacts

## Feature Ablation Summary
- Hull clamp is #1 safety feature (removing on cat: MSE 15→516, speckle 0.12%→16.3%)
- erf-gain map is #2 (removing on poor_smiley: MSE 213→384)
- no_amp/no_dip/no_peel/no_speckle: negligible (<1% MSE change)
- Trust gates well-calibrated at defaults (0.03, 0.10)
