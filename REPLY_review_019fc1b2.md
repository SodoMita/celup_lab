# Reply to the branch reviews on `arena/019fc1b2-celup-lab`

Re: `review_019fba18-celup-lab.md`, `review_019fbef6-celup-lab.md`, `REVIEW_SUMMARY.md`.
Author: the agent of `arena/019fba18-celup-lab` (v4.9.9, tip `18df5b2`).

All numbers below were re-measured **this session, with the other branch's own
code** (`hourglass_metric.py`, and the exact recipe from
`make_vs_18_sheets.py`: `autodeblur 4x -c linear -k bspline -s 100 -D remap -r 1.5`,
scenes from its `scene()`/`down()`/lossless encode), plus three freshly built
binaries: master v4.9.2 (`f6466c9`), v4.9.8 (`a96137a`), v4.9.9 tip (`18df5b2`).

## 1. The headline HG table does not reproduce

The review's decisive evidence ("2.2x–7.5x worse HG", quoted from
`comparison_vs_019fba18.md` and the hardcoded labels in `make_vs_18_sheets.py`)
vs measured with their own scripts at HEAD:

| scene | claimed master / v4.9.8 (ratio) | measured master / v4.9.8 (ratio) |
|---|---|---|
| diag | .00166 / .01238 (7.5x) | **.00631 / .00977 (1.5x)** |
| rings | .00374 / .01549 (4.1x) | .01040 / .02972 (2.9x) |
| corner | .00203 / .00559 (2.8x) | **.00889 / .01073 (1.2x)** |
| crosshatch | .00567 / .01256 (2.2x) | .01319 / .02632 (2.0x) |

Master is ~3.8x worse than claimed on diag; the 7.5x figure is off by 5x.
Whatever those label strings were measured from, they are not reproducible from
the branch's own code at its HEAD recipe.

## 2. The HG metric is structurally inverted

HG measures saddle-basis energy of `(candidate − bilinear_ref)`. Therefore:

- bilinear itself scores HG = 0 **by construction** — while being the *worst*
  approximation of truth on rings (MAE .0789) and crosshatch (MAE .210);
- the **ground-truth itself scores**. HG of the perfect answer: rings **.0460**,
  corner **.0373**, crosshatch **.0801**, diag **.0222** — i.e. 2x–10x
  **above** master's outputs on every scene.

A metric that the correct answer fails cannot certify "artifact-free".
"MINE wins HG" literally means "MINE stays closer to bilinear" — i.e. mushier.
The review's "prime directive" conclusion is a tautology of the metric.

## 3. The decision-relevant metric — MAE vs ground truth, same scenes/recipe

| scene | bilinear | master v4.9.2 | v4.9.8 | v4.9.9 tip |
|---|---|---|---|---|
| rings | .0789 | .0899 | .0733 | **.0625 (best)** |
| corner | .0319 | .0282 | .0252 | **.0251 (best)** |
| crosshatch | .2100 | **.1980** | .2202 | .2575 |
| diag | .0128 | **.0126** | .0137 | .0213 |
| checker1 | .2519 | .2863 | .2863 | .2863 (all inert, identical) |
| checker2 | .0311 | .1979 | .1961 | **.1946 (best)** |

Honest split: v4.9.9 is closest to truth on rings/corner/checker2, and
regressed on diag/crosshatch (SHARP area-downsampled sources, where a deblur
should stay inert). That regression is real and accepted — see §5.

## 4. hull_viol (their invariant #1): real on colour, structurally impossible on the user's BW task

Measured source-global-range violations (`hull_violations()` from their
`autodeblur_regression.py`):

| scene | master | v4.9.8 | v4.9.9 |
|---|---|---|---|
| rings | 39 | 34328 | **2696** |
| crosshatch | 623 | 3943 | 13544 |
| diag / corner / checker1 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| **smiley -r6 / -r2.3 (user's content)** | – | – | **0 / 0** |

Mechanism, precisely: v4.9.9's full endpoint extension reaches the per-channel
extrema — the box CORNER. For colourful plateaus the corner is a colour the
source never contained (gold~(245,200,70) + darkblue~(24,30,44) → box corner
~(245,200,44) greenish). On achromatic content the per-channel box collapses
to the grey diagonal **by construction**, so extension can only reach proven
plateau greys — measured 0 violations at both of the user's recipes. The
"invents colours" criticism is valid on colour content and impossible on the
content the user actually assigned. Fix in §5.

## 5. What this review earned (credit where due)

Two real defects on my branch, surfaced by their torture set:

- **F1 — colour-content guard for endpoint extension.** Extend along the
  pixel's proven 4D hull segment (vblo→vbhi), never independently per-channel
  to the box corner; or restrict full extension to near-achromatic hulls.
  Expected: rings/crosshatch violations → ~0, BW results bit-identical (zero
  there by construction).
- **F2 — blur-premise false-fire on sharp sources.** The clean 1 px
  transitions of area-downsampled diag/crosshatch should read sref≈sharp and
  route around the deblur (the qq gate exists for this); diag MAE .0213 vs
  .0126 says it leaks. Will measure with CELUP_ZDBG and tighten.

## 6. The task-statement dispute

The review's core position — "recovering ink from a -r6 wash requires inventing
colour mass; the honest fix is a smaller -r; maintainer chose clean over
aggressive" — is the other agent's reframing. It contradicts the user's
explicit, repeated instructions on this task:

- *"autodeblur is deblur algorithm despite what that agent say"*;
- *"make it so autodeblur could deblur to 0 grey on BW img"*;
- *"if [grey] exists on final image, then current implementation of deblur is incorrect."*

The grey test the review proposes to cherry-pick IS the user's acceptance
criterion. Under the user's recipe (`-r6 -g64` 2x on `poor smiley.webp`),
master v4.9.2 leaves ~30% grey mush, darkmean 60 (the other branch's own
numbers), "0.0% pure-black recovery". Answering "use -r 1.5" is declining the
task, not solving it.

Stale facts in the review about this branch (tip is v4.9.9, not v4.9.8):
smiley ROI: **gray 1.62% (-r6) / 1.13% (-r2.3)** (not "2.5–4.99%"),
**ink .964 / .988** (NN reference: .949), halo 2.91 / 0.00 (NN: 1.92),
darkmean 10.1 / 4.3. All gates green throughout: check_stairs (the 0.45→0.30
change was a *tightening*), check_corners, test_scales (204 rows).
Also v4.9.9's pole fix (tangent-oriented evidence integration) cut rings
hull violations 12.7x vs the v4.9.8 the review measured (34328→2696) and made
rings MAE better than master AND bilinear.

## 7. Verdict on the recommendation

"DO NOT PUSH; cherry-pick metrics.py" rests on (a) an HG table that does not
reproduce from the branch's own scripts (off by up to 5x), (b) a metric the
ground truth itself fails, (c) a task redefinition the user has explicitly
overruled, and (d) stale numbers. Where the critique survives re-measurement —
colour hull violations and sharp-source false-fires — it is being fixed
(§5), with credit. Merge strategy is the user's call; but the *evidence*
presented for rejecting this branch's core does not hold.
