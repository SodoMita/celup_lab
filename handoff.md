# Handoff: `celup_lab` upscale/hourglass investigation

# v4.9.6 update (2026-08-01): veil root-caused (s-underread) + skirt transport shipped

- Trigger: external review of the miya/smiley sheets ("halos, mush,
  bleeding, inconsistent sharpness").  Smile r6 veil decomposed by
  distance bands: 26.8 @2-4px, 11.2 @4-6, 6.4 @6-10, still 2.1 @10-16
  (NN ~1.9 at 2-4).  Two carrier populations measured: (a) pixels the
  model saturates BRIGHT but whose gain-1 residual keeps the tail --
  the |du|-moment s reads ~4 out-px while the base skirt runs
  sigma ~10-12 out-px (two-scale profile), so `(nu-ufit0)*d2` pays
  ~0 out there; (b) pixels the model saturates DARK (nu<.12, HALF the
  veil band) -- the wash trough is mis-owned as ink interior, only
  resolvable by VALUE gates.
- Shipped: value-gated skirt transport (peel): saturated claims
  sample ~2.6s farther along the normal; bright claims require
  non-darkening taps (along d2), dark claims require |z0|>1.2 AND
  inn<.6 (the washed-inner membership; ink rims keep inn~.8-1, trough
  fog reads .4-.55) AND strictly brighter taps AND s>2.  Targets are
  observed base colours, hull-clamped; nothing is DC-spread (the
  terrace pass is DC-exact and provably cannot remove veil mass).
- Numbers: r6 halo 17.4 -> 14.8, ink .949 -> .942; r2.3 halo 4.75 ->
  3.35, ink .969 -> .971, darkmean 20.6 -> 16.0; diagline GT MAE
  15.26 -> 13.37; ship4x jump95 .023 -> .001; all gates green.
- Dip/line class built (vec-to-centre field + zstar LUT width
  inversion + pass-1.5 dip evaluation) but OFF by default
  (CELUP_DIP=1 to test): in multi-feature troughs the chosen flank
  pair gives garbage widths (point-line floor).  Next honest step:
  tail-calibrated sigma (two-scale LS at consensus level) -- it is
  also what the anchored removal needs to chase the +-20 px tail
  directly instead of by peel.
- Open (updated): cusp/radial tip class; comb-teeth in dense miya
  lash zone; wand-smoke alpha-fringe chroma junk (pre-existing);
  residual narrow-gap fog where NO brighter tap exists within 2.6s
  (needs the sigma fix, not more peel).

# v4.9.5 update (2026-08-01): mass-conserving deblur depth; mouth doubling fixed; tip anatomy

- User on v4.9.4: "improved color restore, but more artifacts, miya
  mouth vertically doubled, line tips are rounded too much. So far
  autodeblur either produced pointy snake tongue lineends, or rounded
  linends, no deblur that would preserve shapes."
- Mouth doubling diagnosis (out-px DBGS columns at the mouth): true
  structure is a 2-3 src px lip band, depth 125-173 on skin 19 -- but
  v4.9.4's saturating 1/att claim pinned it to 255-black, paid as TWO
  rails (each flank frame pays its own claim) with a bright seam at
  ufit~.5 where neither side gate applies.  The dip-centre seam and
  the overclaim are one bug: depth was extrapolated from "assume the
  dip is saturated", not from the measured ink.
- Mass-conserving depth (the user's "accumulate colors back"
  literalised): blur conserves deficit mass; box+gauss dip model pins
  `f = k(wsrc+2.83 sig_src)/(k wsrc+2.83 sig_src)` (k hoisted in
  pass 1, v4.9.4).  Replaces the v4.9.4 `min(k,8)/att*q` hack (that
  one was saturated-depth leaning; the new rule is ink bookkeeping:
  dip gets back exactly what the wash holds, spread over the rendered
  width).  miya mouth 81..255 rails+seam -> single stroke peaking
  ~163 (NN 173); smiley metrics unchanged (clamped at native black
  anyway); ship2x staircase jump95 .263 -> .089 as a side-effect.
- Dip-core tent: two-sided extremum-proven dips pay the centre
  (ufit ~ .5) the min-magnitude same-sign offset component.
- Tip entry: extremum-proven + OUTER-LEVEL-SYMMETRIC flank pairs are
  admitted below the .85 coherence gate (fading to coh .3); unequal
  outsides (T-junction pairing) stay rejected.  Brow/lash taper ends
  recovered on miya.
- **Found + documented, not solved**: sharp cusp apices read NL=1 in
  the lobe map (profile has a single edge there; the coh.85/.55/.2
  gate never mattered -- the DBGR instrumentation row shows NBRANGE/
  one-lobe rejects at every apex pixel).  Cornerstar wedge apex:
  NN tip-top row 16 vs rendered 24 (2x out px) identical with and
  without the tip-entry relaxation.  True cusp preservation needs a
  radial/tip feature class (its own follow-up).
- Also observed (not chased): comb-teeth zigzag inside miya's dense
  lower-lash zone at 4x (v4.9.2 smears it, v4.9.4+ show teeth); the
  wand-smoke alpha-fringe chroma junk persists (pre-existing since
  v4.9.2, present in base too); skirt halo/veil metrics flat as
  before (17.4 r6 smiley ROI vs NN 1.9 -- still the residual-channel
  blur tail, no gate touches it).

# v4.9.4 update (2026-08-01): accumulate-mass deblur (watercolor, round 2)

- User: "improve autodeblur. currently it water colors, produce halo.
  it supposed to accumulate colors back at deblur phase."
- Diagnosis by numbers (smiley 2x ROI rows 240-500; ink = total
  darkness vs NN = 1.0; halo = mean darkness in the 2-4 px band
  outside the NN line mask; darkmean = mean darkness inside the
  mask): v4.9.3 claimed the right plateau but paid it only at
  `ufit0 > .78` (unsteepened model); narrow -r 6 strokes are ~2.9
  sigma wide so interiors read .6-.9 and stayed at the attenuated
  plateau.
- nu-based membership tried and REVERTED: phi1(k*z0) saturates at
  |z0| > 2.5/k -- no side truth; washed skirts were painted black
  (diagline GT MAE 13.7 -> 17.6; inside-vs-outside positions e.g.
  mouth (250,416) ufit .66 pay vs diag (54,64) ufit .37 don't-pay
  confirmed ufit0 keeps the side information).
- Fixes shipped: gate centre .78 -> .70 (scan .55/.60/.65/.70/.78:
  smiley ink .929/.924/.919/.912/.900, darkmean 227/226/225/223/220,
  diag MAE 16.6/15.9/15.1/14.2/14.3 -- .70 best joint); mass-correct
  restore depth `max(1/att, min(k,8)/att * wsrc/(wsrc+2.83*sigma))`
  gated att < .85, cap 4, with pass-1 k hoisted above the restoration
  block; full-strength payment where nu + uf + gInn are all
  saturated (was blurred consensus weight ~.5-.7).
- Net: smiley r6 ink .879 -> .912, core darkmean 215 -> 223, halo
  17.8 -> 17.6 (unchanged); diag MAE 14.2 vs 13.7 base; r 2.3 recipe
  unchanged; check_stairs / check_corners / test_scales (incl. miya
  face sweep) green.
- OPEN: the halo/veil (3-17 darkness in the 2-4 px skirt, NN ~2) is
  untouched by every gate tried (consensus-blur radii, uf centre,
  f_mass, k-cap): it is the base render's gaussian tail minus the
  partial erf-swap correction, surviving through the anchored-eval
  residual channel far from evidence (wS ~ 0 keeps the wash).  Needs
  real mass TRANSPORT (skirt -> core), not another rescale.

# v4.9.3 update (2026-08-01): restored parameters honesty, narrow-line washout, terraces

- Root cause of "ignoring parameters": the sawtooth cap
  `k = fminf(k, s/.6f)` makes `-g 16`/`-g 64` bit-identical while the
  report echoed the request.  The report now prints the effective
  applied range (`effective-k=6.54..12.32 avg ...` on miya 4x).
- Narrow-line washout at `-r 6` ("smiley mouth fades to grey"):
  per-window flank-pair amplitude restoration, heavily gated
  (coherence, flank extremum + direction-consistency tests,
  base-model saturation membership `uf0/uf1`, value-membership
  `gInn`, 2.25x cap, source-range clamp, wS2 evidence + PF
  consensus blur).  Barcode/neon failure modes seen en route:
  per-pixel trust flapping; mirrored-frame rel mixing; off-feature
  saturation claims -- all documented in-code.
- Terrace cleanup for quantized sources (45-level smiley class):
  `estimate_qconf` + normal-direction plateau smoothing in pass 2.
- Verified: tests/check_stairs.py, check_corners.py, test_scales.py
  (incl. miya face sweep once `tests/miya_normal.webp` is copied
  from images/), plus vs-master renders on the real assets:
  smiley r6 mouth dark-mean 108 -> 56 and min 50 -> 0 (native black),
  r23 visual parity, miya 4x MAE 4.29 -> 2.12 vs base.
  Synthetic diagline probe 15.2 vs 14.5
  base (marginal, GT asks sub-blur-line recovery).  Open: r6 eye
  ring minor unevenness on the smiley (thin annulus class).

# v4.9.2 update (2026-07-31): `-D remake` alias; crosshatch delta root-caused

- `-D remake` is now accepted as an alias of `remap` (the user's own
  miya recipe log spells it that way; previously a hard "Unknown
  deblur method" error).
- The v4.9.1 crosshatch HG delta (.00567 vs v4.9 .00434) was chased
  with knob-off builds: coverage gate off = no change; ls-clamp block
  off = .00614 (the clamps HELP, they are not the cause); v4.9-exact
  clamped-u lobe weights = no change.  The whole delta is the BASE
  sigma: v4.9 fit on the sigma-r base scores .00602, the v4.9.1 fit
  on the v4.9 decoupled base scores .00398 (best of the four
  combinations).  I.e. the v4.9.1 fit is strictly better than v4.9's
  on BOTH bases; the decoupled crisp base buys lattice-texture
  fidelity with exactly the visible-tread artifact the user rejected.
  Monotone-tread merge idea withdrawn: it would re-litigate that
  trade.

# v4.9.1 update (2026-07-31): decouple reverted, shading-aware fit, staircase gate

**User rejected v4.9** (same smiley, same recipe): "You returned all
errors.  On that smiley no neon now just because there is not enough
blur now.  -r 6 was because that was the minimal blur when stairs no
longer been visible.  Also there are visible pixels now, returned
problem of snake tongue lineends."  + mandate: "Add test of staircase,
that uses 45 degrees line and detects on final image if there are
staircase".

Root-cause chain (each step measured on the smiley at the user recipe):

1. The v4.9 decouple (base at sigma r/min(K,8)) makes the fallback
   base CRISPER than the lattice-hiding blur the user pins with -r:
   staircase treads on every 45-ish contour, per-tread speckle,
   forked caps.  REVERTED: base again renders at sigma = -r.
   => the neon fix had to move from the blend into the fit.
2. Clamped-u projection kills ramp tails whenever the window is
   narrower than the blur (-r 6): plateau means latch mid-skirt
   greys, lobe weights degenerate.  Added the unclamped `raw[129]`
   channel; lobe weights from raw; saturated-but-flat plateaus still
   hard-break lobes (thin-line flanks stay apart; otherwise the raw
   weights merge them into one window-wide lobe and tips round off).
3. A pure erf cannot represent step-on-linear-shading: fitted on
   shaded art (baked-in mouth/neck gradients) the step amplitude
   absorbed the shading and the render drag flat-topped whole
   gradient zones to plateau colours -- the ACTUAL "neon" mechanism
   (dark band hugging lines, transect dip 0.21 vs shading 0.45-0.65).
   Fix: LS profile `y = a + c*z + b*phi1(z)` on raw; shading passes
   through the gain-1 residual, only phi is steepened.
4. Drag amplitude = locally proven ONE-SIDED PLATEAU SPAN (v4.8
   estimator), clamped to [ab_b, 1.2*span] as a net: the 3-param LS
   phi/linear basis is near-degenerate on wide soft ramps, reading
   ~.7x span at tips (rounds them) and >2x on shaded skirts (neon).
   With span = v4.8/v4.9's fd2=d2 semantics the tip gate holds AND
   the band dies (transect is a smooth 0.43..0.54 shading ramp, one
   clean step), huearc/rampnoise unchanged.
5. Coverage gate: a step fit whose modelled span [a,a+b] is not
   straddled by the window's observed profile range fades wS to 0 --
   kills phantom-knee fits on pure shading slopes >R from any edge.

Tried and REMOVED after measurement (kept in git history, not in the
shipped code):
- step-component centroid refinement (LS then re-moments on y-cz):
  moved mu ~.3 px at tips/corners -> tip extent 83.75 < gate 86.
  Plain single LS on the raw projection is unbiased enough once the
  tails are back in the lobe map.
- pass-1.5 mu-spread steepness governor: along a bending edge mu
  varies by construction, so the governor only ever fired at
  tips/corners (k -> 1 there = rounding); on straight shaded ramps
  (its target) mu_std reads < .35 and it never engaged.

45 deg staircase gate: tests/check_stairs.py + fixture diagline48
(64x64, 4 px line, sigma .5, 45-level quantized).  Sub-pixel crossing
tracking x_e(y) on the FINAL image, robust line fit; gates tread-run
and jump95 at both user recipes (ship2x 0.111, ship4x 0.016, both
PASS) and asserts a deliberately crisp probe build (-r .5 -g 64) IS
flagged (jump95 .685).  Detector cannot silently go toothless.

Gates at ship (all from repo root):
  python3 tests/check_corners.py   hull / tip / width / glow -> PASS
                                   (tip 86.25 vs 86.00 floor, glow .977)
  python3 tests/check_stairs.py    45 deg stair treads -> PASS
  python3 tests/test_scales.py     204/204 PASS
Fixtures vs v4.9 same recipe (caps48/step48/huearc48/rampnoise48/
twoline48): MAE <= .0018, huearc sat .8505, rampnoise HF unchanged.
Miya user recipe (-r 2.3 -g 8): cheek HF .01439 (v4.9 .01686, v4.8
.01452), blush std .2036 (v4.9 .2429, v4.8 .2071).
4x torture HG: checker2 .00505, crosshatch .00567, rings .00374,
diag .00166, corner .00203 -- between v4.8/v4.9 except crosshatch
(crisp decouple kept more lattice texture there; accepted, user's
artifact verdict outranks the texture metric).

Build:
  cc -O3 -DNDEBUG -std=c99 -march=native celup_lab.c -o celup_lab \
    $(pkg-config --cflags --libs libwebp) -lm


## v4.9 update (superseded by v4.9.1 above) (2026-07-31): corner-sharp autodeblur -- junction gating, decoupled base, contour consensus

User review of v4.8 (own test image `poor smiley.webp`, 256x256 hard
pixelated drawing, 2x upscale, recipe
`--mode autodeblur -c linear -k bspline -r 6 -s 100 -g 64 -D remap`):
v4.7 problems gone, but regression: **sharp corners / line-cap tips are
rounded** ("v4.7, even with the doubled snake tongue, made sharp tips;
this may fit some art styles but it loses details"), and **the halo is
different now -- smooth, like neon**.  Reproduced bit-near-exactly
(<=1 LSB, arch noise).

Diagnosis (all confirmed with CELUP_DBG and probe transects):
1. The neon skirt is not from the steepener at all: the base
   reconstruction was rendered at the user's *assumed* blur sigma
   (`-r` 6 on a nearly unblurred source).  Every edge with wS < 1
   dissolved toward a sigma-wide smear, and each source-staircase tread
   simultaneously re-sharpened into its own lobe step => sharp bands
   floating on a wide smooth glow = "neon".  At -r 2.3 the same
   mechanism glows; only ~1.2 looked right.
2. Corner rounding: the tangential averaging (T) in the line sampling
   and the pass-2 delta smoothing both assume the contour is
   translation-invariant along its tangent.  At corners/tips that
   premise fails and the radial fit/delta is dissolved around the
   corner.  Additionally, with a heavily blurred base the *samples
   themselves* encode a rounded tip and the fit re-renders it (v4.7's
   "sharp tips" were phantom overshoot, an accident).
3. New fixture cornerstar48 (acute wedge, sigma 0.5): raw per-pixel
   fits near the apex misplaced mu 1-2 out px; anchored evaluation
   amplified that ~k into +-0.25 colour deltas, pass-2 transported them
   past the observed colour range (white pixels darkened to 0).
   NOTE/history: the first hull clamp stored *linear* lo/hi in u8 --
   0.12 sRGB = 0.012 linear = code 3, toothless precisely in the dark;
   hull codes are now display-quantized (sRGB u8 colour, linear u8
   alpha).

Fixes (model-level; no per-artifact heuristics):
- **Sigma decouple (autodeblur only)**: base render sigma =
  max(.6, assumed_r / min(K,8)) -- never wider than the sharpest ramp
  the deblur itself asserts.  Partial trust then falls back to a crisp
  source sample, never a wide smear.  `-r` keeps meaning "assumed
  source blur" for windows/gates; skipped under `-e` (escalation owns
  sigma there).  stderr reports the decoupled value.
- **Junction gating**: rho = lambda2/lambda1 of the same 4D structure
  tensor, coh = 1-ss01((rho-.10)/.20); scales the line-sampling tangent
  span (Teff), the pass tap weights, AND trust itself (wS *= coh --
  the 1D ramp model is out-of-domain at junctions; identity fallback =
  crisp source tip).  Long arcs (rings/corner torture, hair curls)
  measure rho <= .08 and keep full averaging: no wobble return.
- **Contour-consensus evaluation (pass 1.5)**: instead of rendering
  each pixel from its own jittering 1D fit, the wS-weighted fit
  parameters (mu, s, colour delta d2, plus tangent/coherence) are
  integrated along the junction-aware tangent; z/k/nu are recomputed
  from the consensus (anchored evaluation stays exact, residual still
  re-added per pixel at gain 1; P0 cancels, so only d2 is carried).
  This also eliminated v4.8's accepted +-1 LSB mu-jitter dither at the
  root: rampnoise48 HF .00214 -> .00183 (base .00117).
- **Convex-hull output clamp**: no output may leave the colour range
  observed in the pixel's own sampling window (deblur has no ringing
  vocabulary), enforced at pass 1.5 and again after pass 2 (delta
  transport), codes display-quantized.

Regression gate: tests/check_corners.py (hull / tip extent / flank
width / glow strip on cornerstar48 at the user-style recipe).  All
PASS; the v4.8 binary FAILS the hull check on the same gate.  Sharp-tip
smiley at user's recipe: pointed spikes, thin rings, no glow.

Torture 4x (v4.9 vs v4.8 vs autoblur): HG checker2 .00496/.00469/
.00428, crosshatch .00434/.00417/.00363, rings .00463/.00398/.00260,
diag .00203/.00214/.00127, corner .00297/.00184/.00175; MAE checker2
.08191/.05173/.05445 (dense checker kept crisp by design), corner
.02345/.02238/.02325, rings .05001/.04548/.04906.  Dense gated scenes
now keep source structure instead of melting it (MAE up to +.03 on
synthetic soft-truth scenes: detail preferred, HG stays sub-visible).
miya user recipe: cheek HF .00769 (sub-visible), blush texture .2429
(v4.8 .2071 -- the "watered away" complaint), g8 == g16 (s/.6 cap
binds); huearc saturation .8504; step48 transect sharper than v4.8,
plateaus pinned (.149-0.169 dark / .784-.827 bright), monotone, no
undershoot fringe; caps48 tongues stay fixed.  test_scales.py 204/204.

Memory: autodeblur estimate now 8.0 + 84.0 B/out-px (A f16 + DEL f16 +
PF f40 parameter field + LOH u8x8 + dst 4).

Rejected intermediate steps (kept for the record): per-pixel deltas +
pass-2 smoothing alone (transport overshoot out of hull at wedge
flanks); rho band .06/.25 (barely engaged anywhere -- replaced by
.10/.30); linear-quantized hull codes (toothless at the dark end).

# v4.8 update (2026-07-31): anchored autodeblur -- lobe-local fits, float steepness to 64

User review of v4.7: closer, but (1) "outstanding pixels at centers of
gradient", (2) "line ends look like snake tongue with split to 2
ends", (3) "halo of color surrounding line smoothly from line center
quickly ending near edge of line", (4) "looks like 2 gradients
surrounding edge are combined instead of not going further than each
other", (5) requests: float steepness ("can be much higher than 8"),
"making deblur fit original colors with gradient change", and NO
per-image-part heuristics (they introduce inconsistency).  Best recipe
for the miya image at the time:
`-m autodeblur -r 2.3 -s 100 -g 8 -D remap -c linear -k bspline` (4x),
"but still with watered away colors".

All symptoms root in v4.7's design, reproduced on purpose-built
fixtures (tests/make_test_sources.py: caps48, twoline48, huearc48,
rampnoise48):

- v4.7 evaluates nu = Phi(k * Phi^-1(u_px)): colour-domain mapping.
  d(nu)/du = k at ramp centre -> off-curve pixels amplify k-fold
  ("outstanding pixels"); Phi^-1 explodes near plateaus -> flat noise
  becomes a colour halo hugging a line from its centre, dying at its
  edge; one window-wide fit spanning a thin line averages BOTH flanks
  and BOTH backgrounds into a phantom step whose midpoint sits at the
  LINE CENTRE -> pixels get sorted to the wrong plateau ("2 gradients
  combined"), flipping at caps ("snake tongue"); and every output is
  snapped onto the 1D colour segment, deleting the perpendicular
  colour component ("watered away colors").

v4.8 changes (autodeblur_pass; sampling unchanged):

1. LOBE MAP: |du| along the normal segmented into transition lobes
   (runs above max(.06*wmax, .004), single-sample dips bridged).  The
   pixel is assigned its nearest lobe; P0/P1 plateaus are one-sided
   means clipped at neighbouring lobes; moments mu,s over the lobe
   only.  The PULSE model is deleted: a line interior pixel assigned
   to a flank lobe saturates to plateau+residual = identity, and the
   (CA+CB)/2 phantom-BE concept is gone entirely.
2. ANCHORED EVALUATION: out = F_k(0) + (o - F(0)) (remap), or
   F(0 + s*(ufit-.5)(k-1)*1.5) + residual (push).  On-curve pixels
   steepen exactly by k (identical to v4.7's z-map there); off-curve
   deviations pass at GAIN 1: no amplification, hue/alpha texture
   kept.  Misassignment degrades to identity, never artifact.
3. FLOAT STEEPNESS -g 1..64 (was int 1..8); -e per-edge cap raised to
   16.  Anti-realiasing cap k <= s/.6 unchanged (output ramp >= .6 px
   sigma): on step48 g8 == g16 == g32 because the cap binds first --
   documented in --help and README.
4. PASS 2: the model colour delta (plus flat-flatten delta) is
   smoothed tangentially along the contour (binomial weights over the
   v4.7 tangent span) before application; per-pixel residuals are
   never smoothed.  Removes coherent hundredth-px fit jitter.
5. TRUST: erf-RMSE over the lobe (|du| weights) as before, and the
   beta2 unimodality gate is replaced by a window-level hysteresis
   mid-level CROSSING COUNT (step = 1, one line = 2: full trust; 4+
   fades out by 4.75) -- per-lobe fits are all locally good in dense
   texture, so the suppression signal must live on the window.

Measurements (all reproducible; fixtures regenerate via
tests/make_test_sources.py):

- step48 4x -r1.5 -g8 remap transect x=86..106: transition ~20 px ->
  ~9 px (0.180,0.192,0.227,0.306,0.439,0.573,0.678,0.749,0.784,0.804,
  0.816), plateaus pinned, monotone, no ringing; -g 8/16/32 identical
  (s/.6 cap binding).
- Forensics fixtures (4x -r1.5 -g8 remap):
  caps48: v4.7's cap-centre notch gone (visual);
  twoline48: each flank steepens against its own background (no BE
  mixing possible);
  huearc48 (fast red->cyan sweep): mean saturation base .8490,
  v4.7 .8471, v4.8 .8481 (arc kept, not chord-collapsed);
  rampnoise48 (+-6 LSB dither): HF std base .00117, v4.7-g8 .00175,
  v4.8-g8 .00225 -- see caveat below.
- miya_face, user recipe (4x, -r2.3 -s100 -g8 -D remap -c linear
  -k bspline): cheek-gradient HF std v4.7 .01759 -> v4.8 .01568
  (g16 .01581); blush local texture std .1652 -> .1677.  Spike
  line-end centre-notch visible in v4.7 removed in v4.8 -g8/-g16
  (sheet), no dark rim; -g16 safe (anchored noise stays gain-1).
- Torture 4x defaults, autodeblur: checker2 HG .00469 (autoblur
  .00428), crosshatch .00417, rings .00398, diag .00214, corner
  .00184.  MAE improved on ALL scenes vs v4.7 (checker2 .0517 vs
  .0567).  The v4.7 corner-HG .0101 phase-offset caveat is FIXED by
  lobe localization: .00184 ~ autoblur's own .00175.
- tests/test_scales.py: 204/204 PASS.  Clean -Wall -Wextra -Wshadow.

Trade-offs accepted / caveats:

- On synthetic ramps with strong injected dither the residual model
  jitter (window-moment centre noise ~.02 px, printed once at k) shows
  as ~+/-1 LSB random dither -- sub-visible at 4x zoom.  Explored and
  rejected: core-trimmed centroid (starves the width: sigma-6 reads as
  2.5 and the trust gate rejected every clean wide ramp) and Gaussian
  truncation inversion (exact for Gaussians but flapped the rmse gate
  per-pixel on window position).  Tangential pass-2 smoothing removes
  the coherent part; the remainder is random.  On REAL content (miya),
  texture passes through at gain 1 and overall HF is LOWER than v4.7
  (no k-amplification).
- checker2 HG .00469 vs autoblur .00428 (v4.7: .00402): slight
  residual steepening activity on the checker scene; MAE improved
  (.0517 vs autoblur .0544 vs v4.7 .0567).
- Pinned invisible-alpha pixels differ vs v4.7 (residual preservation
  keeps hidden RGB under alpha<=16; 0 pixels with alpha>16 differ by
  more than 128/255; appearance identical; ~2% larger lossless file on
  miya_face default).
- CELUP_DBG=x,y prints per-pixel fit internals (lobe bounds, mu, s, k,
  z0, wS, rmse) around a coordinate -- diagnostic for future tuning.
- Memory estimate for autodeblur raised to 44 B/out-px of scratch
  (A f16 + DEL f16 + DIR f8 + dst 4); the --max-mib guard accounts it.

# v4.7 update (2026-07-31): analytic autodeblur (profile-fit steepening)

User review of v4.6: "it doesn't reduce steepness, it creates a bright
line in the middle of the gradient and negates colors. I guess there
are separate color, alpha gradients instead of 4 channel vectors + 1
gradient. I guess there is still box windows like 5x5 because there
are stepladder artifacts. Can deblur be more analytical by
constructing a function of gradient that fits original then changing
slopes on that function, then sampling pixels?"

All three hypotheses verified at 3x nearest (crop docs/review_v47*):
magenta/cyan hue-inverted fringe riding edges (per-channel box-corner
snapping -- at an edge channels move in opposite directions, so
independent [lo,hi] clamps can pick a 4-tuple that is a corner of the
box = colour from the opposite side), a white SQUARE ring around the
beauty-mark dot (fixed-window value quantisation), banded transitions.
v4.7 rewrites autodeblur_pass around the proposed analytic model:

- ONE gradient direction per pixel for the premultiplied 4-vector:
  principal axis of the 4D structure tensor (per-channel Sobel).
- Sample real colours along that normal, WITH tangential averaging
  over the +-T*tan offsets (T = clamp(.75*scale,1,3)): genuine edges
  are translation-invariant along their contour, so the averaging
  erases source-lattice jitter BEFORE the fit; without it any residual
  staircase in the base gets amplified back into scallops by the
  steepening (corner-torture "greek key" reproduced and killed).
- Project onto the local colour segment (endpoints = robust means of
  line-extreme samples: REAL colours, never box corners).
- STEP vs PULSE classification by endpoint coverage, SOFT-BLENDED
  (ss01 over [.45,.65]) so no branch seams.
- Fit: erf edge/pulse via |du| moments; steepen via the pixel's OWN
  fitted coordinate z = Phi^-1(u_px): remap evaluates Phi(k*z), push
  displaces z toward the plateau.  (The naive Phi(-k*mu/s) of the
  moment fit was unusable: window clipping biases the centroid toward
  the window centre -- measured +0.79 vs true +2 offset on the
  synthetic ground truth -- which silently damped steepening to x1.1.)
- Reconstruct as convex mix of the two segment colours: hue-safe,
  alpha coupled (rgb <= alpha by convexity), no overshoot; k capped so
  output sigma >= .6 px (no re-aliased sawtooth); separate output
  buffer (no scan-order coupling).
- Trust gates: fit-rmse (full <= .03, none >= .10) AND step
  unimodality (flatness m4/s^4: clean ramps 2.0..2.6, multi-crossing
  ~1.4, closed below 1.7).  Rationale: rmse alone cannot separate a
  windowed WIDE clean ramp (its erf fit is imperfect exactly like a
  double crossing) from crosshatch -- the flatness gate does.
  (Debug override: CDG=lo,hi env.)

Verification:
- synthetic gaussian step (tests/step48_src.webp, regenerated by
  make_test_sources.py): ramp 30%-transition ~20 px -> ~12 px at
  -r 1.5/4x/-g 8, plateaus pinned, no ringing; transects in the commit
  history.
- miya_face 4x -r3 -g8 (user recipe): v4.6 midline/boxy halo/fringe
  gone, edges hue-consistent; mid-transition pixel share .776 (base)
  -> .728 (v4.6) -> .665 (v4.7 remap) i.e. stronger AND clean
  narrowing; badge/eye crops in docs/review_v47_final.png.
- default 4x anime look preserved side-by-side vs v4.4-4.6.
- torture (default -s 2), autodeblur HG: checker2 .0040 (< autoblur
  .0043), crosshatch .0060, rings .0050, diag .0027, corner .0101;
  MAE <= autoblur on crosshatch/rings/diag.  Corner HG sits on the
  white V band as UNIFORM phase offset (HG95 == HG -- no localized
  texture) of the fit regularization, present even at -g 1; the
  visible scallops there were lattice jitter, killed by tangential
  averaging.  tests/test_scales.py 204/204 PASS; clean -Wall -Wextra
  -Wshadow build.  autodeblur outputs change everywhere by design
  (no bit-identity with v4.3..v4.6).

Caveats: HG on the synthetic 'corner' scene stays elevated (~.01) as
documented above (regularization phase, not texture); at aggressive -g
on busy multi-strand texture the trust gates soften but faint banding
may persist.

# v4.6 update (2026-07-31): sigma-aware deblur gate (wide-blur unblur fixed)

User report with exact command:
`miya_facehalf.webp -> 4, --mode autodeblur --max-mib 2048 -D push -c exp
-k bspline -r 3 -s 100 -A 10 -g 8` -- "blur is intentionally wide to
remove pixel stepladders, supposed to be unblurred but no."

Forensics (v4.5 binary): on the sigma=3 base the deblur differed on only
4.7% of pixels, strong-edge slope x0.99 -- inert despite k=8. Causes:

1. Gate blindness. The v4.3 gate (rel = |grad|/range over a 1.25-src-px
   window, smoothstep [.08,.18]) opens only for implied ramp widths
   1/(2*rel) in ~[2.8, 6.25] output px. sigma=3 ramps are ~30 output px
   wide (measured median implied width 19.9 px on miya lineart) -> gate
   w p50 = 0.0 -- intentionally blurred edges are classified as smooth
   shading at any k. Same bug hits high scales at default sigma: the
   fixed-px gate equals <1 src px at 8x+, steepening nearly shut.
2. Push quantisation in the wide regime: (u-.5)(k-1)*1.6*scale = 22.4 px
   at k=8/4x overshoots the R=15 window, so the range clamp collapsed
   samples to window extremes.

Fix (v4.6): when scale*max(1,fitted_sigma) > 4 the pass switches to a
sigma-aware branch: window widens with sigma (cap 64), gate compares
implied ramp width range/(2|grad|) against the fitted gaussian ramp
width (open <= .53*2.5*sigma*scale, closed >= 1.05x that), push
displacement is capped to +/-R. At scale*sigma <= 4 the v4.3 formulas
run bit-exactly -- default 2x..4x outputs, the tuned miya/badge looks,
-adaptive/-sdf/-autoblur paths all byte-identical (verified on an
8-combination matrix incl. -e 1.5, -D push -g 6 pins).

Verification of the reported case (miya_face 320x300, 4x, sigma 3, -g 8,
-c exp -k bspline): strong-edge mean slope x0.99 (v4.5) -> x1.44 (push)
and x1.88 (remap); max slope unchanged (no new extrema -> no halos, same
range-clamp argument as v4.3); 1:1/2x-nearest crops show hair boundary,
bang tips, beauty mark, eyebrow going from ~10 px gradients to defined
edges with zero fringing (docs/review_v46_wide_gate.png, gitignored).
tests/test_scales.py full sweep PASS; hourglass_metric autodeblur rows
unchanged at torture scale (diag HG .00142, checker .00589, corner
.00244 = v4.4 numbers).

Caveat: in the wide regime the gate opens on any bounded-width
transition, including soft shading feet (blush shoulders): at -g 8 those
may reshape slightly -- inherent to "unblur everything this wide";
lower -g or smaller -r narrows the reach.

# v4.5 update (2026-07-31): manual pins for every auto parameter; complete help

User question: "can these params be set manually? in help there is no
list of all possibilities."

Audit answer: the autoblur/autodeblur run-time parameters (kernel,
sigma, curve, curve param) were already pinnable individually since
v4.2 (`-k/-r/-c/-p`; the fit skips pinned dimensions), method since
v4.4 (`-D`) -- but the help never said so, and `-r` was mislabelled
"*blurcompress modes" only. Steepness was only indirectly reachable
through the `-s` formula. v4.5 closes all of that:

- `--help` rewritten: all 23 modes enumerated, every option with its
  complete value set and defaults, plus a "what is automatic / how to
  pin it" table mapping each auto-chosen parameter to its flag.
- NEW `-g, --deblur-steepness K` (1..8, 0=auto): exact autodeblur slope
  multiplier. Priority `-g` > `-e` per-edge adaptation > `-s` formula;
  `-e` still steers the blur fit when combined with `-g`.
- Manual is authoritative: `-e` sigma escalation now skips when `-r`
  pins sigma (one stderr note); full pin (`-k -r -c -p`) skips the
  validation fit entirely and reports `autoblur manual: ...`.
- Effective values are echoed (stderr + Done line now includes
  method/steepness for autodeblur), so an auto run can be replayed
  exactly by pinning its reported numbers.
- `-d/--adaptive-debug` is a bitmask; bit 8 (line class) existed in the
  renderer but the parser capped at 7 -- range fixed to 0..15 and the
  bits documented in help.
- No default change: v4.4 and v4.5 binaries are bit-identical on
  autodeblur 2x/4x(-e 1.5), autoblur, adaptive, sdf spot checks; scale
  sweep 204/204 PASS; clean -Wall -Wextra -Wshadow build.
- Verified interactions: `-e 2 -r .5` (escalation skipped, manual
  sigma), `-e 2 -g 5` (blur still goal-escalated, steepness pinned),
  `-g 9` rejected with exit 2. Visual: miya face strip at 8x
  `-g 1 / auto / -g 8 -e 2` -- monotone soft->crisp, no halo fringes
  at max steepness (review/mf_steepness.png, gitignored).

# v4.4 update (2026-07-30): edge-width goal, auto-chosen deblur methods

User feedback: "autoblur chooses not enough blur to get rid of sawtooth
edges, and not much is left for deblur to do; implement the other deblur
methods found and auto-choose the best; autoblur alone may be fine but
tunable goal values would be good, usable in autodeblur to increase blur
towards smooth edges first of all."

What shipped:

1. `--edge-goal W / -e W` (src px, default 0 = off).  New robust width
   metric: per strong-edge pixel, width = (range over a +-1.5-src-px
   window) / (Sobel slope), 30th percentile; calibrated so a gaussian
   ramp of sigma s reads ~2.5 s (its real AA width).  Enforcement is
   POST-FIT at target resolution: while width30 < goal and sigma < 2.5,
   sigma *= 1.35 and re-render.  Two earlier designs failed and were
   discarded, which is worth recording: (a) a validation-score penalty
   never moved the pick (MSE landscape gaps >> penalty), and (b)
   validation-side width goals are miscalibrated because the 2x
   downsampled train image already carries widened edges, so "goal met"
   at validation did not translate to the full-res render.  badge 8x
   -e 2.0 escalation trace: sigma .50 (1.37px) -> .68 (1.61) -> .91
   (1.84) -> 1.23 (2.15 OK).  miya 3x -e 1.4: fit untouched (1.79px
   already) -- the goal only spends blur where needed.  Same value feeds
   autodeblur's steepness (adaptive k per edge = width/goal, clamped
   [1,3]): mushy transitions steepen, goal-met edges keep their shape.
2. `--deblur-method auto|remap|push / -D` (default auto).  The v4.3
   research produced two guarded steepeners; both are in, and the best is
   chosen per image by the self-supervised 2x-downscale proxy MSE, same
   criterion as the blur fit: (1) remap = monotone value remap inside the
   local range (closed-form Anime4K); (2) push = spatial gradient push
   along the unit gradient scaled by normalised position (Anime4K's
   actual mechanism), local-range clamped.  Validation renders each
   candidate at sw and scores reconstruction of the source; miya chose
   remap (.0005225 vs .0005313; both beat the plain autoblur proxy
   .0005251 -- measured proof the deblur recovers signal, not just
   cosmetics).  Shock filters remain rejected (staircasing is the input
   problem here, not the solution).

Numbers (defaults, -e off = v4.3 behaviour): evaluator MAE table
identical to v4.3 (autodeblur == autoblur to 1e-4); HG unchanged; scale
sweep PASS (192 checks, 12 modes incl. autodeblur, scales 1.5-24x +
32x11 face strip).  Visual: badge inner corner at 8x -- nearest
staircase -> autoblur e0 faint steps -> autoblur -e2 smooth arc ->
autodeblur -e2 smooth AND crisp (sdf-grade corner, but by monotone
remap, so no halo class anywhere).  Thin-line caveat: goals >= ~1.5 src
px soften true 1px line-art by design; -e stays off by default.

# v4.3 update (2026-07-30): autodeblur -- gradient-slope steepening for AI-upscaled art
# v4.3 update (2026-07-30): autodeblur -- gradient-slope steepening for AI-upscaled art

User context: the miya art is diffusion-generated and ALREADY internally
upscaled by the generator; it arrives with mushy AA, diffusion salt and
lossy block boundaries that must be CLEANED during upscale. autoblur is
their favourite base ("better than triangle: consistency + detail
preserving"); adding artifacts at all is unacceptable; they asked for an
"autodeblur that doesn't use 5x5 patches, instead analyzes gradients and
increases their slope, and uses autoblur".

Research (all three line up with that request):

- Anime4K (bloc97, 2019) -- iterative gradient-ascent on the colour
  heightmap: "push pixels towards probable edges ... maximizing the
  gradients ... equivalent to minimizing blur, but without overshoot or
  ringing artifacts commonly found on traditional unblurring and
  sharpening approaches"; line-detector gating so textures are untouched.
  https://github.com/bloc97/Anime4K (preprint/results), discussion
  https://news.ycombinator.com/item?id=20698721
- Osher-Rudin shock filters -- the classic gradient-slope steepener; the
  sign-hard update staircases (this is the quantization/region artifacts
  the user saw in Krita's unblur brush). Adaptive-gradient variants exist
  to suppress exactly those (J. Visual Comm. 2016).
- Anime-encoder descaling (guide.encode.moe/encoding/descaling.html) --
  fit the wrong-kernel upscale, invert to native res, rescale with a sane
  kernel; validate by re-upscaling and comparing. Our autoblur fit plays
  the descale-fit role, continuously rather than per-known-kernel.

`--mode autodeblur` design (v4.3): render the autoblur base, then per
pixel over a ~1.25-src-px window take the robust per-channel range
[lo,hi], normalise u = (v-lo)/(hi-lo), remap u' = .5+(u-.5)*k
(k = 1+.25*(s-1), clamp 3, from --strength) and write back inside the
SAME range. Monotone + range-anchored => halos/ringing/hourglass/new
colours impossible by construction. Blend weight = smoothstep(|grad|/
range in [.08,.18]): true edges steepen fully, low-relative-slope shading
(blush etc.) untouched so it cannot posterize. True flats are instead
pulled toward the window mean (range gate .008..025 pm) to mop up
diffusion salt. Premultiplied invariant rgb <= alpha re-asserted per
pixel (channel-independent remaps briefly broke it near alpha~0; put()
then quantizes to saturated junk -- 644k such px vs 1.3M in autoblur
base after the fix, i.e. BELOW the inherent straight-alpha quantization
floor of this asset).

Bugs caught by the evaluator during development: (1) ungated flat-
flatten stained a soft halo band around every line where window means
mix both sides of an edge -- range-gating removed it (diag MAE .01251
-> .01028 == autoblur); (2) the premultiplied-invariant break above.

Measured: MAE identical to autoblur to 1e-4 on all 9 scenes (sharpness
is a free addition at these k); HG identical to autoblur except slightly
higher on sharpened edges, still far under sdf (diag .00142 vs .00529;
crosshatch .00377 vs .00884; rings/corners similar dark-horse levels).
Badge rounded-rect inner corner at 8x: sdf-grade smooth rounded corner,
which was the exact sdf behaviour the user praised. No new artifact
class observed on miya 3x/10x sweep; 192/192 scale-sweep tests pass.

Verdict-for-user: on this content autodeblur is the default-recommendable
anime mode; sdf remains the max-precision geometry option; deblurcompress
remains the raw-detail option (its consistent hourglass texture read
"somewhat good" to the user -- noted as accepted there).

# v4.2 update (2026-07-30): gradient-only suppression, sdf halo guard, alpha cleanup, CLI
# v4.2 update (2026-07-30): gradient-only suppression, sdf halo guard, alpha cleanup, CLI

Driven by a new user asset (miya_normal.webp, 768x1376 RGBA chibi art,
LOSSY WebP with heavy hidden-RGB garbage under alpha~0 and lossy
semi-transparent salt in empty regions) and user feedback:

"hourglass should not apply to symmetrical inputs, only gradients;
adaptive is worse; sdf still produces artifacts/halos; triangle would
look very good if not for artifacts; edge detection never worked well."

## What changed

1. **Suppression passes are gated by local directedness, not just class
   weights.** `build_direction_gate()` accumulates the 3x3 structure
   tensor of the pm luminance proxy and returns
   coherence = sqrt((Jxx-Jyy)^2+4Jxy^2)/(Jxx+Jyy) times a gradient-energy
   reliability Jsum/(Jsum+.02). edges/ramps/line flanks -> ~1 (full
   suppression); symmetric dots, star cusps, checker phases, junction
   crossings -> ~0 (verbatim passthrough). Both remove_hourglass_basis()
   and suppress_speckle_pm() multiply this in. Result on miya 3x: the dark
   ring adaptive used to draw around symmetric coat dots is gone, sdf's
   specks at dot/hair boundaries gone; torture HG basically unchanged
   (crosshatch .00878->.00884, checker2 .00319->.00397, everything else
   within noise) -- the checker *policy* was already preventing hourglass
   formation, the suppressors were belt-and-braces that also ate real
   symmetric features. MAE table unchanged to 1e-4.
2. **sdf halo guard.** Final colour is clamped to the local 3x3 source
   envelope union the base pixel's own value (+1.5e-3 eps). sdf by design
   re-places contours inside measured endpoint colours; any residual
   overshoot (tone-conservation DC subtraction, endpoint-field slip) is
   now physically impossible, not just small. MAE delta ~1e-4.
3. **alpha_despeckle() at load** (-A/--alpha-clean T, default 10, 0=off):
   (a) alpha==0 -> rgb=0; (b) 0<a<=T isolated dust (<=2 of 8 neighbours
   above T) -> wiped; (c) isolated mid-alpha salt a in (T,160], a > 3x
   brightest neighbour + 24, no neighbour >= a/2 -> wiped. Connected
   faint glow bands and >=2px sparkles survive by construction. miya bg
   dust at 3x: 46710 -> 2241 semi-transparent pixels (nearest mode);
   adaptive 47447 -> 1557.
4. **CLI**: -h/--help with grouped recommendations; short aliases for
   every option (-m -s -r -a -P -A -k -c -p -M -d). sdf/adaptive on big
   images need -M (memory guard default 512 MiB; the 768x1376 source at
   3x already exceeds it -- error message says so explicitly).
5. **tests/test_scales.py**: sweep scales 1.5..24 (incl. >20x) x 11 modes
   on a synthetic 12x12 AA circle, plus the miya 32x11 face strip at
   2/4/8/16/22x (regenerated by tests/make_miya_fixtures.py when the user
   asset is present; skipped otherwise). Checks: exit 0, exact output
   dims, zero staircase treads, AA-band adjacent-step budget per mode.
   Current: 187/187 ok. A note on methodology: extreme-scale artifact
   testing only needs a small content window (32x11px at 22x = 704x242);
   full-frame >20x renders waste time without showing anything more.

## miya measurements (3x unless noted)

- Autoblur 10x on an 8x pm-box-reduced small (96x172): fit bspline
  sigma .50 / linear; silhouette AA-band max adjacent alpha step 0.133
  (p95 0.129), ZERO staircase treads on hat/head curves -- round, no
  sawtooth, as requested. Same scene triangle 0.098, adaptive 0.196.
- 22x face strip: autoblur/triangle render round smooth contours;
  adaptive/sdf stay pixel-faithful (visible staircase retained at 22x).
  That's intended: adaptive/sdf are the sharp family; for round output
  use autoblur/triangle.
- Residual: a faint square block boundary visible at 10x+ on flat purple
  zones is a LOSSY-BLOCK artifact inherited from the source (present in
  nearest too); sharpeners merely reveal it. triangle/autoblur hide it.
  Not a pipeline bug. (Worth a future --block-clean T?; user asked for
  nothing of the sort yet.)
- Edge-quality verdict on this asset after the halo guard: triangle is
  the cleanest look overall; sdf now matches adaptive sans fringes.

# v4.1 update (2026-07-30): SDF rewrite (plane fields, no Voronoi), speckle suppression

## Why

User feedback on v4.0: the sdf mode was "the worst algorithm here,
producing lots of artifacts". Crop forensics (CELUP_SDF_DUMP field dumps
+ loss maps on examples/*_source_96) found five artifact classes, all
rooted in the seeded-contour + Voronoi distance-transform design:

1. **Washboard banding** parallel to strong edges: Voronoi wins
   alternated from one source row to the next, rippling d along the
   contour; the large endpoint axes of distant blobs then painted
   stripes for tens of pixels.
2. **Rainbow stripes in flat semi-transparent interiors**
   (curves_alpha): sub-LSB premultiplied noise, amplified by the
   bounded-Mitchell base, was routed into smooth junction branches where
   the sdf delta still re-thresholded (confidence ~1e-12 but not 0).
3. **Phantom midline contours** inside thin features (seed-side signing
   remnants between the two flank contours).
4. **Halo pairs** straddling hard edges: the sdf contour position
   disagreed with where the base kernel put the step.
5. **Ghost extensions**: a short measured segment's plane extrapolated
   far beyond its support.

A reference SDF/MSDF implementation from another agent was also
reviewed: colour-aware Sobel edge mask -> 2-pass chamfer distance
transform -> sign from the pixel's own alpha>0.5, then *snap to nearest
neighbour* near the edge (MSDF = same with 3 direction-binned distance
channels + median combine). The DT+snap approach discards precisely the
sub-pixel contour position information the plane fit provides, so its
output is nearest-neighbour plus staircases ("same result as nearest").
Not adopted; but it out-MAE'd v4.0 sdf on synthetic diagonals
(0.00846 vs 0.01248), which confirmed v4.0 sdf's deltas were mostly
noise. The rewrite keeps the fitted sub-pixel information instead.

## What changed in `sdf` (v4.1)

1. **No distance transform, no Voronoi.** Every confident coherent-edge
   pixel of the class map (w_edge*(1-w_checker) > .35, |grad t| in
   [.12,1.4]) splats its fitted t-plane as a *signed-distance plane*
   d_k(p) = (t0 - .5 + g.(p - p0))/|g| into its 9x9 source
   neighbourhood with a Gaussian weight (sigma 1.5 src px). d, ramp
   width, endpoints A/B and confidence accumulate as weighted means: an
   edge votes for a local linear contour model, nothing global.
2. **Anti-ghost truncation**: splat weight is 0 where |d_k(p)| > 2.6
   src px, so a short segment cannot paint its plane across the image.
3. **Ramp-width de-dilution**: the 5-tap LS slope saturates at 0.3 for
   true AA ramps narrower than ~1.6 px, so naive w = 1/|g| overestimates
   the width ~2x and dilutes the sharpening. w = g>=.295 ? 1.2 :
   (1/g < 4.3 ? 1/g - 2.1 : 1/g), clamp [.6,5].
4. **[1,2,1]^2 d smoothing** + a gradient-coherence gate
   (|grad d| inside [.5,.8]) folded into conf: regions where splatted
   planes disagree (crossings, gratings) abstain instead of averaging
   into mush.
5. **Render base = full `upscale_adaptive` output**, not bounded
   Mitchell: out = adaptive + conf * (t' - t_base(decoded adaptive px))
   * axis, t' = smoothstep(clamp(.5 + d'/w', 0, 1)), w' narrowed by
   --strength (1 - 0.12*strength, clamp >= .45). sdf thereby inherits
   adaptive's entire checker/junction/flat policy: Nyquist checkers and
   crossings are *bit-identical* to adaptive, and artifact classes
   2/3/4 are gone structurally (the delta only acts where an edge was
   measured, against the same base that rendered the rest).
6. **Tone conservation**: the delta's local DC over a box of radius
   2.5 src px * scale is subtracted, but gated per pixel with
   w_k = |D_k|/(|D_k| + 0.5*mean|D|). The ungated version injected
   inverse halos into flat regions (curves_alpha max deviation 74.6 ->
   normal levels).

`sdf` hourglass/compression call sites also run the new speckle pass
(below). Perf: 2.66 s on 512x512 at 3x (adaptive 2.0 s); memory = the
adaptive output plus ~36+40 floats per source pixel of fields.

## Speckle suppression (hourglass removal follow-up)

User-suggested pattern implemented, plus one related one found on the
torture results. `suppress_speckle_pm()` runs after every
`remove_hourglass_basis()` call (adaptive .60 gated; deblur .95;
consistent .80; hourglasscompress .95/.55):

- **loner**: centre pixel deviates from all 8 neighbours by
  dev > 4*spread + 2.5e-3 (pm units) -> overwrite with the 8-neighbour
  mean. This is the requested "single pixel surrounded by another
  colour in a 3x3 grid".
- **domino**: an axis-aligned pixel *pair* whose mean deviates from an
  otherwise uniform 10-pixel ring by the same test -> both overwritten
  with the ring mean (fresh snapshot, so pairs don't mask each other).

Both are gated by w_hg (checker + .65*junction) and share the basis
remover's policy guard (skipped for nearest/scale2x bases), so genuine
thin lines survive. Measured: adaptive torture loner count 81 -> 0,
deblurcompress 5909 -> 0; MAE and HG/CHK unchanged (the pass removes
isolated specks, not edge amplitude). Other patterns were scanned for
(1px zippers along edge flanks, etc.); nothing else was safely
suppressible without eating real 1px detail, so the pass stays minimal.

## Measured results (v4.1, pm-linear RGBA)

Standard 9-scene evaluator, MAE (lower better; sdf at default
strength):

    scene     bilinear  sdf      autoblur  adaptive  deblurcompress
    diag      0.01045   0.00876  0.01029   0.00899   0.00735
    curves    0.00890   0.00636  0.00874   0.00710   0.00549
    gradient  0.00115   0.00115  0.00115   0.00115   0.00115
    axis      0.00630   0.00571  0.00612   0.00552   0.00451
    shallow   0.00374   0.00289  0.00367   0.00310   0.00237
    thin      0.01103   0.01007  0.01095   0.01007   0.00889
    corner    0.02342   0.02287  0.02325   0.02253   0.02088
    parallel  0.00652   0.00637  0.00641   0.00646   0.00540
    alpha     0.00998   0.00807  0.00981   0.00862   0.00709

sdf beats adaptive on 5/9 (diag, curves, shallow, parallel, alpha),
ties thin/gradient, trails on axis/corner (junction-dominated scenes
where sdf abstains and adaptive's junction smoothing is already good).
Versus v4.0 sdf: diag .01248->.00876, curves .01046->.00636, axis
.00822->.00571, shallow .00614->.00289, alpha .01228->.00807, corner
.02415->.02287.

Hourglass amplitude (HG, lower = cleaner), torture set:

    scene      adaptive  sdf      note
    checker1   0.00540   0.00540  sdf == adaptive (inherited checker policy)
    checker2   0.00319   0.00319  sdf == adaptive
    crosshatch 0.00878   0.00878  sdf == adaptive
    rings      0.00221   0.00337
    diag       0.00205   0.00529  sharpening delta adds some amplitude
    corner     0.00060   0.00282  back vs adaptive -- the known trade

## Remaining limitations / next ideas (v4.1)

- sdf's residual HG on sharpened diagonals/corners is the price of the
  re-threshold; the coherence gate already abstains where planes
  disagree. Next lever would be a per-pixel strength ramp from the
  junction class map rather than the global conf product.
- The de-dilution width curve is hand-shaped from the observed slope
  saturation; a learned/iterated width estimate might do better.
- Combined comparison image: `comparison_sheets/comparison_sheet.png`
  (22 mode specs x 11 scenes, 9600x4994). It is *not in git* by design
  (no image files in the repo); regenerate with
  `python3 make_lab_comparison_sheets.py`.

# v4 update (2026-07-30): SDF mode, autoblur 8x sawtooth/mosaic fix, git

## What changed since v3

1. **`--mode sdf`: signed-distance-field edge reconstruction.** Every
   confident coherent-edge pixel of the class map contributes its fitted
   t-plane's sub-pixel t=0.5 crossing as a *seed point on the edge's
   mid-contour*, plus ramp width (1/|grad t|, clamp 0.6..3 src px) and the
   two premultiplied endpoint colours. A two-pass vectorial distance
   transform (8SSEDT neighbourhood) gives every source pixel a signed
   distance to the nearest mid-contour; the d/width/endpoint/confidence
   fields are upsampled to the target grid (d/width/conf: bilinear --
   Mitchell's negative lobes ring a signed field; endpoints: bounded
   Mitchell). Output = bounded-Mitchell base + conf * (t' - t_base) *
   (B'-A'), t' = smoothstep(clamp(.5 + d'/w', 0, 1)) with w' narrowed by
   `--strength` (factor 1 - 0.12*strength, clamp >= .45). The delta form
   self-annihilates in flat regions (both t saturate), junctions/lines/
   checkers (no seeds: confidence 0), and 1.5px+ from any contour, so the
   mode can only re-shape WHERE an edge was measured. Two subtle fixes
   mattered (loss-map diagnosed):
   - **sign by query-side, not seed-side**: between a thin bright feature's
     two flank contours, seed-side signing put a phantom zero-crossing down
     the midline (dark stitch). The sign now comes from the *query* pixel's
     own projection onto the winning seed's colour axis.
   - **[1,2,1]^2 smoothing of the signed distance field** before upsampling:
     stair-stepped seed polylines relax to their chord, so the rendered
     contour is a smooth straight/curved line instead of +-1px weave.
   Checker/Nyquist-ambiguous pixels suppress seeding outright (edge weight
   is already checker-peeled; the gate also multiplies the confidence).
2. **autoblur 8x sawtooth + "blurry pixels" fixed by a renderer swap** (the
   fit stays). v3 blurred the source *grid* (sigma down to 0.15 degenerates
   to identity) and sampled with a shaped 2x2 tap: C1 seams at every source
   cell border (measured 7.1x interior d2 on a smooth gradient -> the
   "blurry pixels" mosaic), and AA staircases were tracked as hard
   one-phase steps (measured 38 steps >0.05, max 0.22, down one column ->
   sawtooth). v4 splats the *analytic* kernel at the target resolution with
   the gradient curve as a per-cell monotone coordinate warp. Kernel
   profiles carry floors that guarantee >=1 src px of real support (box is
   the trap: halfwidth 0.5 at continuous coordinates IS nearest). Selection
   adds a smoothness prior the proxy MSE cannot express (target is the
   sharp input): among candidates within 3% MSE, larger sigma / smoother
   kernel family wins, and curves pay a max-slope penalty (factor
   1 + 0.3*(steep-1)) so a near-step curve must *truly* fit the image
   (pixel art still picks it; smooth content never does). Result at 8x:
   seam/interior 0.71, max column step 0.09-0.18 spread over the full
   kernel width, fit now picks e.g. bspline/.50/sigmoid instead of
   gaussian/.15/cubic.
3. **Git workflow**: the lab now lives in a git repository (`celup_lab` at
   /home/user/lab/work); deliverables are `git bundle` files of the whole
   repo history instead of zip snapshots.

## Measured results (v4, pm-linear RGBA)

Standard 9-scene evaluator, MAE (lower better; sharpening trades MAE for
crispness by design -- v4 sdf default strength):

    scene     bilinear  sdf      autoblur  adaptive  deblurcompress
    diag      0.01045   0.01248  0.01029   0.00899   0.00735
    curves    0.00890   0.01046  0.00874   0.00710   0.00549
    gradient  0.00115   0.00115  0.00115   0.00115   0.00115
    axis      0.00630   0.00822  0.00612   0.00552   0.00451
    shallow   0.00374   0.00614  0.00367   0.00310   0.00237
    thin      0.01103   0.01079  0.01095   0.01007   0.00889
    corner    0.02342   0.02415  0.02325   0.02253   0.02088
    parallel  0.00652   0.00749  0.00641   0.00646   0.00540
    alpha     0.00998   0.01228  0.00981   0.00862   0.00709

Hourglass/parity band (HG, lower = cleaner) on the torture set:

    scene      lanczos3  sdf      autoblur  adaptive  deblurcompress
    crosshatch 0.02429   0.00524  0.00363   0.00878   0.01232
    checker2   0.00447   0.00122  0.00428   0.00319   0.00025
    rings      0.00790   0.00969  0.00260   0.00221   0.00411
    diag       0.00360   0.00399  0.00127   0.00205   0.00525

sdf is the cleanest *sharp* mode on 1px-Nyquist content (crosshatch HG
beats adaptive 0.0088 and deblurcompress 0.0123, near autoblur's soft
0.0036), and visually it is the only mode that removes 1px staircases at
8x (thin line -> smooth uniform-width stroke). On `rings` (alternating
curvature at Nyquist) it stays near base level (0.0097 vs mitchell-class
0.008) because it runs no hourglass remover -- documented; adaptive remains
the best-rounded default there. autoblur 8x metrics, before -> after:
gradient cell-seam d2 ratio 7.1 -> 2.4 (invisible: max |dy| 0.008),
wandering-line column: 38 hard steps (max 0.22) -> smooth ramp (max 0.18
spread across the ~11px kernel transition, seam ratio 0.71).

Perf (512px, x3): sdf 1.5s, autoblur 1.6s (fit included), adaptive 2.4s.

## Remaining limitations / next ideas (v4)

- sdf MAE sits between bilinear and the sharpeners: ~85% of the residual
  gap is the deliberate ramp-narrowing at edges; `--strength` widens/
  narrows it further. The contour-position estimator is locally exact
  (sub-pixel seed crossings), globally limited by the 8SSEDT chamfer error
  (~0.1px, invisible) -- a Felzenszwalb exact EDT would be the upgrade.
- sdf `rings`/curved-Nyquist: seeds there are edge-class (checker peel
  already zeroes the rest); a curvature-aware conf falloff could gate
  alternating-curvature fields further.
- sdf smooths sign-consistent corners by ~1 src px (the d-field blur);
  junction-class pixels keep the base kernel so genuine corners are
  largely spared, but a corner-preserving smoothing mask is possible.
- autoblur's fit is still MSE-based: the 3% near-tie band and slope
  penalty are hand-set priors; a small perceptual (SSIM/banding) term in
  the proxy score would make them principled.

# v3 update (2026-07-30): thin-line class, junction sharpening, autoblur

## What changed since v2

1. **Thin-line (pulse) class** in the classifier: the structure tensor of the
   5x5 t-field gives orientation coherence; the profile along the dominant
   normal must be a narrow pulse (both outer sides agree, core extremum far
   from the side level, width below ~0.6 src px). True 2D checkers fail
   coherence, true edges fail both-sides-agree. Detected lines reconstruct
   with a Gaussian pulse profile along the **nearest fitted line** (Voronoi
   selection, avoiding per-host jitter dapple), narrowed by 1/sqrt(strength)
   with a width-aware fade, between the patch's own colour-axis endpoints --
   constant along the line, so no lateral structure can appear. 1px lines
   visibly sharpened (thin MAE 0.01105 -> 0.01007, beating bilinear 0.01103);
   wider 2px lines are deliberately left to the smooth classes (sharpening
   them exposed the source staircase).
   Debug: fixed a variable-shadowing bug (`line_conf` name reused) caught by
   building with `-Wshadow`; `CELUP_CLASS_DEBUG=1` prints detection details.
2. **Junction sharpening** via relaxed damping: the gated consistency
   correction at junctions went from 5% to 35% of full weight. Crossings
   recover genuine detail with no measured artifact increase (the residual
   at junctions is sign-consistent; only alternating residuals build
   hourglass phase, and those remain gated by checker weights).
3. **Line-weight smoothing**: the peeled line weight is [1,2,1]-smoothed
   once (other classes stay per-pixel) to keep pulse rendering blocks
   coherent; gates are derived after smoothing and weights renormalized.
4. **`--mode autoblur`**: fully automatic *blur-only* upscale, fitted per
   image. Overall kernels: box / triangle / gaussian / bspline + sigma;
   gradient transition curves: linear / sigmoid / cubic / exp / log / sqrt /
   circle / nearest (all symmetric, endpoint-fixing, monotone -- no ringing
   by construction). Two-stage self-supervised fit on the 2x-downscale proxy
   (kernel+sigma first, then curve family/param). Manual pins:
   `--blur-kernel`, `--blur-radius`, `--blur-curve`, `--curve-param`.
   Selected parameters are printed to stderr and in the final report.

## Measured results (v3, pm-linear RGBA)

Standard 9-scene evaluator, MAE (lower better):

| scene | bilinear | autoblur | adaptive | deblurcompress |
|---|---|---|---|---|
| diag | 0.01045 | 0.00971 | 0.00899 | 0.00735 |
| curves | 0.00890 | 0.00823 | 0.00710 | 0.00549 |
| gradient | 0.00115 | 0.00116 | 0.00115 | 0.00115 |
| axis | 0.00630 | 0.00568 | 0.00552 | 0.00451 |
| shallow | 0.00374 | 0.00345 | 0.00310 | 0.00237 |
| thin | 0.01103 | 0.01034 | 0.01007 | 0.00889 |
| corner | 0.02342 | 0.02287 | 0.02253 | 0.02088 |
| parallel | 0.00652 | 0.00601 | 0.00646 | 0.00540 |
| alpha | 0.00998 | 0.00924 | 0.00862 | 0.00709 |

adaptive now beats bilinear on **every** scene; autoblur beats bilinear on
every scene *without any sharpening* (its checker2 scene fit hits
MAE 0.00048 via the curve family -- transition-shape choice matters).

Torture scenes, HG = hourglass-basis amplitude vs bilinear (artifact energy;
note the thin-line pulse structure is *real* detail and legitimately raises
HG versus a flat bilinear -- visually verified, see docs/*.png):

| scene | lanczos3 HG | deblur v1 HG | deblur v3 HG | adaptive HG |
|---|---|---|---|---|
| crosshatch | 0.02429 | 0.01704 | 0.01232 | 0.00878 |
| rings | 0.00790 | 0.01225 | 0.00411 | 0.00221 |

## Remaining limitations / next ideas

- Crosshatch-class inputs (1px lines crossing at double-pixel Nyquist) are
  still reconstructed soft by adaptive; deblurcompress v3 renders them
  crisply and cleanly. A crossing-pair pulse model (two superposed pulses)
  could close the gap.
- Pulse sharpening refines width only, not position: sub-0.3px line
  position error remains (bounded, invisible at 4x).
- autoblur's validation proxy needs >= 8x8 input; on tiny images it falls
  back to gaussian/0.75/linear defaults.
- Junctions other than relaxed correction remain bilinear: a segmented
  (soft-alpha cluster) junction model is the remaining invention-free
  sharpening avenue there.

---

# v2 update (2026-07-30): classifier policy layer implemented

The "Recommended next direction" from the v1 handoff has been implemented
and measured. Hourglass/bow-tie artifacts are addressed **by construction**:
sharpening can no longer fire in ambiguous cells at all, instead of being
subtracted after the fact.

## What changed

1. **Unified 5x5 patch classifier** (`build_class_map`): per source pixel it
   fits a two-colour colour axis, a spatial t-plane, and a *competing parity
   (checker) model*, plus exact 2x2 checker detection, endpoint clustering,
   side variance and neighbour flips. Output: per-pixel class weights
   (edge/checker/junction/base) + gates for the iterative modes +
   fitted-edge plane parameters (kept for diagnostics; the edge-plane
   sharpening target itself was A/B-tested and **removed** -- it lost to the
   bounded kernel on anti-aliased edges).
   Key subtlety: staircased AA diagonal edges also produce alternating
   labels. Parity must *dominate the plane model* (`checker_r2 - plane_r2`)
   before a patch counts as checker, so real diagonal edges keep their
   sharp treatment.
2. **New flagship `--mode adaptive`**:
   - base per pixel: bounded Mitchell (edges included) / plain bilinear at
     junctions / explicit checker-policy fallback in ambiguous cells;
   - then 3 class-gated consistency iterations (step .55) to recover edge
     sharpness (the mechanism that made deblurcompress score well, now unable
     to touch checker/junction cells);
   - then focused hourglass-basis removal (skipped for hard policies).
3. **`--checker-policy lowpass|bilinear|nearest|mitchell|scale2x|auto`**:
   explicit per-image intent for ambiguous cells. `auto` picks scale2x for
   pixel-art-like inputs (tiny unique-palette ratio + almost no soft links)
   and lowpass otherwise.
4. **`--mode classmap`**: classifier visualization (R=edge G=checker
   B=junction) -- verify routing on the actual input.
5. **`--mode scale2x`**: standalone generalised-scale EPX/Scale2x (exact
   palette colours only).
6. **deblurcompress / dehourglass rewired on the same gates**:
   back-projection weight `(0.12+0.88*edge)*(1-checker)*(1-0.95*junction)`,
   unsharp confined to edge territory, hourglass removal focused on
   ambiguous cells. Deblur is **~7.5x faster** (32.7s -> 4.4s on a 512px
   scale-3 test) because the 5x5 statistics are computed once per source
   pixel instead of per output pixel per iteration.
7. **`hourglass_metric.py`**: quantitative artifact metric -- per-cell fitted
   hourglass/saddle amplitude vs bilinear reference (HG/HG95), checker-cell
   MAE (CHK), and new checker1/checker2/crosshatch/rings torture scenes
   (also added to `make_lab_comparison_sheets.py`).
8. **`--adaptive-debug N`**: dev switch zeroing class weights (1=edge,
   2=checker, 4=junction) for error attribution.

## Measured results (HG = hourglass amplitude, lower = cleaner)

| scene | metric | lanczos3 | deblur v1 | deblur v2 | adaptive | adaptive auto |
|---|---|---|---|---|---|---|
| crosshatch | MAE | 0.12401 | 0.12365 | 0.14634 | 0.15080 | 0.14683 |
| crosshatch | **HG** | 0.02429 | 0.01704 | **0.00105** | **0.00108** | 0.00209 |
| rings | HG | 0.00790 | 0.01225 | 0.00355 | 0.00191 | 0.00534 |
| diag | MAE | 0.00814 | 0.00608 | 0.00729 | 0.00879 | 0.00877 |
| diag | HG | 0.00360 | 0.00696 | 0.00498 | 0.00191 | 0.00192 |
| corner | MAE | 0.02050 | 0.01864 | 0.02178 | 0.02278 | 0.02278 |
| corner | HG | 0.00551 | 0.00492 | 0.00187 | 0.00058 | 0.00061 |
| checker1 | MAE | 0.21424 | 0.20764 | 0.21424 | 0.27126 (lowpass: texture erased by design) | **0.00000** (scale2x: perfect) |

Crosshatch hourglass energy: **16-23x lower** than lanczos3/deblur v1.
On the standard 9-scene evaluator, `adaptive` beats bilinear MAE on all
natural scenes (ties on 1px-thin-line scenes, which are Nyquist-ambiguous)
while posting 2-9x lower HG than any sharp mode. The gated `deblurcompress`
keeps most of its sharpening (it remains the sharper option) with an order
of magnitude less artifact energy.

## Remaining limitations / next ideas

- 1px-wide anti-aliased diagonal lines are genuinely Nyquist-ambiguous; the
  lowpass policy softens them (MAE ties bilinear). A dedicated thin-line
  class (sharpen along the fitted line normal only, never invent lateral
  structure) could recover them; not implemented.
- Junctions use plain bilinear: safe but slightly soft around crossings.
- `auto` policy is global-heuristic based; exotic mixes (pixel art with
  heavy global AA) may need an explicit `--checker-policy`.
- Classifier thresholds are tuned on procedural scenes; for photographic
  content review `classmap` output when in doubt.
- The compress/blurcompress/consistentcompress modes are unchanged and
  remain diagnostic; adaptive/deblurcompress supersede them.
- `test_celup3.py` compares in sRGB byte space and is dominated by hidden
  RGB behind transparent pixels; numbers identical to the v1 binary
  (no regression). Prefer `evaluate_upscalers.py` / `hourglass_metric.py`,
  which measure premultiplied linear RGBA.

---

# v1 handoff (historical)

## Current status

The current code is in `celup_lab.c`. It builds and runs, but the user reports that **hourglass/bow-tie checker artifacts still remain**. The latest work reduced some cases, but did not solve the artifact class.

Important user observations:

- Raw `compress` / `blurcompress` create obvious hourglass/checkerboard artifacts.
- `consistentcompress` / `hourglasscompress` reduce them but can leave sharper hourglasses.
- `lanczos*` and `deblurcompress` can also produce softer/blurry hourglasses.
- `deblurcompress` can invent colours at crossings, looking like lossy codec/JPEG artifacts.
- The artifact is better described as **hourglasses made of two triangles forming a checkerboard pattern**, not merely isolated pointy triangles.
- The user wants algorithms that avoid or suppress these artifacts, not image outputs.

## Build

```sh
cc -O3 -DNDEBUG -std=c99 -march=native celup_lab.c -o celup_lab \
  $(pkg-config --cflags --libs libwebp) -lm
```

## Main files in this archive

- `celup_lab.c` — current experimental C implementation.
- `README_lab.md` — accumulated mode notes and usage examples.
- `evaluate_upscalers.py` — analytic paired evaluator; supports `EXE:MODE`.
- `make_lab_comparison_sheets.py` — procedural comparison sheet generator; useful if images are needed later, but images are not included here.
- `test_celup3.py` — older regression/quality script from the initial lab.
- `comparison_sheets.md` — text-only metrics from the last generated comparison run.
- `handoff.md` — this file.

No generated images/examples are included in the archive.

## Current modes of interest

The binary currently exposes many modes. Relevant ones:

- `nearest`
- `bilinear`
- `triangle`
- `cubic`
- `mitchell`
- `lanczos2`
- `lanczos3`
- `dehourglass`
- `compress`
- `safecompress`
- `consistentcompress` / `hourglasscompress`
- `blurcompress`
- `safeblurcompress`
- `edgecompress`
- `deblurcompress`

`--auto-blurcompress` currently defaults to `deblurcompress` and tunes blur/strength by internal downscale validation.

## What has been tried

### 1. New reconstruction kernels

Added bounded:

- Mitchell-Netravali
- Lanczos2
- Lanczos3

These are premultiplied-linear and local-range clamped. Later, checker detection was added to blend kernel output toward bilinear in detected checker cells.

### 2. Compress family

Original `compress`:

- bilinear sample
- choose farthest pair in local 2×2
- project sample to endpoint line
- remap `t` using `compress_curve()`

Problem: treats checker/saddle cells as confident two-colour edges and creates hourglass geometry.

`safecompress`:

- added two-colour confidence gate
- later added `cell_edge_coherence()` to reject saddle/checker 2×2 patterns

This became safer but less aggressive and can still be imperfect.

### 3. `blurcompress` / `safeblurcompress`

`blurcompress` made artifacts worse because it compresses already-blurred 2×2 endpoint models.

`safeblurcompress` blends the old target into bilinear only when a raw 5×5 patch looks like a coherent two-colour edge. It reduced some gradient triangles but does not solve crossings/checkerboards.

### 4. `edgecompress`

Attempted continuous edge fitting over 5×5 patches:

- fit colour axis
- fit spatial `t` plane
- narrow edge coordinate
- use side averages and clamping

Problem: at crossings/multicolour patches it can still create geometry or colours not really present. It is intentionally conservative now, but not a solution.

### 5. `deblurcompress`

Several iterations:

1. bounded unsharp + edgecompress — invented geometry/colours.
2. bounded unsharp + safecompress — inherited 2×2 compression problems.
3. iterative downsample consistency / back-projection — avoided 2×2 compressor but still produced soft hourglasses and sometimes invented codec-like colours at crossings.
4. current version uses confidence-weighted back-projection and final hourglass-basis removal; still not solved according to the user.

### 6. `consistentcompress` / `hourglasscompress`

Started from raw `compress`, then tried to repair artifacts by source downsample consistency.

Later added a targeted checker/hourglass half correction:

- downsample high-res result back to source
- compute low-res residual
- detect alternating checker phase
- adjust alternating top/bottom or left/right triangular halves

Problem: reduces artifacts but leaves sharper hourglasses.

### 7. `dehourglass`

Starts from `lanczos3`, then fits/removes hourglass basis and does light consistency correction.

Currently uses extended basis in interpolation-cell coordinates:

- `b0 = |u - .5| - |v - .5|`
- `b1 = (u - .5) * (v - .5)`

Fit is relative to bilinear reference, not absolute colour. This was added because other LLMs correctly noted the prior basis alignment was half-pixel shifted and insufficient.

It helps numerically but user still sees hourglass artifacts.

## External suggestions already applied

From the uploaded suggestion files, these were implemented or partly implemented:

1. **2×2 checker detector**: `checker2x2_confidence_pm()`.
2. **Checker-aware kernel fallback**: `upscale_kernel()` blends Mitchell/Lanczos toward bilinear when checker confidence is high.
3. **2×2 edge coherence**: `cell_edge_coherence()` rejects saddle-like compressor cells.
4. **5×5 endpoint flip penalty**: `patch_two_colour_confidence()` includes a spatial endpoint-label flip penalty.
5. **Hourglass basis alignment fix**: `remove_hourglass_basis()` uses resampler/compressor coordinates with `-0.5` offset.
6. **Extended basis**: removes both hourglass and bilinear-saddle residual bases.
7. **Residual-relative basis fit**: fits `hr - bilinear_reference` instead of absolute `hr`.
8. **Cubic clamp bug fix**: central-pair range is collected before clamping.
9. **Checker-aware deblur weighting**: weighted residual back-projection attenuates checker cells.

## Important caveat

The external suggestions also argued that many of these local fixes are fundamentally limited because checkerboards are a Nyquist/aliasing ambiguity. A local algorithm cannot know whether a 2×2 checker means:

- real checker texture,
- crossing thin lines,
- aliased diagonal structure,
- codec noise,
- or a real high-frequency pattern.

The current code still attempts to sharpen/reconstruct in ambiguous cases, hence remaining artifacts.

## Current last metrics snapshot

From `comparison_sheets.md` in this archive:

- `deblurcompress` and `deblur auto` often score best by MAE.
- But the user visually rejects them due to residual/invented hourglass structure.
- `dehourglass` is less aggressive and safer-looking, but still has hourglasses.
- `safecompress` became less aggressive after checker guards and may score worse but should invent less.

Do not optimize solely against MAE; visual artifact rejection is more important.

## Recommended next direction

The next useful work should probably **stop adding post-hoc hourglass subtractors** and instead add a policy/classifier layer:

1. Classify local patches into:
   - coherent single edge,
   - checker/Nyquist ambiguity,
   - crossing/junction,
   - smooth gradient,
   - texture/noise.
2. For coherent single edge: allow edge/deblur sharpening.
3. For checker/Nyquist ambiguity: choose explicit policy:
   - `nearest` if pixel-art/hard texture,
   - positive/lowpass kernel if natural/smooth,
   - do not use negative-lobe Lanczos or endpoint compression.
4. For crossings/junctions: use conservative interpolation, not fitted edge/deblur geometry.
5. For smooth gradients: no compression or deblur sharpening.

Potential new mode idea:

```text
--mode adaptive
```

Pseudo-policy:

```text
if checker_conf high:
    q = bilinear or nearest/positive-kernel fallback
elif coherent_edge_conf high:
    q = mild deconv or edge model
elif gradient_conf high:
    q = Mitchell / cubic
else:
    q = Mitchell or Lanczos2 with low detail blend
```

Another possible direction is to add an explicit user option for checker policy:

```text
--checker-policy bilinear|nearest|mitchell|lowpass
```

This may be better than trying to infer intent.

## Known code risks / cleanup needed

- `celup_lab.c` has grown into an experimental monolith. It needs cleanup if work continues.
- Several modes are diagnostic and should not be considered production quality.
- Some helper names/comments may lag behind current behaviour due to many rapid iterations.
- `remove_hourglass_basis()` is now more sophisticated but still post-hoc.
- `deblurcompress` can still visually hallucinate in ambiguous/crossing regions despite weighted corrections.
- `consistentcompress` / `hourglasscompress` remain based on raw `compress`; they are expected to retain some artifacts.

## Suggested immediate next experiment

Implement a **strict checker/crossing fallback mode** instead of another deblur:

- Start from Mitchell or bilinear, not Lanczos3 or compress.
- Use checker2x2 + 5×5 flip ratio + junction score.
- In checker/junction cells, force bilinear or nearest, with no subsequent deconv correction.
- Only apply deconv/detail boost where `patch_two_colour_confidence` is high and checker/junction confidence is low.

This should trade some sharpness for artifact removal, which matches the user's latest feedback: current outputs are still too hourglass-prone even when sharper.
