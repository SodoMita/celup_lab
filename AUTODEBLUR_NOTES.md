# autodeblur: the initial calculations, the improvement queue, and a memo to agent contributors

Audience: anyone (human or agent) touching `upscale_autodeblur` / `autodeblur_pass` in
`celup_lab.c`.  State: master = v4.9.2 (84ddd1a).  Everything below is measured, not
speculated; the git history v4.7..v4.9.2 is the receipt.

**Provenance:** `docs/AUTODEBLUR_GENESIS_v47.txt` is the verbatim design session in
which this algorithm was born (v4.6 user complaints -> per-channel box-corner hue
inversion diagnosis -> the analytic profile plan).  Read it first; it explains WHY
independent per-channel remaps and box windows are forbidden designs, not just that
they are.

Plan-vs-shipped deltas (initial v4.7 plan -> v4.9.2 reality):
- 4D structure tensor / 1 gradient for RGBA: SHIPPED, unchanged.
- line samples along the sub-pixel normal via bilinear sampler: SHIPPED (sample_fn).
- STEP/PULSE two-erf pulse model: EVOLVED -- v4.8 replaced the special-case two-erf
  pulse by the LOBE MAP (segment |\Delta u| into transitions; nearest lobe owns the
  fit; flanks never merge).  More general: handles wedges, 3+ crossings, and absorbs
  the old PULSE branch entirely.
- steepening s -> s/k with anti-sawtooth floor s/k >= .6 px: SHIPPED (k = min(k, s/.6)).
- convex reconstruction n = P0 + u'*(P1-P0) from REAL endpoint colors: SHIPPED; later
  hardened to the pass-2 hull clamp (window min/max, display-quantized).
- convex-combo-only => premultiplied invariant automatic, no post clamp: SHIPPED.
- evaluate the fit at the pixel itself (no box-window value snapping), separate
  dst buffer (no in-place scan coupling): SHIPPED.
- shading gate wclos (only blur-width transitions get steepened): EVOLVED into the
  sa/sb strength mapping + v4.9.1 coverage gate; the v4.9.1 linear term c now absorbs
  shading INSIDE the fit instead of only gating it away.
- moments (mu = first, s = second moment of |\Delta u|): SHIPPED, and v4.9 later
  stabilised mu by tangent consensus (pass 1.5) instead of the more aggressive
  refinement loops that were tried and removed.
- method 2 "push" = evaluate ORIGINAL profile at displaced coordinate
  off = (u-.5)(k-1)*1.5*s: SHIPPED, formula unchanged (z0 + (ufit-.5)(k-1)*1.5).

---

## 1. The initial calculation (what the algorithm actually is)

All processing is linear-light, premultiplied RGBA (sRGB conversions go through the
`to_linear/to_srgb` LUTs at the borders).  Premultiplied matters: interpolation and
alpha commute only in that space; hue cannot invert across a semi-transparent edge
because colors live on 1-vector segments.

**Local 1-vector model.**  In a small window around a pixel, the (4-vector) colors are
assumed to lie on a line segment between two plateau colors `CA, CB`.  The direction
`d = (CB-CA)/|CB-CA|` comes from the top eigenvector of the 4D structure tensor of the
premultiplied colors (one direction for the whole RGBA vector, so no per-channel
disagreement).  Every sampled color `C(z)` along the gradient normal `z` is reduced to
one scalar:

    raw(z) = ((C(z) - CA) . d) / L2,     L2 = |CB-CA|^2
    u(z)   = clamp(raw(z), 0, 1)         (raw kept unclamped since v4.9.1)

**Blur model.**  The observed profile is an ideal unit step convolved with a Gaussian
of width `s` sitting at `mu`:

    u(z) = phi1((z - mu)/s),   phi1(z) = 0.5*(1 + erf(z/sqrt(2)))

`phi1' / phi1` relates measured slope to sigma exactly, which is also how the mode
*estimates* blur radius for `-r auto` (validation-proxy fit in autoblur).

**The deblur (sharpening).**  Rendering the same model at a *narrower* width is an
exact operation on the model: evaluate `nu = phi1(k*z0)` instead of
`ufit = phi1(z0)`, `z0 = (0 - mu)/s` at the pixel's own position (`t=0`), with
steepness `k >= 1` (k = s/0.6 capped, i.e. never claim an output ramp sharper than
0.6 px; `-g K` pins k up to 64, `-e` adapts per edge, `-s` formula otherwise).
Because the window coordinate moves with the pixel (sub-pixel bilinear sampling of
`C(z)` via `sample_fn`), the evaluation is *anchored*: sub-pixel accuracy everywhere,
no re-sampling grid — the "1 px staircase of the fit itself" cannot exist.

**Anchored output (v4.8 form, the anti-halo core):**

    v = o + wS * (nu - ufit) * b * d

`o` = the pixel's own (base-render) color.  Only the difference along `d` between the
steepened and current profile is applied; every deviation of `o` from the model
(shading, texture, noise, dither, hue arcs) passes with **gain 1**.  The v4.7 form
`v = a + b*phi1(k*phi1^-1(u))*d` had color-domain gain `k` at the ramp center — that
is precisely the "outstanding pixels mid-gradient" and "flat-noise halo hugging the
line" the user reported; the anchored form removes it structurally, not by threshold.

**Estimation order per pixel (pass 1):**

1. Structure tensor → normal direction, coherence `coh = 1-ss01((rho-.10)/.20)` with
   `rho = lambda2/lambda1`; tangent span `Teff = T*coh` (0 at junctions/corners).
2. Sample `C[j]` along the normal at sub-pixel positions (bilinear on the base render),
   window half-width `R = max(3, ceil(6*sigma))` capped to the ring buffers.
3. Pick segment extremes `CA, CB`, orient `d` dark→bright (so `u_left < u_right` on
   the two window ends).
4. **Lobe map**: segment `|du/dz|` (and the raw tail weights) into transition lobes,
   single-sample dips bridged, saturated-but-flat plateaus = hard breaks.  The pixel
   fits its NEAREST lobe only, domain clipped at neighbouring lobes; plateau colors
   `P0/P1` = one-sided means of `u<.25 / u>.75`.  This is what stops a thin line's two
   flanks + two backgrounds from averaging into one phantom step (the v4.7
   "snake-tongue"/"combined gradients").
5. `mu, s` from |du| moments on the lobe (`s` clamped to [.3, half-lobe]).
6. Profile fit (v4.9.1): weighted LS of `y = a + c*z + b*phi1(z)` on **raw** —
   a linear baseline takes baked-in shading so `phi1` models only the step; the
   baseline cancels in `(nu-ufit)` and shading rides the gain-1 residual.
7. **Amplitude `b` := one-sided plateau span** `sp = (P1-P0).d/L2`, net-clamped to
   `[ab_b, 1.2*sp]`: the LS's phi/linear basis is near-degenerate on wide soft ramps
   (reads .7*sp at tips → rounding, >2*sp on shaded skirts → flat-top "neon" band);
   the plateau span is the only contrast the window itself proves.
8. **Trust product `wS`** (everything multiplicative, any gate can zero the pixel):
   - `wS = ss01((sb-s)/(sb-sa))` strength→sharpness mapping (contrast gates `-s`),
   - RMSE of the 3-param fit vs raw, `wS *= ss01((trust_hi-rmse)/(trust_hi-trust_lo))`
     (.10/.03) — curved/textured ramps misfit and fade,
   - multi-crossing: mid-level crossings on the whole window (>2.25) fade to 0 —
     dense texture/hair/text suppression,
   - coverage: window profile must straddle the modelled `[a, a+b]` step (kills
     phantom knees on pure shading slopes),
   - `wS *= coh` — the 1D ramp model is invalid at junctions; there the pixel keeps
     the base sample (source-faithful, never a bogus fit).

**Pass 1.5 (contour consensus, v4.9):** the per-pixel fits are stored wS-weighted
(`[wS, wS*mu, wS*s, wS*d2(4), wS*coh, wS*tanx, wS*tany]`) and integrated along the
junction-aware tangent; `z/k/nu` are re-derived from the consensus before evaluation,
so anchored evaluation stays exact while `mu` jitter (1-2 out px near wedges) is
averaged out.  **Pass 2** smooths the *delta* (v-o) tangentially, also junction-aware.

**Final hull clamp:** pass-2 output is clamped into the local window color hull
(per-channel min/max over the fit window), quantized in DISPLAY space (sRGB u8 for
color, linear u8 for alpha): the deblur has *no ringing vocabulary* — it can never
invent a color the window didn't already contain.  This plus gain-1 residual is why
the mode cannot halo.

That's the whole model: `observed = plateau1 + span*phi1((z-mu)/s) + residual`,
render as `plateau1 + span*phi1(k*(z-mu)/s) * wS + residual*1`, everything guarded,
clamped to observed colors.

---

## 2. How to improve it further (honest queue, risk-ranked)

| # | Idea | Expected win | Risk | Notes from measurements |
|---|------|--------------|------|--------------------------|
| 1 | **Texture-class residual gain**: in multi-crossing windows (cross>2.25, currently faded to wS=0), amplify only the high-frequency residual (e.g. gain 1.15 on laplacian component), still hull-clamped | recovers lattice texture (crosshatch HG .00567 → ~.004) with zero change on edges | LOW | the v4.9.2 knob-off matrix proved the whole crosshatch delta is base sigma, not the fit; texture gain buys it back legally |
| 2 | **Curvature/wedge model for corners**: replace "turn the model off at junctions" (coh gating) with an explicit two-arm fit and wedge evaluation | sharp acute tips at strong -r (currently tips ride the blurry base) | HIGH | cornerstar tips currently pass only because coh kills the fit; a wedge model could genuinely sharpen them |
| 3 | **Multi-scale mu consistency**: fit at 0.5x and 1x windows, accept mu only if the two agree < .25 px, else fall back | kills rare mu outliers at low contrast | MED | safer than centroid refinement (tried, measured, REMOVED: it moved mu ~.3px at tips and rounded them: 83.75 vs gate 86) |
| 4 | **SNR-bounded k**: estimate residual noise per window, cap k so the sharpened riser stays < 2*noise sigma | cleaner flats at high -g | MED | mostly covered by rmse trust already; explicit bound lets relatives of `-e` go further safely |
| 5 | Quantization-aware LS: u8 sources (the 45-level smiley class) fit against intervals [q_i, q_i+1] | less mu wobble on quantized sources | MED | matters only at k>8 on ≤6-bit content |
| 6 | Tiled/streamed passes to cut ~92 B/px peak | big images at 4x under smaller -M | LOW | pure engineering |

Not on the list, because measured and rejected (do not retry):
base-sigma decouple (=treads, user-rejected), mu refinement loop (rounds tips),
mu-spread steepness governor (only fires at bending edges, i.e. exactly the geometry
it destroys), deconvolution/RL/USM sharpening anywhere (ringing vocabulary),
MAE-chasing on synthetic torture scenes (rewards reproducing source quantization).

---

## 3. Memo to agents (paste-ready for ARENA_AGENTS.md)

See section "FOR AGENTS" below.

---

## FOR AGENTS -- autodeblur contract (v4.9.2, master@84ddd1a)

autodeblur is NOT a sharpening filter; it is a MODEL (`p1 + span*phi1((z-mu)/s)`
per pixel-window, see docs/AUTODEBLUR_NOTES.md sec.1 and the genesis design doc
docs/AUTODEBLUR_GENESIS_v47.txt) rendered at a narrower
sigma with the residual passed at GAIN 1, and every safety gate below is
load-bearing -- each was added in response to a user-visible artifact, removed,
and re-added after the artifact returned.  "Improving" one number by removing
or loosening a gate trades an artifact back in.

INVARIANTS (must hold in any diff you ship):
1. Output colors never leave the local window hull (display-quantized clamped).
   Deblur has no ringing vocabulary.  If your change can produce a color the
   source window didn't contain, it's wrong by construction.
2. Residual gain stays 1 (no `k*x` amplification of o - model).
3. Base render sigma == -r exactly (the user pins -r as the lattice-hiding
   blur).  No decouple, no crisp fallback, no "effective sigma" tricks.
4. Anchored evaluation: sub-pixel mu estimation only via tangent-consensus or
   averaging -- NEVER by an iterative re-centering loop (measured: rounds tips).
5. (-k/-r/-c/-p/-D/-g/-e) pinning keeps working exactly; every effective value
   echoed to stderr.
6. Any per-pixel multiplier of wS must also work through the pass-1.5 wS
   weighting (consensus) -- raw per-pixel effects reappear otherwise.
7. Trust must be MONOTONE-safe: wS reduction may only ever make output closer
   to the base render (identity = safe degradation).

ALREADY TRIED AND REJECTED (with measurements; do not spend your session
re-discovering these -- check git history v4.9..v4.9.2 for receipts):
- base-sigma decouple (render base at r/min(K,8)): stair treads + speckle +
  forked caps return at the user's -r 6 recipe.  REJECTED BY THE USER.
- step-component centroid refinement: tip extent 83.75 < gate 86.  REMOVED.
- mu-spread steepness governor: fires only at bending edges.  REMOVED.
- monotone-tread merge: withdrawn after knob-off analysis (crosshatch delta
  tracks base sigma, not the fit).
- xBR/Super-xBR family for smooth gradients: 234x worse gradient MSE (own
  measurements on arena/019fba12).  Keep xBR modes pixel-art-only.
- Tuning to MAE / evaluate_upscalers on synthetic scenes: it rewards
  reproducing source quantization.  Artifact-free + user recipe beats MAE.

GATE PROTOCOL (all from repo root, must be green BEFORE you claim progress):
    python3 tests/make_test_sources.py        # regenerate fixtures
    python3 tests/check_corners.py            # hull/tip/width/glow
    python3 tests/check_stairs.py             # 45deg treads, ship+probe
    python3 tests/test_scales.py              # 204 configs
  PLUS eyeball (numbers are necessary, NOT sufficient):
    - poor smiley at: 2 --mode autodeblur --max-mib 1048 -c linear
      -k bspline -r 6 -s 100 -g 64 -D remap
      compare vs master's output; post crops of spike/mouth/neck.
    - miya fixture at the user recipe (-r 2.3 -s 100 -g 8 -D remap):
      cheek HF and blush std must not exceed master's (+5%).
  Your branch's banner say "AGENT-<id>" not a marketing version bump;
  version numbers move on master by the maintainer after review.

WHERE TO LOOK:
  upscale_autodeblur (entry, sigma pinning) -> autodeblur_pass
  (pass 1 fit -> pass 1.5 consensus -> pass 2 delta smooth + hull clamp).
  lsq_profile = the linear+erf fit.  CELUP_DBG=x,y env prints per-pixel
  fit internals; keep it working.
