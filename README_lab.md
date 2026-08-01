# celup_lab: comparable baseline modes

```sh
bash build.sh                          # or the cc line below
python3 tests/make_test_sources.py     # recreate tests/*.webp fixtures
python3 make_lab_comparison_sheets.py  # regenerate examples/, comparison_sheets/
```

```sh
cc -O3 -DNDEBUG -std=c99 -march=native celup_lab.c -o celup_lab \
  $(pkg-config --cflags --libs libwebp) -lm

./celup_lab in.webp adaptive.webp 2 --mode adaptive                       # recommended default
./celup_lab in.webp adaptive-auto.webp 2 --mode adaptive --checker-policy auto
./celup_lab in.webp nearest.webp  2 --mode nearest
./celup_lab in.webp bilinear.webp 2 --mode bilinear
./celup_lab in.webp cubic.webp    2 --mode cubic
./celup_lab in.webp mitchell.webp 2 --mode mitchell
./celup_lab in.webp lanczos2.webp 2 --mode lanczos2
./celup_lab in.webp lanczos3.webp 2 --mode lanczos3
./celup_lab in.webp dehourglass.webp 2 --mode dehourglass
./celup_lab in.webp blurred.webp  2 --mode blur
./celup_lab in.webp compressed.webp 2 --mode compress
./celup_lab in.webp consistentcompress.webp 2 --mode consistentcompress --strength 4
./celup_lab in.webp hourglasscompress.webp 2 --mode hourglasscompress --strength 4
./celup_lab in.webp safe-compressed.webp 2 --mode safecompress --strength 4
./celup_lab in.webp blur-then-compress.webp 2 --mode blurcompress --strength 4
./celup_lab in.webp safe-blurcompress.webp 2 --mode safeblurcompress --strength 4
./celup_lab in.webp edgecompress.webp 2 --mode edgecompress --strength 4
./celup_lab in.webp deblurcompress.webp 2 --mode deblurcompress --strength 4 --blur-radius .7
./celup_lab in.webp auto-blurcompress.webp 2 --auto-blurcompress
./celup_lab in.webp autoblur.webp 2 --mode autoblur                       # fitted blurry upscale
./celup_lab in.webp sdf.webp 2 --mode sdf                                 # signed-distance-field edges
./celup_lab in.webp classmap.webp 2 --mode classmap                       # classifier diagnostic
```

All modes use linear-light premultiplied RGBA and lossless WebP output.
Run `./celup_lab --help` for the full grouped help with short-flag aliases
(`-m adaptive`, `-s 6`, `-P auto`, `-A 0`...); every long flag still works.

## v4.9.4: C1-continuous 3x3 consensus DSDF & ARM64 / Android segfault fix

- **C1-continuous 3x3 consensus DSDF (`--mode dsdf`) across arbitrarily high upscales**:
  - Scientific diagnosis: Previously, `upscale_dsdf` evaluated `d_geom` from only the single nearest Voronoi seed cell (`roundf(sx), roundf(sy)`). Because `build_class_map` marks a 3-pixel-wide band around edges with `w_edge > 0` and sets `t0` to the mean of the 5x5 patch around each cell, an outer neighbor cell (1 pixel outside the true edge) had its own zero-crossing line `t0 - 0.5 = 0` located 1 source pixel away from the true edge. At high upscales (e.g. 8x, 16x), every marked cell drew its own independent contour inside its Voronoi boundary, causing:
    1. Spikes and jumps at Voronoi cell boundaries (`roundf(sx)` transitions);
    2. Edge caps copied and pasted 1 pixel distance more away from circle/curve centers.
  - Solution: Replaced single-cell Voronoi evaluation with a C1-continuous Gaussian-kernel consensus across the 3x3 neighborhood around any target coordinate `(sx, sy)`:
    - Candidate cells are rejected if their zero-crossing line `t = 0.5` does not pass through or near the cell (`fabsf(t0 - .5f) > .65f * mag`).
    - For valid edge cells in the 3x3 window, `d_geom`, endpoint colors `A, B`, and confidence `conf` are accumulated with Gaussian weights `conf * expf(-r2 * 1.5f)`.
    - Verified across all scale factors 1.5x to 24x (`tests/test_scales.py`): circle edges render as a single monotonic C1-continuous contour with zero step overflows (`step 0.000`) and zero offset caps.
- **ARM64 / Android segmentation fault fix for `sdf`, `msdf`, and `dsdf`**:
  - Root-caused the crash reported on ARM phones when running `--mode sdf`, `--mode msdf`, and `--mode dsdf`. All three modes call `upscale_adaptive` to build the adaptive reference image underneath. `upscale_adaptive` terminates with `suppress_speckle_pm(hr, dw, dh, ...)`.
  - In `suppress_speckle_pm`, Pass 2 (the domino pair pass) previously iterated `for (int vert = 0; vert < 2; vert++) for (int y = 1; y + 1 < dh; y++) for (int x = 1; x + 1 < dw; x++)`.
  - For a horizontal pair (`vert = 0`), the 3x4 bounding box around the pair needs `j` up to `+2`, which accessed `y + 2` at `y = dh - 2` (`dh` -> out of bounds by 1 row). For a vertical pair (`vert = 1`), the 4x3 bounding box around the pair needs `i` up to `+2`, which accessed `x + 2` at `x = dw - 2` (`dw` -> out of bounds by 1 column).
  - On ARM Linux/Android (including Android's default Scudo allocator), reads 4 bytes past heap allocation limits trap immediately with `SIGSEGV` (segmentation fault).
  - Fixed by correcting loop bounds to `for (int vert = 0; vert < 2; vert++) for (int y = 1; y + 2 - vert < dh; y++) for (int x = 1; x + 1 + vert < dw; x++)`. Verified zero AddressSanitizer / UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`) heap-buffer overflows or memory errors across all modes.

## v4.9.3: SDF line-angle invariance, junction/entrance-corner fix, 10-pixel border fade removal; 32-mode WebP sheets

- **SDF line-angle invariance (30°, 45°, 60°, shallow angles)**:
  - In `build_class_map`, discrete staircase steps on 30° and shallow lines have higher 5x5 plane MSE (`~0.035`) and lower `R^2` (`~0.50`) than 45° lines, causing `plane_conf` and `plane_r2` to reject them so `w_edge` dropped below `.18f`. Fixed by loosening `plane_mse` threshold to `.020f, .085f` and `plane_r2` threshold to `.25f, .70f`, allowing SDF to recognize and smooth lines at arbitrary angles with 100% confidence.
- **SDF junction & entrance-to-big-object artifact removal**:
  - When thin lines enter large solid objects or turn corners, splatting linear distance planes up to 3.5 source pixels away caused planes from thin features to radiate across junctions and bevel/chamfer corners (`check_corners.py`). Furthermore, 1D diagonal lines were sometimes misidentified as junctions. Fixed by adding 2D gradient structure tensor coherence (`coh_2d` and `junc_2d = 1.f - ramp01(coh_2d, .35f, .75f)`) in `build_class_map`, preventing confident 1D lines from being marked as junctions. In `upscale_sdf`, splat cutoff is restricted from `3.5f` to `2.0f` source pixels with tighter Gaussian weighting (`1.1f`), preventing linear planes from radiating across entrances to big objects.
- **SDF 10-pixel border fade removal**:
  - `upscale_sdf` previously penalized SDF confidence `f[10]` by splat weight normalization (`ramp01(iw, .15f, .5f)`) and smoothed gradient magnitude (`ramp01(|grad d|, .15f, .5f)`). Because border pixels have truncated neighborhoods and clamped boundary gradients, SDF faded out over ~10 pixels around image borders. Fixed by removing `f[10] *= ramp01(iw, .15f, .5f)` completely and lowering the gradient threshold to `.05f, .20f`, maintaining 100% SDF confidence around image borders right up to coordinate 0.
- **SDF 4-pass staircase suppression everywhere**:
  - Replaced the single-pass `[1,2,1]/4` signed-distance field smoothing in `upscale_sdf` with 4 iterative separable passes on `d0/d1` and loosened the edge confidence/derivative gates (`ck <= .18f`, `|grad d|` `.15f, .50f`, extent `3.5f`). This eliminates the 1-pixel periodic staircase wobble and reduces SDF staircase step `jump95` from `0.306px` to **`0.065px`** (almost 5× smoother) with `res95 = 0.063px`, suppressing staircases across all edges.
- **Vector-like graphics & advanced library upscaler benchmarks**:
  - Added `py:vector` (Vector-Contour Edge-Directed Upscaler, designed for vector-like graphics and pixel art to produce C1-continuous contours without staircase treads) alongside `scipy:spline5`, `cv2:lanczos4`, and `py:edgedir` in `evaluate_upscalers.py`, `hourglass_metric.py`, and `make_smiley_staircase_sheets.py`.
  - On `staircase_diag45_comparison.png`, `py:vector` scores **jump95 = 0.112px | res95 = 0.105px** (zero staircase treads), while `autodeblur Miya (-r 2.3 -s 100 -g 16)` scores **jump95 = 0.003px | res95 = 0.018px**.
- **Parameter override fixes**:
  - In `autodeblur`, explicit `--deblur-steepness K` (`-g K`) now overrides the default `.6 px` ramp clamp (`fminf(k, s / .6f)`), ensuring requested steepness is applied without being silently capped on narrower edges.
  - In `autoblur`, explicit `--blur-radius R` (`-r R`) is now evaluated directly by the parameter tuner (`auto_tune_soft_params`) instead of skipping sigmas outside the hardcoded 6-value table (`.15, .30, .50, .75, 1.10, 1.60`).
- **3x3 Checkerboard confirmation**:
  - Replaced standalone 2x2 checkerboard gating (`checker2x2_confidence_pm`, which misdetected 1-px diagonal lines as checkerboards and forced bilinear staircase treads) with 3x3 pattern confirmation (`checker3x3_at_pm`). True checkerboards (`pixelart_src.webp`: 1596 cells) continue to be lowpassed, while diagonal contours (`diagline48_src.webp`: 0 false positives) keep full cubic/Lanczos/autodeblur smoothness without bilinear staircases.
- **Advanced Python & C library comparison benchmarks**:
  - `evaluate_upscalers.py` and `hourglass_metric.py` now accept standard PIL (`pil:bicubic`, `pil:lanczos`) and advanced mathematical / edge-directed external upscalers:
    - `cv2:lanczos4`: OpenCV 8x8 Lanczos4 window.
    - `scipy:spline5`: SciPy 5th-order quintic C4-continuous B-spline interpolation.
    - `py:edgedir`: Python Edge-Directed Super-Resolution (Anime4K-style directional sharpening and normal displacement).
  - While `cv2:lanczos4`, `py:edgedir`, and `scipy:spline5` score competitive MAE on simple edges, `hourglass_metric.py` shows their hourglass/bow-tie artifact energy (HG) on textured and diagonal scenes (`rings`, `diag`, `corner`, `checker2`, `crosshatch`) is 5× to 10× higher than `celup_lab:adaptive` (e.g. `rings` HG: `cv2:lanczos4` = 0.01217, `scipy:spline5` = 0.01553 vs `celup_lab:adaptive` = 0.00191), demonstrating that `adaptive` eliminates bow-tie artifacts by construction.
  - `make_smiley_staircase_sheets.py` regenerates 18-mode visual comparison sheets (`poor_smiley_comparison.png`, `poor_smiley_crop_comparison.png`, `staircase_comparison.png`, `staircase_diag45_comparison.png`) with quantitative staircase metrics.

## v4.9.2 (micro): `-D remake` = `remap` alias; crosshatch analysis

The user's miya recipe spells the method `-D remake`, which v4.9.1
hard-errored on; it is now an alias of `remap`.  The v4.9.1
crosshatch HG note below was also fully root-caused by knob-off
builds: none of the new mechanisms (coverage gate, LS clamp, lobe
weights) moves the number; it tracks the base sigma alone (v4.9.1
fit + decoupled base = .00398, better than v4.9's own .00434), i.e.
the v4.9.1 fit beats v4.9's on either base and the delta is exactly
the rejected-crispness trade.

## v4.9.1: user-review fixes -- sigma decouple reverted, shading-aware profile fit, 45 deg staircase gate

User's verdict on v4.9 (same smiley, same recipe
`-r 6 -s 100 -g 64 -D remap`): **"You returned all errors. On that
smiley no neon now just because there is not enough blur now. -r 6 was
because that was the minimal blur when stairs no longer been visible.
Also there are visible pixels now, returned problem of snake tongue
lineends."** plus the standing demand: a staircase regression test on a
45 degree line, run on the final image.  Diagnosis and what changed:

- **The v4.9 base-sigma decouple is REVERTED.**  Rendering the
  fallback base at `r/min(K,8)` dodged the neon skirt by making the
  base crisper than the blur the user explicitly asked to keep: the
  lattice staircase -r 6 exists to hide came straight back, and with
  it per-tread speckle and forked caps ("snake tongue").  The base
  again renders at sigma = `-r`; `-r` is the assumed source blur for
  windows and gates, exactly as documented pre-v4.9.
- **The neon band is fixed where it is actually created -- the
  profile fit, not the blend.**  On a wide window (-r 6) the clamped
  `u` projection destroys ramp tails (window narrower than the blur),
  and a pure erf cannot represent step-on-linear-shading: the fit
  absorbed baked-in art shading into the step and dragged whole
  gradient zones to one plateau colour (the smooth dark "neon" band
  hugging lines, brightest in the mouth/neck shading).  v4.9.1:
  - the projection keeps an **unclamped raw channel**; lobe weights
    are taken from it, so ramp tails re-enter the lobe map (a
    saturated-but-flat plateau still hard-breaks lobes, so a thin
    line's two flanks stay apart);
  - the profile model becomes **linear baseline + erf step**
    (`y = a + c*z + b*phi(z)`, `tests`-visible helper `lsq_profile`):
    shading stays in the gain-1 residual channel untouched, only the
    step component is steepened;
  - the **drag amplitude is the one-sided plateau span** (the v4.8
    estimator), not the LS amplitude: on soft wide ramps the
    phi/linear LS basis is near-degenerate, reading ~0.7x the true
    span at tips (rounding) and >2x on shaded skirts (band); the
    plateau span is what the window itself proves, bounded to
    `[b_min, 1.2*span]` as a safety net for singular fits;
  - a **step-evidence coverage gate** fades the fit to zero when the
    window's observed profile does not straddle the modelled step
    (fits invented on pure shading slopes were another source of
    phantom bands).
- **Two mechanisms tried and REMOVED after measurement:** the
  step-component centroid refinement (re-centred mu at tips/corners
  and rounded them -- tip extent 83.75 vs gate 86; plain single-LS on
  raw projections is unbiased enough once tails are back in the lobe
  map) and the pass-1.5 mu-spread steepness governor (along a bending
  edge mu varies by construction, so it only ever fired at the
  geometry it destroyed; on straight shaded ramps it never engaged).
  The repository history keeps both attempts; the shipped code does
  neither.
- **45 degree staircase gate** (`tests/check_stairs.py`, fixture
  `diagline48`): sub-pixel edge-crossing tracking per output row on
  the FINAL image; tread-run length and adjacent-row crossing jitter
  (jump95) must stay smooth at BOTH user recipes (2x -r 6 and 4x
  -r 2.3) and MUST flag a deliberately crisp probe build -- the
  detector itself is asserted non-toothless.

Measured: the four hull/tip/width/glow invariants
(`tests/check_corners.py`) PASS with tip extent 86.25/87 (v4.9:
86.00) and glow 0.977; staircase gate ship2x jump95 0.111, ship4x
0.016, crisp probe 0.685 flagged; miya user recipe cheek HF .01439
(v4.9 .01686, v4.8 .01452), blush std .2036 (v4.9 .2429, v4.8 .2071);
huearc saturation .8505 unchanged; rampnoise HF unchanged; 4x torture
HG checker2 .00505 / crosshatch .00567 / rings .00374 / diag .00166 /
corner .00203 -- between v4.8 and v4.9 except crosshatch, where the
soft base costs lattice texture the crisp decouple kept (the same
crispness the user rejected on the smiley; accepted trade, stated
plainly); test_scales 204/204.  Smiley at the user's recipe: shading
gradient across the neck preserved (transect 0.43->0.54 smooth, step
single), mouth line continuous, no neon band, no treads, sharp spike
tips.

*The v4.9 section below is kept for history; wherever it claims the
decoupled base sigma is the neon fix, v4.9.1 supersedes it.*

## v4.9: corner-sharp autodeblur -- junction gating, decoupled base sigma, contour-consensus fits

Review of v4.8 (user's own 256px hard-edged smiley, 2x,
`-r 6 -s 100 -g 64 -D remap -c linear -k bspline`): "previous problems
resolved, but now sharp corners/line-end tips come back ROUNDED, and
the halo is different -- smoother, like a neon glow".  Two model
defects, no per-artifact patches:

- **Neon glow** = low-trust pixels blending toward the base render at
  the user's ASSUMED blur sigma (-r): at a mismatched -r every edge
  grows a sigma-wide skirt, and staircase treads re-sharpen into
  contour bands floating on that skirt.  In autodeblur mode the base
  reconstruction sigma is now **decoupled** from the assumed blur:
  `sigma_base = max(.6, r / min(K,8))` -- never wider than the sharpest
  output the deblur itself can produce, so partial trust can no longer
  invent a wide smear.  `-r` remains the assumed source blur (window
  sizing, shading gate).  NOTE: on a hard pixelated source do NOT
  overset -r (it is a claim, not a strength knob); ~1-2.3 fits art with
  light antialiasing.
- **Rounded corners** = the tangential mechanisms (line-sample
  averaging, pass smoothing) assumed the contour is translation-
  invariant along its tangent -- false at corners/tips.  A **junction
  measure from the same structure tensor** (`rho = lambda2/lambda1`)
  now scales the tangent span and the pass tap weights: straight
  contours keep full anti-wobble averaging; corners keep their own
  radial fit and stay sharp.

Plus one structural upgrade found by the new forensics fixture
(`cornerstar48`, acute wedge): raw per-pixel fits can misplace the ramp
centre by 1-2 output px near wedges, and anchored evaluation rendered
that jitter amplified ~k, pushing deltas outside the local colour
range.  v4.9 renders each pixel from a **tangentially integrated
contour-consensus fit** (pass 1.5: wS-weighted mu/s/delta-colour
integrated along the junction-aware tangent; z/k/nu recomputed from the
consensus so anchored evaluation stays exact), and the output is
clamped to the **local observed colour hull** (per-pixel window
min/max, display-quantized -- a deblur has no ringing vocabulary).
The hull invariant and corner sharpness are gated by
`tests/check_corners.py`.

Measured (4x torture, HG vs v4.8/autoblur): checker2 .00496/.00469/
.00428, crosshatch .00434/.00417/.00363, rings .00463/.00398/.00260,
diag .00203/.00214/.00127, corner .00297/.00184/.00175 -- dense gated
scenes now keep the source structure (MAE up to +.03 on synthetic
soft-truth scenes, by design: detail preferred over smoothness);
huearc saturation .8504 (unchanged); rampnoise HF .00183 (v4.8:
.00214, base .00117 -- consensus removed the v4.8 mu-jitter dither);
step48 transect sharper, plateaus pinned, monotone; miya user recipe
blush texture .2429 (v4.8 .2071), cheek clean.

## v4.8: anchored autodeblur -- lobe-local fits, evaluation at the pixel's own position

Review of v4.7: "closer, but it can create outstanding pixels at
centers of gradient; line ends look like snake tongue with split to 2
ends; a halo of color surrounds the line smoothly from line center,
quickly ending near the line edge; looks like 2 gradients surrounding
an edge are combined instead of not going further than each other.
Can steepness be float, much higher than 8? Can the deblur fit the
original colors with the gradient change? Surely specific artifacts
aren't supposed to be handled by per-image-part heuristics."  All
three observations trace to v4.7's three remaining design flaws, and
the fix is model-level, not per-artifact heuristics:

- v4.7 remapped each pixel's COLOUR through the steepened fit
  (nu = Phi(k*Phi^-1(u))).  d(nu)/du = k at the ramp centre, so a
  pixel epsilon off the fitted curve rendered k*epsilon off
  (outstanding mid-gradient pixels); near plateaus Phi^-1 explodes,
  printing flat noise as the colour halo that hugs a line from its
  centre and dies at its edge; and one window-wide fit spanning a
  thin line averaged BOTH flanks and BOTH backgrounds into one
  phantom step centred mid-line (the "combined gradients"), which
  forked line caps into snake tongues.  v4.7 also snapped every
  output onto the fit's 1D colour segment, discarding the
  perpendicular colour component -- the "watered away colors".

v4.8 keeps the v4.7 sampling (4D structure tensor, one gradient
direction for the premultiplied vector, tangentially averaged line
samples) but redefines fit domain and evaluation:

- LOBE MAP: |du| along the normal is segmented into transition lobes.
  The pixel is assigned its NEAREST lobe; plateau colours, centre mu
  and width s come from that lobe alone with one-sided margins
  clipped at neighbouring lobes -- two flanks and two backgrounds
  never enter one fit, and misassignment only yields plateau+residual
  = identity (degrades safely, never to an artifact).  The PULSE
  branch is gone: one analytic model handles steps and lines alike.
- ANCHORED EVALUATION: the steepened fit is evaluated at the pixel's
  GEOMETRIC position on the normal and the pixel's own fit residual
  is re-added with GAIN 1: out = F_k(0) + (o - F(0)).  On-curve
  pixels steepen exactly by k; off-curve deviations (texture, hue
  arcs, dither, alpha) pass through unamplified: no k-amplified
  speckle, no Phi^-1 halo, original colours kept -- "fit the original
  colors with the gradient change", as suggested.  Consequence: -g
  now takes FLOATS 1..64 (noise stays gain-1 while k grows).  The
  anti-realiasing cap k <= s/.6 still decides the true maximum per
  edge (wider intended blur via -r/-e or higher scale raises it).
- The model delta is smoothed TANGENTIALLY along the contour (pass 2)
  to remove hundredth-pixel fit jitter before application; residuals
  are never smoothed.
- MULTI-CROSSING trust replaces the beta2 gate: per-lobe fits are all
  good inside dense texture, so suppression counts hysteresis
  mid-level crossings over the whole window (step = 1, one line = 2:
  full trust; 4+ fades to zero) beside the erf-RMSE-over-lobe gate.

Measured: step48 4x -r1.5 -g8 remap transition ~20 px -> ~9 px,
plateaus pinned, no ringing (-g 8/16/32 identical there: the s/.6 cap
is the binding constraint by design; raise -r for more).  miya_face
with the user's recipe (-r 2.3 -s 100 -c linear -k bspline -D remap):
cheek-gradient HF noise .0176 (v4.7) -> .0157 at -g8, unchanged at
-g16 (texture gain ~1); blush texture retention .1652 -> .1677
(anchored colours, not watered); spike line-end centre-notch in v4.7
gone at -g8/-g16 (review sheet).  Torture 4x defaults: checker2 HG
.00469 (autoblur .00428), crosshatch .00417, rings .00398, diag
.00214, corner **.00184** -- the v4.7 corner phase-offset caveat
(.0101) is FIXED by lobe localization, at autoblur level (.00175);
every MAE improved vs v4.7 (checker2 .0517 vs .0567).  tests/
test_scales.py 204/204 PASS; clean -Wall -Wextra -Wshadow.  Known
caveat: on synthetic ramps with injected dither, residual model
jitter shows as ~+/-1 LSB random dither (sub-visible; real-content
texture passes through at gain 1 instead -- see handoff v4.8).

## v4.7: analytic autodeblur -- profile fit per gradient, slope on the fit

Review of v4.6 on the wide-blur recipe: "it doesn't reduce steepness,
it creates a bright line in the middle of the gradient and negates
colors... separate color, alpha gradients instead of 4-channel vectors
+1 gradient... still box windows like 5x5 because there are stepladder
artifacts. Can deblur be more analytical: construct a function of the
gradient that fits the original, change the slopes on that function,
then sample pixels?" All three guesses were verified at 3x zoom
(v4.6: magenta/cyan hue-inverted fringe riding edges from per-channel
box-corner snapping; a white SQUARE ring around the beauty-mark dot
from fixed-window value quantisation), and the answer is the new core
doing exactly the proposal:

- per pixel, ONE gradient direction for the whole premultiplied RGBA
  vector (principal axis of the 4D structure tensor over per-channel
  Sobel), colours sampled along that normal with tangential averaging
  (kills source-lattice jitter; otherwise steepening prints it as
  scallops -- the corner-torture "greek key" seen and fixed);
- fit an analytic profile of the gradient coordinate: error-function
  edge (STEP) or two-flank pulse (thin LINE), soft-blended between the
  two by endpoint coverage (no branch switching seams);
- steepen the slope ON THE FIT: remap evaluates Phi(k * Phi^-1(u_px)),
  push evaluates a z-displaced original (the first-order Anime4K push
  in analytic form); the pixel's colour is rebuilt as a convex mix of
  two REAL local colours (segment endpoints).  By construction: hue
  cannot invert (no box corners), alpha stays coupled (convexity keeps
  rgb <= alpha), nothing overshoots (fit bounded in (0,1)), the map is
  spatially continuous (no window-quantised bands), k is capped so
  output ramps stay >= .6 px sigma (~1.5 px 30%-width: no re-aliased
  sawtooth), rendering goes to a separate buffer (no scan-order
  coupling), and already-linear shading is inert (u == .5 argument).
- two trust gates: fit-rmse (full trust <= .03, none >= .10) and a
  STEP-unimodality gate (flatness m4/s^4: clean ramps 2.0..2.6, multi-
  crossing windows ~1.4, closed below 1.7) -- crosshatch/rings/text
  windows whose geometry a single edge/pulse model cannot explain are
  left alone instead of hallucinated.  (Debug override: CDG=lo,hi.)

Measured: synthetic gaussian step (-r 1.5, 4x, -g 8) 30%-transition
~20 px -> ~12 px with plateaus pinned and no ringing (internal
detail: the naive Phi(-k*mu/s) was killed by the window-clipped moment
centroid bias; mapping via the pixel's own fitted coordinate z =
Phi^-1(u) is what made the analytic idea actually steepen).  miya
-r3 -g 8 (the recipe): the v4.6 midline / white boxy halo / cyan
fringe are gone; narrowing stronger than v4.6 by mid-transition share
(.665 vs .728 vs base .776) and hue-consistent
(docs/review_v47_final.png).  Default anime look preserved side-by-
side.  Torture (default -s 2): checker2 HG .0040 (below autoblur
.0043), crosshatch .0060, rings .0050, diag .0027, corner .0101; MAE
<= autoblur on crosshatch/rings/diag.  The corner HG sits on the
white V band as uniform phase offset of the fit regularization (no
localized texture; HG95 == HG) and is gate-insensitive even at -g 1;
the visible defects there (scallops) were fixed by tangential
averaging.  tests/test_scales.py 204/204 PASS; clean -Wall -Wextra
-Wshadow.  NOTE: autodeblur outputs change everywhere vs v4.3..v4.6
(deliberate redesign).

## v4.6: deblur gate is sigma-aware -- wide intentional blur really unblurs

Report: `-m autodeblur -k bspline -c exp -r 3 -s 100 -A 10 -g 8 -D push`
(blur intentionally wide to erase pixel stepladders, deblur supposed to
restore the edges) produced the blurred base unchanged. Diagnosis, two
stacked causes:

- **gate blindness**: the v4.3 edge gate (|grad| / window-range over
  [.08,.18], window 1.25 src px) classifies transitions by implied
  width; ramps from a sigma=3 fit are ~2.5*3 = 7.5 src px wide, i.e.
  ordered-of-magnitude past the gate -- every blurred edge was
  misread as "smooth shading" and left alone even at k=8.
- **inertia on linear ramps**: even with the gate open, the remap is
  inert where u == .5, which holds everywhere on a *linear* gradient
  (safety property by construction); wide gaussian ramps need the
  spatial redistribution of the push method plus a working gate.

Fix, active only when the blur is actually wide (`scale*max(1,sigma) > 4`):

- analysis window widens with the fitted sigma (cap 64 px);
- the gate compares implied ramp width against the full width of a
  gaussian ramp of the fitted sigma (2.5*sigma*scale px), so a blurred
  step is an edge at ANY blur level while unbounded shading still falls
  outside the gate;
- push displacement is capped to the window (at -g 8, sigma 3 the
  22 px displacement overshot the R=15 window and the range clamp
  quantised everything to the window extremes).

Measured on the reported command (miya_face, 4x, sigma 3, -g 8): strong-edge
mean slope x0.99 (v4.5) -> x1.44 (v4.6 push) / x1.88 (v4.6 remap); max
slope unchanged (no new extrema: no halos, by the same range-clamp
argument as v4.3). Visually: hair/skin boundary, bang tips, beauty mark,
eyebrow go from ~10px gradient to defined edges, zero fringe.

Invariant: whenever `scale*max(1,sigma) <= 4` the v4.3 formulas evaluate
bit-exactly, so default 2x..4x autodeblur, the tuned miya/badge looks,
and all v4.4/v4.5 comparisons are byte-identical (verified); torture HG
metrics unchanged (diag .00142). The wide branch also repairs high
scales: at 8x+ the old fixed-px gate mapped to <1 src px and the
steepening was nearly shut there too.

Recipe (blur-wide-then-unblur, e.g. sawtooth-y sources):

```sh
./celup_lab in.webp out.webp 4 -m autodeblur -k bspline -c exp -r 3 -g 8 -D remap
```

## v4.5: every automatic parameter is manually pinnable; complete --help

"Can these params be set manually? In help there is no list of all
possibilities." Now yes, and the help lists everything (all 23 modes,
every value set per option, defaults, and the pin table below).
Most pins existed since v4.2/v4.4 but were undocumented or mislabelled
(`-r` was described as "*blurcompress only" while it also pins the
autoblur sigma); v4.5 fixes the docs, adds the one genuinely missing
knob, and makes manual settings authoritative:

| parameter (echoed to stderr) | automatic choice          | manual pin                        |
|------------------------------|---------------------------|-----------------------------------|
| kernel                       | validation-proxy fit      | `-k box\|triangle\|gaussian\|bspline` |
| sigma                        | fit, then `-e` escalation | `-r R` (exact; `-e` then backs off) |
| curve                        | validation-proxy fit      | `-c linear\|sigmoid\|cubic\|exp\|log\|sqrt\|circle\|nearest` |
| curve param                  | fit                       | `-p P` (0..40)                    |
| deblur method                | 2x proxy MSE              | `-D remap\|push` (auto = proxy)   |
| steepness k                  | `-s` formula or `-e`/edge | `-g K` (exact 1..8) -- NEW        |

- Only UNPINNED parameters are fitted, so partial pins work
  (`-k gaussian -r .75` keeps those two and still fits curve/param).
  When all four autoblur parameters are pinned the validation fit is
  skipped entirely (faster) and the run prints
  `autoblur manual: kernel=... sigma=... curve=... param=...`.
- Every effective value is echoed to stderr/stdout, so an automatic run
  can be reproduced exactly later by pinning its reported numbers.
- NEW `-g, --deblur-steepness K` (1..8, default 0 = auto): sets the
  autodeblur slope multiplier directly instead of via the `-s` formula
  (k = 1+.25*(s-1), clamp 3). Priority: `-g` > `-e` per-edge adaptation
  > `-s` formula. With `-g` and `-e` together, `-e` still steers the
  BLUR fit; only the steepness adaptation is pinned.
- FIX: `--edge-goal` escalation no longer overrides a sigma the user
  pinned with `-r` ("manual wins over goal", one stderr note).
- FIX: `-d/--adaptive-debug` accepted only 0..7, but bit 8 (drop the
  line class) existed in the renderer -- range is now 0..15 and the
  bits are documented (1 edge, 2 checker, 4 junction, 8 line).
- Default behaviour is unchanged: v4.4 vs v4.5 binaries produce
  bit-identical outputs on autoblur/autodeblur/adaptive/sdf spot runs;
  scale sweep 204/204 PASS.

Quick recipe -- pin what an auto run chose:

```sh
./celup_lab in.webp probe.webp 4 -m autodeblur            # note stderr
./celup_lab in.webp final.webp 4 -m autodeblur \
  -k bspline -r 1.23 -c linear -D remap -g 2.5            # exact rerun
```

## v4.4: tunable edge-width goal (-e), auto-chosen deblur methods (-D)

The autoblur fit's validation proxy is biased to little blur (a sharper
reconstruction trivially matches the sharp target): on staircase-y sources
it picked the legal minimum ("not enough blur to get rid of sawtooth
edges"), and autodeblur then had nothing to work against. Two goal-value
knobs fix this and expose the trade explicitly:

- `-e, --edge-goal W` (src px, 0..8, default 0 = off): after the fit, the
  rendered edge width (robust p30 of range/slope over strong edges,
  calibrated so a gaussian ramp reads ~2.5 sigma) is measured AT TARGET
  RESOLUTION; if below the goal, sigma escalates x1.35 per step (ceiling
  2.5) and re-renders until the goal is met -- "increase blur towards
  smooth edges first of all", spent exactly as much as needed.  Direct
  target-res enforcement replaced a validation-penalty attempt whose
  width measurements were miscalibrated by the 2x proxy.  stderr reports
  every step (badge 8x: s .50 -> 1.37px -> .68 -> 1.61 -> .91 -> 1.84 ->
  1.23 -> 2.15 OK for -e 2).  In autodeblur, the same value adapts the
  steepness PER EDGE: wide mushy transitions get k toward 3, edges
  already at the goal get k=1 (untouched) -- nothing over-sharpens.
  Warning: goals >= ~1.5 visibly soften genuine 1px line-art; that is the
  point of the knob, aim it at the content.
- `-D, --deblur-method auto|remap|push` (default auto): the deblur
  methods the v4.3 research surfaced, both artifact-guarded, with the
  best AUTO-CHOSEN per image by the same self-supervised 2x proxy:
  (1) remap: monotone slope remap u'=.5+(u-.5)k inside the measured local
  range (Anime4K's "gradient maximization without overshoot", closed
  form); (2) push: Anime4K-flavoured spatial gradient push (sample the
  base along the unit gradient by (u-.5)(k-1)*1.6*scale px, clamped to
  the local range).  Proxy scores and the winner print to stderr (miya:
  remap .0005225 < push .0005313 -> remap; note remap IMPROVES on the
  raw autoblur proxy .0005251 -- the deblur is real signal gain, not
  cosmetics).  Osher-Rudin shock filters remain deliberately unadopted:
  the references show their staircase signature is exactly what we are
  removing.

## v4.3: autodeblur mode (gradient-slope steepening on the fitted blur base)

Addresses the "AI-upscaled anime-art cleanup" case: diffusion-rendered /
internally-upscaled art arrives with mushy 1-2px AA, salt noise in flats
and lossy block boundaries; the goal is to clean AND upscale with zero new
artifacts. `-m autodeblur` renders the fitted autoblur base (which the
user judges better than triangle on anime for consistency/detail), then
steepens transitions directly in gradient space as requested:

- per pixel, over a ~1.25 src px window: robust per-channel range
  [lo,hi], normalised u, slope remap u' = .5 + (u-.5)*k with k from
  `--strength` (k = 1 + .25*(s-1), clamped 3). MONOTONE and
  range-anchored: halos/ringing/hourglass/quantized new colours are
  structurally impossible.
- blend weight = smoothstep of |grad|/local-range over [.08,.18]: real
  edges get the full steepening, smooth shading/blush (low relative
  slope) is never touched (no posterization, unlike shock filters or
  Krita's unblur brush),
- true flat/near-flat windows are instead pulled toward their local mean
  (flat-flatten), mopping up diffusion salt without staining edge
  surrounds (range-gated; v1 of the pass painted edge halos, fixed),
- premultiplied invariant rgb <= alpha restored per pixel.

References implemented/considered: Anime4K's iterative gradient-ascent
push (heightmap gradient maximization without overshoot/ringing),
Osher-Rudin shock filters (staircase-prone; avoided the sign-hard
update), anime-encoder descaling (fit-the-blur then render -- the
autoblur base IS the fitted descale). MAE == autoblur to 1e-4 (sharpened
variant trades nothing measurable), HG == autoblur except edges
(diag HG .00142 vs sdf .00529), all 192 scale-sweep tests pass
(1.5x..24x incl. the miya face strip).

## v4.2: gradient-only suppression, sdf halo guard, alpha cleanup, CLI

- **Hourglass/speckle suppression only touches directed gradients, never
  symmetric inputs.** Both cleanup passes (`remove_hourglass_basis` and the
  v4.1 loner/domino speckle pass) are now multiplied by a structure-tensor
  *direction gate*: the 3x3 gradient outer-products must align behind one
  dominant orientation (edges/ramps/line flanks) for suppression to apply.
  Symmetric centres -- dots, star cusps, checker phases, junction
  crossings, isolated Nyquist pixels -- pass through verbatim. On the miya
  art asset this removed the dark ring adaptive used to draw around
  symmetric dots and the speckles sdf left near them; torture-set HG is
  essentially unchanged (crosshatch .00878->.00884, checker2 .00319->
  .00397, checker1 unchanged, rings/diag/corner unchanged).
- **sdf halo guard**: the re-thresholded colour is clamped to the local
  3x3 source envelope (union with the base pixel's own excursion). Any
  overshoot beyond what the source neighbourhood supports is a halo by
  definition and is clipped -- kills the bright fringes/dark notches sdf
  could draw along strong edges. MAE impact ~1e-4; sharpening preserved.
- **Alpha garbage cleanup** (`-A T, --alpha-clean T`, default 10, 0=off):
  lossy web assets hide bright RGB under alpha 0 and sprinkle isolated
  semi-transparent salt (alpha 1..~150) over "empty" regions; resampling
  reproduces it and sharpening amplifies it. At load, alpha==0 pixels are
  zeroed, isolated <=T dust is wiped, and fully isolated mid-alpha specks
  (no neighbour at half their alpha) are removed. Genuine faint glows and
  >=2px sparkles (which always have comparable-alpha neighbours) survive.
  On miya: semi-transparent background dust 46710 -> 2241 px (nearest).
- **CLI overhaul**: `-h/--help` prints a full grouped help (recommended
  modes, every option, ranges, defaults). Every long option has a short
  alias: `-m -s -r -a -P -A -k -c -p -M -d`. Old long flags unchanged.
- **Scale testing**: `tests/test_scales.py` sweeps every mode at 1.5, 2-6,
  8, 10, 12, 16, 20, 24x (yes, past 20x) on a synthetic AA circle plus a
  32x11 face strip at 2/4/8/16/22x -- asserting dimensions, zero staircase
  treads and bounded AA steps. 187/187 pass. Extreme scales only need a
  small source window to expose artifact patterns; full-frame 22x renders
  are pointless for that.

## Which mode should I use? (v4)

- **adaptive** is the flagship default for natural images. A 5x5 patch
  classifier routes every cell: bounded Mitchell for ordinary content and
  coherent edges, plain bilinear at junctions/crossings, an anisotropic
  pulse model for genuine 1px thin lines (sharpened along the line normal
  only, so no lateral/hourglass structure can appear), and an explicit
  non-inventing policy in checker/Nyquist-ambiguous cells. On top, a few
  class-gated consistency iterations and a focused hourglass-basis cleanup
  recover edge sharpness without rebuilding checkerboard/bow-tie artifacts.
- **adaptive --checker-policy auto** detects pixel-art-like inputs (hard
  palette, few soft blends) and switches the ambiguous-cell fallback to the
  crisp scale2x sampler; natural images get the lowpass fallback. Use this
  when the input mix is unknown.
- **autoblur** is the best *blurry* upscale for the image: no
  sharpening, no plain bilinear. It decomposes a blurry reconstruction into
  an overall blur kernel (box / triangle / gaussian / bspline + sigma) and a
  gradient transition curve (linear / sigmoid / cubic / exp / log / sqrt /
  circle / nearest), and *fits both to this image* by the self-supervised
  2x-downscale validation proxy. It beats bilinear MAE on every test scene
  while never inventing high frequencies -- ideal when you explicitly want
  a soft result with the best-matched transition shape.
  v4 renders the fit by *continuous kernel splatting* at the target
  resolution (v3 blurred the source grid, which degenerated to blocky cells
  and staircase tracking at large scales), so 8x upscales are smooth: no
  mosaic ("blurry pixels"), no sawtooth on wandering lines.
- **autodeblur** (v4.3) is the anime/AI-art default: autoblur's fitted
  clean base + gradient-slope steepening with a monotone, range-anchored
  remap (zero new artifacts by construction) and flat-zone salt
  flattening. Crisper than autoblur, safer than every sharpening mode;
  tune edge steepness with `-s` (2 subtle, 6 assertive, 10+ approaches
  posterization). See the v4.3 section for the mechanics and references.
- **sdf** (v4.1) is the smooth-geometry sharp option. Every confident
  coherent-edge pixel's fitted t-plane is a *signed-distance plane*
  (sub-pixel, oriented); the mode kernel-averages all candidate planes into
  C1-smooth distance/width/endpoint/confidence fields, then re-thresholds
  the local two-colour transition against the upsampled field as a
  confidence-gated delta ON TOP of the full adaptive pipeline (v4.0's
  distance-transform/nearest-seed approach produced washboard banding,
  phantom midline contours and flat-zone streaks -- all diagnosed and
  replaced; the other agent's chamfer-SDF/MSDF that snaps to nearest at
  edges was reviewed and not adopted).  Further guards: ramp widths are
  de-diluted (the 5x5 LS plane fit saturates at slope 0.3 for true AA <
  1.6px), planes splat only near their own ramp (no ghost extensions),
  |grad d| coherence suppresses junction/midline zones, and the delta's
  local DC is removed so contour sharpening can never shift tone.  sdf
  MAE-beats adaptive on 5 of 9 standard scenes and inherits its checker
  policies; it is the cleanest 8x line-art upscale (crosshatch HG 0.0088,
  lanczos3 0.0243).
- **deblurcompress** remains the maximum-detail option: same gated iteration
  core, more iterations, more sharpening. It is ~7.5x faster than v1
  (per-cell gates are precomputed once) and its crossing/checker artifacts
  are greatly reduced, but adaptive is still the cleaner default.
- Pixel art / sprites: `--mode adaptive --checker-policy scale2x` (or
  `--mode scale2x` for a pure, aliased-hard result) and `--mode nearest`
  as the zero-invention reference.

## New options (v2)

`--checker-policy lowpass|bilinear|nearest|mitchell|scale2x|auto`
controls how **adaptive** reconstructs checker/Nyquist-ambiguous cells:
- `lowpass` (default): Gaussian sigma .75 source px; removes the aliased
  band entirely. Safest for natural images; intentional checker texture is
  smoothed away (this is the correct non-inventing choice when the checker
  origin is unknown).
- `bilinear`: keep the bilinear X-crease; soft diamond texture remains.
- `nearest` / `scale2x`: hard, palette-exact reconstruction; `scale2x`
  adds the classic diagonal-connection rule. Right choice for pixel art.
- `auto`: scale2x when the image looks like pixel art (few unique sRGB
  quads and almost no soft-contrast neighbour links), lowpass otherwise.
  The heuristic is global, so one bad region cannot flip a natural photo.

`--checker-policy` only affects `adaptive` (and `sdf`, which inherits the
adaptive pipeline). `--adaptive-debug N` (0..15) is
a development aid that zeroes selected class weights (bit 1 = edge,
bit 2 = checker, bit 4 = junction, bit 8 = thin-line) to attribute error to
policy branches. `CELUP_CLASS_DEBUG=1` in the environment prints thin-line
detection details to stderr.

## Hourglass speckle suppression (v4.1)

After the basis-fit hourglass remover, all gated iterative pipelines
(adaptive, deblurcompress, consistentcompress/hourglasscompress) now run an
*isolated-pixel* pass: a single output pixel (loner) or an axis-adjacent
pair (domino) that deviates from every surrounding pixel much more than the
surroundings deviate among themselves is overwritten with the surrounding
average, blended by the same class gate (checker/junction ambiguity) and
amount as the basis removal.  Genuine lines/edges are always supported by
their neighbourhood and never match; measured on the torture scene:
adaptive loners 81 -> 0, deblurcompress 5909 -> 0, HG metrics unchanged
(the pass removes specks orthogonal to the fitted bases, not amplitude).

## autoblur options (v4)

`--blur-kernel box|triangle|gaussian|bspline|auto` picks the overall spatial
blur kernel; `auto` fits it (and its sigma) for the current image.
`--blur-radius R` pins the sigma. `--blur-curve
linear|sigmoid|cubic|exp|log|sqrt|circle|nearest|auto` picks the gradient
transition curve; `--curve-param K` pins its parameter (steepness k for
exp/log, exponent p for sqrt; ignored by the fixed families). Anything left
unset is fitted by the internal 2x-downscale validation proxy (two stages:
kernel+sigma with a linear curve, then the curve family). All curves are
symmetric about the midpoint and fix both endpoints, so the reconstruction
is always monotone -- no ringing, no invented colours, by construction.

v4 rendering/selection notes (the 8x fixes):
- The fitted model is rendered by splatting the *analytic* kernel at the
  target resolution with the transition curve as a per-cell coordinate warp,
  not by blurring the source grid and re-sampling. Kernel profiles carry a
  floor (~1 source px of genuine support), so every source pixel always
  blends into its neighbours and no per-cell mosaic can form, at any scale.
- The proxy MSE cannot see blockiness (the validation target is the sharp
  original), so near-tie picks (within 3% MSE) are resolved toward the
  larger sigma / smoother kernel family, and steep warp curves pay a
  max-slope penalty: `nearest` or log4.5 on smooth content must win
  outright, while genuinely stair-stepped sources (pixel art) still fit
  them because their MSE gap is huge.

```sh
./celup_lab in.webp out.webp 4 --mode autoblur                                   # full auto fit
./celup_lab in.webp out.webp 4 --mode autoblur --blur-kernel triangle            # fit sigma+curve
./celup_lab in.webp out.webp 4 --mode autoblur --blur-curve circle               # fit kernel+sigma
./celup_lab in.webp out.webp 4 --mode autoblur --blur-kernel gaussian \
           --blur-radius .8 --blur-curve log --curve-param 3                     # fully manual
```

## Memory guard

The command-line encoder needs the complete decoded source and complete output raster; libwebp's convenience lossless encoder also needs internal working storage. `celup_lab` now estimates a conservative peak before output allocation and refuses work over the limit rather than risking an OOM kill. The default is `512 MiB`:

```sh
./celup_lab in.webp out.webp 4 --mode cubic --max-mib 768
```

`--max-mib` accepts `32..65536`. Raising it does **not** create RAM; it only permits the allocation when the machine really has enough available memory. True low-memory arbitrary-size WebP output needs a tiled/streaming encoder backend, because the current libwebp convenience API is full-frame.

- **nearest** is the consistency reference: no new intermediate colours.
- **bilinear** is the intentionally soft/blurry baseline the discussion refers to. It uses positive 2×2 weights and has no ringing.
- **cubic** is the bounded cached Catmull–Rom mode from celup3: sharper smooth reconstruction without cubic overshoot.
- **mitchell** is a bounded Mitchell–Netravali bicubic (`B=C=1/3`). It is usually less crisp than Catmull–Rom but smoother and less prone to stair-step emphasis.
- **lanczos2** is a bounded 4-tap windowed-sinc reconstruction. It is a good sharp default candidate when you want more real detail recovery than cubic while limiting ringing.
- **lanczos3** is a bounded 6-tap windowed-sinc reconstruction. It is the sharpest general-purpose mode added here; it costs more CPU and can show more texture/ripple than `lanczos2`, but the footprint clamp prevents obvious negative-weight halos.
- **dehourglass** starts from `lanczos3`, fits the per-source-pixel hourglass/saddle basis `|x-.5|-|y-.5|`, subtracts it, lightly restores source downsample consistency, then subtracts the basis again. It targets the blurry checkerboard hourglasses seen even in normal resamplers without using endpoint compression.
- **blur** is a genuinely soft reference: a source-space 5×5 binomial Gaussian blur, then bilinear interpolation. A Gaussian is preferable to a 4×4 box average because the box kernel makes a flatter, more artificial footprint and has a worse frequency response.
- **compress** is an explicit experiment implementing gradient-width compression. It begins with bilinear reconstruction, selects the farthest pair in the local source 2×2, projects the bilinear sample onto that pair, then applies `smoothstep(t)`. This pulls a blend toward either flat endpoint while keeping its `t = 0.5` midpoint in place. It can improve JPEG-like flat vector graphics but will also harden real tonal gradients; it is not a general default.
- **safecompress** is the safer compressor revision. It uses the same endpoint remap, but blends it in only when the local 2×2 is a high-confidence two-colour cell: enough contrast, samples close to one line segment in premultiplied-linear RGBA, and samples clustered near the two endpoints. This is intended for binary/vector transitions and avoids most indiscriminate gradient hardening.

- **blurcompress** runs the same compressor on the blurred 2×2 endpoint model. This is the literal blur-then-narrow experiment; the saved result shows that information lost to a wide blur cannot be fully restored by local contrast remapping.
- **safeblurcompress** keeps the blur-then-narrow target but blends it into an unblurred bilinear base only when a raw 5×5 source patch looks like a high-confidence two-colour edge. Broad gradients contain many mid-ramp samples, so compression is suppressed and the small-triangle gradient artifacts are greatly reduced.
- **consistentcompress** / **hourglasscompress** is a targeted fixer for the old 2×2 compressor's checkerboard hourglass artifact. It first makes the raw `compress` result, detects the alternating low-resolution residual, then adjusts only the alternating top/bottom or left/right triangular half of each source-pixel block. This tries the pointy/wide hourglass colour flip/correction directly, followed by a light source-consistency pass. It is still based on `compress`, so it remains a diagnostic mode rather than the clean deblur default.
- **edgecompress** fits a continuous 5×5 colour-axis/spatial-edge model, but the current version is deliberately conservative: it requires a coherent two-colour patch, uses averaged side colours instead of arbitrary farthest endpoints, clamps to the local premultiplied-linear channel range, and caps the blend amount. This greatly reduces invented colours/shapes, but also means it will refuse corners and many crossings.
- **deblurcompress** is no longer based on `compress`/`safecompress`. It is now an iterative inverse-filter/back-projection mode: initialize a high-resolution premultiplied-linear image, box-downsample it to the source resolution, compare against the actual source, back-project the residual, apply bounded unsharp preconditioning, clamp to the local source colour range, and repeat. The back-projection is weighted by two-colour edge confidence, so crossings/multicolour junctions are corrected much more cautiously to reduce JPEG/video-codec-like invented colours.

`--auto-blurcompress` defaults to `deblurcompress` and automatically selects `--blur-radius` and `--strength` for the current input. Because a single low-resolution image has no true high-resolution ground truth, "optimal" here means optimal under a self-supervised validation proxy: the program downscales the input by 2× in premultiplied-linear RGBA, reconstructs it with a grid of blur/strength candidates, and picks the pair with the lowest premultiplied-linear RGBA MSE against the original input. The selected parameters are printed to stderr and used for the final upscale.

`--strength N` controls compression for `compress` and `blurcompress`, with `N` in `[1, 100]`. It is a symmetric power-sigmoid exponent: `1` means no narrowing, `2` is the default strong setting, `4` is very strong, and large values deliberately approach hard endpoint selection. The curve remains monotonic and leaves the midpoint fixed for all settings.

`--blur-radius R` controls the source-space Gaussian sigma for `blur` and `blurcompress`, with `R` in `[0.1, 4]` source pixels. The filter radius is `ceil(3R)`, capped at 12 source pixels. `R=.5` is a light anti-jaggedness blur; `R=1` is the old approximate blur amount; larger values are intentionally soft.

**triangle** is a positive-weight 2×2 triangular interpolation reference. It is included to make source-cell faceting visible for comparison, not as a recommended general-purpose mode. `miya_triangle_blur_radii.png` compares it with nearest neighbour and four Gaussian radii.

`miya_compress_crop.png`, `human_compress_comparison.png`, and `miya_blur_then_compress_crop.png` compare these modes against the baseline filters. They are intended for visual judgement at 100%; do not infer quality from a reduced viewer thumbnail.

`evaluate_upscalers.py` now also accepts `EXE:MODE` candidates, so one binary can be compared across lab modes:

```sh
python3 evaluate_upscalers.py ./celup_lab:cubic ./celup_lab:mitchell ./celup_lab:lanczos2 ./celup_lab:lanczos3 ./celup_lab:safecompress
```

The safer compressor added here gates narrowing to high-confidence two-colour ramps. It is *not* RGB morphology or indiscriminate colour dilation, which would shift outlines and destroy thin details.

## Checker/hourglass guard revision

The implementation now incorporates the main external suggestions: two-colour compression is suppressed when a 2×2 cell looks like a checker/saddle instead of a coherent edge; the 5×5 two-colour gate includes a spatial endpoint-flip penalty; Mitchell/Lanczos kernels blend toward bilinear in detected checker cells; the hourglass remover uses the same `-0.5` interpolation-cell alignment as the resamplers and fits residual bases relative to bilinear (`|u-.5|-|v-.5|` plus `(u-.5)(v-.5)`) instead of absolute colour; and the old cubic vertical clamp bug was fixed so clamping occurs after the central-pair range is fully collected.

These changes intentionally make the compressor modes less aggressive in checker/crossing regions. Some MAE numbers for `safecompress` can rise, but the visual goal is fewer bow-tie/hourglass structures and fewer invented crossing colours.

## v2: classifier policy layer (see handoff.md for the full story)

Following the handoff's recommended direction, v2 replaces post-hoc artifact
patching with a single 5x5 patch classifier plus per-class policies:

- **Classifier** (`build_class_map`): per source pixel it fits a colour-axis
  two-colour model, a spatial t-plane, and a competing parity (checker)
  model, and measures exact 2x2 checker cells, endpoint clustering, side
  variance and neighbour label flips. Classes: coherent edge, checker/Nyquist
  ambiguity, junction/crossing, smooth/flat. Staircased anti-aliased diagonal
  edges also alternate labels, so parity must *dominate* the plane model
  before a patch counts as checker -- real diagonal edges stay sharp.
- **adaptive** routes each output pixel by class weights into bounded
  Mitchell / bilinear / checker-policy fallback, then runs 3 gated
  consistency iterations + focused hourglass removal.
- **deblurcompress / dehourglass** use the same precomputed gates: back-
  projection weighted by coherent-edge confidence (near zero in checker
  cells, heavily damped at junctions), unsharp confined to edge territory,
  hourglass removal focused on ambiguous cells. Deblur is ~7.5x faster
  because the 5x5 statistics are evaluated once per source pixel instead of
  for every output pixel of every iteration.
- **classmap** visualises the classifier (R=edge, G=checker, B=junction).
- **hourglass_metric.py** adds quantitative artifact evaluation: fitted
  hourglass/saddle basis amplitude vs a bilinear reference (HG), checker-cell
  MAE (CHK), plus new checker1/checker2/crosshatch/rings torture scenes.

Measured on the torture scenes (details in handoff.md): crosshatch HG drops
from 0.0243 (lanczos3) / 0.0170 (deblur v1) to 0.00105 (deblur v2) and
0.00108 (adaptive); MAE on natural edge scenes stays at or below bilinear
for adaptive and close to deblur v1 for the gated deblurcompress.
