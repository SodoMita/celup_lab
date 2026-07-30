# celup_lab: comparable baseline modes

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
- **sdf** (v4) is the smooth-geometry sharp option: it extracts a signed
  distance field of every confidently measured edge mid-contour (from the
  classifier's t-planes, propagated by a vectorial distance transform),
  upsamples the field, and re-thresholds the local two-colour transition
  against it. Staircases straighten into smooth contours (0.5px weave ->
  chord), transition width stays as measured (mildly narrowed by
  `--strength`), and far from measured edges it falls back to the bounded
  Mitchell base. It removes hourglass/bow-tie structure by construction
  (single smooth iso-crossing, no parity alternation) and is the cleanest
  8x line-art upscale: crosshatch HG 0.0052 (lanczos3 0.0243).
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

`--checker-policy` only affects `adaptive`. `--adaptive-debug N` (0..15) is
a development aid that zeroes selected class weights (bit 1 = edge,
bit 2 = checker, bit 4 = junction, bit 8 = thin-line) to attribute error to
policy branches. `CELUP_CLASS_DEBUG=1` in the environment prints thin-line
detection details to stderr.

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
