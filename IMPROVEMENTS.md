# Celup Lab Improvements - v4.9.x Series

## v4.9.3 (2026-07-31): Kernel Support & Parameter Fixes
- Fixed kernel support cap (8→32) to prevent staircase artifacts
- Fixed auto_tune_soft_params to respect user-pinned sigma values
- Raised blur_pm radius cap (12→32)

## v4.9.4 (2026-08-01): Tangent Span Improvement
- Increased tangent span T from clamp(scale*0.75+0.5, 1, 3) to clamp(scale+0.5, 1, 5)
- At 4x: T=4 (was 3), providing more samples for contour averaging
- Junction gating (coh) still protects corners

## v4.9.5 (2026-08-01): Gaussian Tangential Weights
- Line-sample tangential averaging: uniform box → Gaussian (σ=Teff*0.6)
- Pass 1.5 consensus: triangular → Gaussian (σ=T*0.55)
- Pass 2 delta smoothing: triangular → Gaussian (σ=T*0.55)
- Smoother contour averages, no hard cutoff artifacts

## v4.9.6 (2026-08-01): xBRZ Pixel-Art Upscaler
- New --mode xbrz: Zenju xBRZ 1.8 cellular-automata upscaler
- Supports integer scales 2-6
- Preserves sharp corners without staircases
- YCbCr perceptual distance LUT
- Per-scale blend patterns with rotation support

## Known Issues & Future Work
- xBRZ rotation logic needs testing on complex patterns
- Could add multi-pass refinement for autodeblur
- Consider adding xBRZ as a pre-processing step before autodeblur
