# Handoff: `celup_lab` upscale/hourglass investigation

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
