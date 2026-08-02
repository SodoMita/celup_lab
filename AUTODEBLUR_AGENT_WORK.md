# Agent working notes: improving autodeblur (branch arena/019fbef6-celup-lab)

## 0. Mission (from user)

> read all docs. see how other agents on other branches tried to improve
> autodeblur. inherit all tests, comparison sheets from there. implement
> autodeblur better. never forget to push to github after each change. keep
> intermediate thought math calculations in files. improve autodeblur to
> upscale image without artifacts, staircase.

## 1. What the docs say autodeblur IS (state: master v4.9.2)

autodeblur is NOT a sharpen filter. It is a MODEL rendered at a narrower sigma:

    observed = P0 + span*phi1((z-mu)/s) + residual           (per pixel-window)
    render   = P0 + span*phi1(k*(z-mu)/s) * wS + residual*1   (anchored, gain-1)

where:
  - P0, P1 = REAL plateau colours from one side of the pixel's own lobe
  - span    = one-sided plateau span (robust), the contrast the window proves
  - phi1    = normal CDF (erf step), mu/s = lobe moments
  - z       = pixel's own position on the normal (anchored, t=0)
  - k       = steepness, capped k <= s/0.6 (output ramp >= 0.6px sigma: no re-alias)
  - wS      = trust product (strength->sharpness map, fit-rmse, multi-crossing
              count, coverage, coherence coh=lambda2/lambda1)
  - residual= pixel's deviation from the model, passed at GAIN 1 (anchored)

Pipeline: pass1 per-pixel fit -> pass1.5 contour-consensus along tangent ->
pass2 tangential delta smooth -> final convex-hull clamp (display-quantized).

## 2. INVARIANTS (must hold in ANY diff shipped -- from AUTODEBLUR_NOTES sec "FOR AGENTS")

1. Output colours never leave the local window hull (display-quantized clamp).
   Deblur has NO ringing vocabulary.
2. Residual gain stays 1 (no k*x amplification of o - model).
3. Base render sigma == -r exactly (user pins -r as the lattice-hiding blur).
   No decouple, no crisp fallback.
4. Anchored evaluation: sub-pixel mu only via tangent-consensus/averaging,
   NEVER iterative re-centering (rounds tips -- measured 83.75 < gate 86).
5. (-k/-r/-c/-p/-D/-g/-e) pinning keeps working; effective values echoed.
6. Any per-pixel multiplier of wS must work through pass-1.5 wS weighting.
7. Trust MONOTONE-safe: wS reduction may only make output closer to base.

## 3. What OTHER agents did (6 branches all == 3 commits 82b5ffc)

They did NOT touch autodeblur_pass. They added:
  - hybrid mode (classify image -> xBRZ pixelart / adaptive photo / autodeblur lineart)
  - xbr + xbrz pixel-art upscalers (celup_lab_xbr.c, celup_lab_xbrz.c)
  - "smooth" supersampled mode
  - 3x3 envelope clamp in upscale_edgecompress (used by sdf path)
  - make_example_comparison_sheets.py, tests/*_ctypes.py, generate_test_fixtures.py
  - DELETED AUTODEBLUR_GENESIS_v47.txt + AUTODEBLUR_NOTES.md (we keep ours)

They TRIED "v4.9.5 mass-conserving deblur depth" (recover line contrast via
mass-conservation f = k(wsrc+t)/(k*wsrc+t)) and REVERTED it in 82b5ffc:
"revert unstable mass-conserving depth for stable ERF profile fitting and
local 3x3 envelope clamp" -> it caused corner rounding + SDF halos.
=> Core autodeblur_pass is bit-identical to master v4.9.2.

## 4. Baseline measurements (master v4.9.2, this sandbox)

  check_corners.py : PASS  (hull ok, tip 86.25 vs 87 src, width 6<=12, glow .9774>=.88)
  check_stairs.py  : PASS  (ship2x jump95 .111, ship4x .016, probe-crisp .685 flagged)
  test_scales.py   : 204/204 PASS

## 5. The improvement queue (AUTODEBLUR_NOTES sec 2, risk-ranked)

| # | idea | win | risk |
|---|-----|-----|------|
| 1 | texture-class residual gain: in multi-cross windows (cross>2.25,
|   |   wS=0 now) amplify HF residual (~1.15) hull-clamped | crosshatch
|   |   texture, zero change on edges | LOW (proven by knob-off matrix) |
| 3 | multi-scale mu consistency (fit .5x + 1x, accept if agree<.25px) | kills
|   |   mu outliers at low contrast | MED |
| 4 | SNR-bounded k (cap k so riser < 2*noise sigma) | cleaner flats hi -g | MED |
| 6 | tiled/streamed passes (~92 B/px peak) | big images | LOW (eng) |

ALREADY TRIED+REJECTED (do NOT re-litigate):
  base-sigma decouple (stair treads, USER-rejected), mu refinement loop (rounds
  tips), mu-spread steepness governor (fires only at bending edges), deconv/RL/
  USM anywhere (ringing vocabulary), MAE-chasing synthetic scenes (rewards
  source quantization reproduction), xBR for smooth gradients (234x worse MSE).

## 6. My plan (ordered, each a separate commit+push)

STEP A (infra inheritance): bring over make_example_comparison_sheets.py +
      adaptive max-mib + the 3x3 edgecompress envelope clamp (these are
      additive and do not touch autodeblur_pass). Build, run gates, push.
      -> keeps the "inherit tests/comparison sheets" part.
STEP B (baseline regression harness): add tests/autodeblur_regression.py that
      captures a numeric fingerprint of autodeblur on the torture fixtures
      (HG amplitude, MAE, edge sharpness, staircase jump95) so every diff is
      measurable against a fixed reference, plus an image-diff for eyeballing.
STEP C (the improvement): implement queue #1 -- texture-class residual gain in
      the multi-crossing windows, hull-clamped, gain-1 safe on edges. This is
      the one proven-low-risk win the maintainer explicitly left open. Then
      evaluate stairs/corners/scales + the regression harness.
STEP D (staircase): analyze where residual staircase remains (base vs deblur),
      and if a SAFE smoothing within the deblur's tangential pass (already
      junction-aware) can reduce jump95 without losing corner sharpness.

## 8. RESULTS of the investigation (measured, not assumed)

### What was tried and what shipped

| idea | mechanism | measured effect | verdict |
|------|-----------|-----------------|---------|
| tighten multi-crossing gate | CELUP_CROSSGATE sweep | **NO CHANGE** on torture HG (crosshatch/rings/diag/corner byte-identical across gate settings). Confirms handoff: texture HG comes from the autoblur BASE + legitimately-steepened single lines (cross=2, full trust), NOT from the gate's faded region (already wS=0). | gate already optimal -- kept as env knob, default v4.9.2 |
| texture-residual gain (queue #1) | hull-clamped laplacian crispening in model-off windows | IMPROVES MAE on all torture scenes (checker2 .088->.082, crosshatch .150->.149); REDUCES checker2 HG (.0031->.0024); but RAISES diag/corner HG and **AMPLIFIES SALT** (rampnoise HF .00314 base -> .00390 at tg=0.20). coh gate does NOT stop it (dither-on-gradient reads as high-coh). | SHIPPED opt-in `-T`/`--texgain` (default OFF) -- for explicit clean-lattice content; unsafe as default on diffusion art |
| wider tangential span T | CELUP_TSPAN sweep | reduces quick jump95 but ROUNDS CORNERS (tip 86.25->84.25 FAIL) -- re-litigates the v4.9 gate | default unchanged; env knob kept |
| extra straight-contour tap | CELUP_STRTAP, coh>0.88 only | corners PASS but authoritative check_stairs ship2x 0.111->0.154 (AMBIGUOUS/worse, still passing) | default OFF; env knob kept as experimental |

### Is the deblur even ADDING staircase?
On diagline48 (45-deg line, ship2x -r 6 -g 64), staircase jump95 (quick metric):
  nearest2x = 1.00 (pure jaggies) -> autoblur base (-r 6) = 0.318 ->
  **autodeblur = 0.210**.  The deblur REDUCES staircase vs its own base via
  tangential averaging. autodeblur makes diagonals SMOOTHER, not staircased.
  The residual 0.11 (authoritative gate) is the base's lattice footprint at the
  user's chosen -r; lowering it further rounds corners.

### Default behaviour is BYTE-IDENTICAL to master v4.9.2
All new code paths are gated by env vars (default OFF) and texgain defaults to
0, so default autodeblur is bit-identical to master on step48/cornerstar48/
huearc48 (verified with cmp). Gates: check_corners PASS, check_stairs PASS,
test_scales 204/204 PASS.

### Why no default core change shipped
The autodeblur core is at a measured local optimum: every lever either (a)
does nothing (crossgate), (b) trades MAE for salt-amplification (texgain), or
(c) re-litigates a gate the maintainer tuned and explicitly warns against
re-opening (T-span rounds corners; base-sigma decouple returns treads). The
honest, safe contribution is the opt-in `-T` (queue #1, for clean lattice
content) plus the measurement infra (regression harness, comparison sheets,
env knobs) that makes the NEXT improvement measurable.

## 9. v4.9.3 SHIPPED (this branch)
- `-T, --texgain G` (0..1, default 0): opt-in hull-clamped lattice crispening
  (AUTODEBLUR_NOTES queue #1). Gated to model-off + directed (coh) windows.
- env tuning knobs (default = v4.9.2 behaviour): CELUP_CROSSGATE=off,span,
  CELUP_TEXGAIN=g (also set by -T), CELUP_TSPAN=mult, CELUP_STRTAP=1.
- make_example_comparison_sheets.py (inherited + hardened).
- tests/autodeblur_regression.py + autodeblur_regression_MASTER_v492.txt ref.

## 10. Math scratch (kept here as required)

### anchored evaluation identity (why gain-1 is safe)
  out = F_k(0) + (o - F(0))
  Let o = F(0) + e   (e = pixel's residual to its own fit).
  out = F_k(0) + e  -> the steepened fit value + untouched residual.
  d(out)/d(e) = 1  (off-curve texture passes unamplified).  GOOD.
  v4.7 did nu = phi1(k*phi1^-1(u))  -> d(nu)/du = k at centre -> e amplified
  k-fold -> outstanding mid-gradient pixels + phi^-1 halo.  Anchored kills it.

### anti-realias cap
  output ramp sigma = s/k; floor s/k >= 0.6px  =>  k <= s/0.6.
  0.6px sigma ~= 1.5px 30%-width ramp (2.5*sigma).  Below that -> new sawtooth.

### multi-crossing gate (queue #1 target)
  cross count = hysteresis mid-level crossings over the WHOLE window.
  step=1, line=2 -> full trust; 4+ -> wS *= (1 - ss01((cross-2.25)/2.5)) -> 0.
  In those dense windows the pixel currently keeps its base colour (identity).
  queue #1: add a SMALL hull-clamped HF residual gain there so the lattice
  texture (crosshatch) is recovered without re-steepening any single edge.

### mass conservation the other agent tried (recorded, NOT reusing)
  Their f = k(wsrc+t)/(k*wsrc+t), t=2.83 sig_src.  Self-limiting depth recovery.
  Reverted: it rounded corners + halod sdf because the coherence gate (>0.85)
  disabled restoration exactly at wedge tips, and lowering it admitted T-junctions.
  We avoid contrast-recovery entirely; we only touch the HF residual channel.
