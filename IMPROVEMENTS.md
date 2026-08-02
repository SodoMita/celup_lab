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

## v4.9.10 (2026-08-01): Jinc2-Bilateral xBR mode restored

- New `--mode jinc2_bilateral`: faithful CPU port of Hyllian's
  jinc2-bilateral-xbr.slang (windowed-jinc 2-lobe space weights with
  WA=0.5, WB=0.88, a bilateral range term I(p00,c)=lanczos(luma|p00-c|*STR,2)
  guided by a bilinear pm-linear reconstruction, and an anti-ringing clamp
  to the central 2x2 source cell).  Works at any scale 1..32.
- `superxbr` (previously accepted by the validator but dispatched to
  uninitialized memory) now routes to the same jinc2-bilateral pass.
- Tunables via env: CELUP_J2B_STR (bilateral strength), CELUP_J2B_AR
  (anti-ringing amount); -s gently raises STR.
- Added to tests/test_scales.py (AA-circle sweep 1.5x..24x all step 0.000).
- Measurements at 4x: evaluate_upscalers MAE ~= lanczos3 (better on
  axis/shallow/parallel, best MAE on the grey-on-grey scene with zero
  out-of-source-range pixels); hourglass HG lower than lanczos3 on
  checker2/crosshatch/rings/diag; diagline staircase gate 2x jump95 .040
  and 4x .159 (both PASS).  Fixed build.sh to compile xbr/xbrz sources.

## Comparison sheets for the four real test images

- New `make_user_comparison_sheets.py` builds stacked mode-comparison sheets
  (nearest/bilinear/cubic/mitchell/lanczos3/jinc2_bilateral/superxbr/
  adaptive/autodeblur/sdf) for miya face (miya_normal crop 320x300, x2),
  smiley (poor smiley x4), cat (x2) and pikachu (x2).  Outputs lossless WebP
  (or PNG when a sheet exceeds WebP's 16383px limit) into comparison_sheets/,
  plus a combined `sheet_all_four` panel.

## v4.9.11 (2026-08-01): jinc2_bilateral stepladder tuning knobs

Exposed all four shader parameters as both CLI flags and env vars so the
stepladder on flat / gradient-free (hard pixel-art) images can be tuned:

    --j2b-wa A   window A   [0,1] default .50   CELUP_J2B_WA
    --j2b-wb B   window B   [0,1] default .88   CELUP_J2B_WB
    --j2b-str S  bilateral  [0,1] default 1.0   CELUP_J2B_STR
    --j2b-ar R   anti-ring  [0,1] default 1.0   CELUP_J2B_AR

Cause of the stepladder: the bilateral range term snaps each output pixel
onto the nearest source colour, re-quantising the edge to the output
lattice.  Two effective fixes, measured on the 45-degree diagline fixture:

  - Lower --j2b-wb toward ~0.80: the strongest single knob.
    diagline jump95: 2x 0.034->0.021, 4x 0.146->0.027, 8x 0.219->0.042
    (up to ~5x less staircase).  WB=0.825 is the shader-documented
    "kills dithering" value.
  - Lower --j2b-str toward ~0.2: also reduces staircase and is the best
    overall-MAE knob (beats lanczos3 on diag/curves/axis/shallow/corner/
    parallel in evaluate_upscalers).

Note WA is *inverted* from intuition for staircase: raising WA increases
blur but also increases the negative-lobe ringing that re-quantises edges,
so raising WA makes the staircase *worse*.  Use WB or STR instead.
