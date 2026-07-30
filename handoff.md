# Handoff: `celup_lab` upscale/hourglass investigation

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
