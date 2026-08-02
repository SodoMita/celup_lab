# Polar artifacts (cardinal-point circle deformation) — diagnosis

User: "On circle there is rect of pixels on top, bottom, left, right, and these
are blurred with washed away colors spreading away to all sides of source.
These are polar artifacts. Halo ends on them."  i.e. at the 4 cardinal points
(top/bottom/left/right = polar 0/90/180/270 deg) of a circle, the deblur
deforms the edge (a flat/notch "rect") with a washed halo.

## The artifact, measured (direction-independent)

A clean circle (radius 107.5 out-px) upscaled 4x.  Metric = subpixel mid-level
crossing RADIUS at cardinal (0/90/180/270) vs diagonal (45/135/225/315) angles,
sampled by fine (0.25px) isotropic bilinear rays — direction-independent, unlike
a naive band-width scan (which is biased on sharp edges).

| mode | card_R - diag_R (px) | meaning |
|------|----------------------|---------|
| bilinear | +0.19 | cardinal bulges OUT (classic) |
| autoblur | +0.02 | isotropic |
| autodeblur -r1.5 -g64 | -0.14 | cardinal pulled IN slightly |
| autodeblur -r6 -g64 | **-0.77** | cardinal pulled IN hard (visible) |
| autodeblur -r6 -g1 (no steep) | **-0.005** | ISOTROPIC |
| autodeblur -r1.5 -g1 (no steep) | +0.02 | isotropic |

## Decisive isolation

- With steepening OFF (-g 1, k=1) the deblur is **isotropic** on circles
  (-0.005 / +0.02).  So the framework (anchored eval, lobe map, consensus,
  hull clamp) is NOT the cause.
- With steepening ON (-g 64) the cardinal points are pulled **inward**, and the
  pull **scales with -r** (the analysis window): -0.77 at r6 (R=30), -0.14 at
  r1.5 (R=5).  Roughly linear in R.
- => the polar artifact is the STEEPENING shifting the consensus edge position
  (mu) inward at axis-aligned (cardinal) curve points, where the wide window
  samples the circle's curvature asymmetrically vs diagonal points.

## What was tried and RULED OUT (each measured on the real radius metric)

- gradient kernel: Sobel -> Scharr (isotropic): no change.
- normal line sampling: bilinear -> 2x2 isotropic box: no change (softens).
- rmse trust band (CDG 0.055..0.10, 0.08..0.13, 0.09..0.14): no change.
- consensus + pass-2 isotropic box sampling: reduced the (biased) band metric
  but only by softening; the radius metric is the truth.
None moved the real radius metric, because the inward shift is the steepening
acting on a consensus mu that is biased at cardinal points by the wide window
sampling the curvature.

## Resolution

The pull is proportional to -r.  At **-r 1.5 it is -0.14 px (sub-pixel, same
league as autoblur's +0.02) — effectively gone.**  -r 6 (-0.77 px) is what
makes it visible, and -r 6 is also the cause of the grey ink wash and the worst
staircase.  So `-r 1.5` resolves all three at once.

A deblur-level fix (isotropic steepening / curvature-corrected consensus mu)
is a redesign of the consensus edge-position estimate, not a gate/knob — the
softening trade-off of isotropic sampling and the corner-rounding trade-off of a
smaller tangent span both make the naive fixes worse overall.  Not faked.
