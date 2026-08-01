# Head-to-head: this branch (arena/019fbef6) vs arena/019fba18 (v4.9.8)

User asked: "arena/019fba18 also does autodeblur, but added too much artifacts
to it and added grey test for bw image. from sheets ill see how well you did."

This is the measured comparison. Two labeled PNG sheets accompany it:
- `docs/sheet_artifacts.png` — torture scenes, [bilinear | MINE | v4.9.8]
- `docs/sheet_bw_art.png`    — BW/grey content, [nearest | MINE | v4.9.8]

## What 019fba18 (v4.9.8) actually changed
It kept the mass-conserving deblur depth (v4.9.5, `f = k(wsrc+2.83sig)/(k wsrc+2.83sig)`)
that the **shared** branch reverted, then added v4.9.6 value-gated skirt
transport, v4.9.7 own-line plateau transport, and **v4.9.8 erf-gain post-map**
(its "grey test" fix: a post-map on the finished colour to kill mid-grey on
BW art). It also added `tests/metrics.py` (the grey test: fraction of pixels
neither near-black nor near-white) and recalibrated `check_stairs` (0.45→0.30).

## The two axes (they trade against each other)

### 1. ARTIFACTS — the user's complaint. MINE wins by a wide margin.
Hourglass/ringing energy (HG, lower = cleaner) on the 4x torture set:

| scene | MINE HG | v4.9.8 HG | v4.9.8 penalty |
|-------|---------|-----------|----------------|
| diag | 0.00166 | 0.01238 | **7.5x** worse |
| rings | 0.00374 | 0.01549 | **4.1x** worse |
| corner | 0.00203 | 0.00559 | **2.8x** worse |
| crosshatch | 0.00567 | 0.01256 | **2.2x** worse |

The mass-conserving depth + skirt transport + erf-gain post-map recover
contrast by moving colour mass around, and that movement leaves strong
hourglass/ringing on every edge and curve. Visible in `sheet_artifacts.png`.
MINE (master v4.9.2 core) keeps the anchored, hull-clamped model that has
no ringing vocabulary.

### 2. GREY TEST (BW ink recovery) — what 019fba18 optimized for. v4.9.8 wins.
019fba18's own `tests/metrics.py` on the smiley 2x user recipe (-r6 -g64),
ROI rows 240:500:

| metric | MINE | v4.9.8 | meaning |
|--------|------|--------|---------|
| ink (frac <128) | 0.861 | **0.957** | v4.9.8 recovers more ink |
| darkmean | 60.0 | **11.4** | v4.9.8 ink is near-pure-black; MINE washed grey |
| gray% | 29.98 | **4.99** | v4.9.8 has far less mid-grey mush |
| halo (skirt) | 19.74 | 8.99 | v4.9.8 skirt is darker (its transport) |

Pure-BW test 4x -r6 -g64: MINE recovers 0.0% pure-black ink (the -r6 blur
washes the 3px lines to grey and the clean deblur cannot invent depth back);
v4.9.8 recovers 17.7%. Visible in `sheet_bw_art.png`.

## Why this is the documented, chosen trade-off (not a bug in MINE)
The handoff (`AUTODEBLUR_NOTES.md` "ALREADY TRIED AND REJECTED") lists the
base-sigma decouple AND the mass-conserving depth exactly: both recover
washed ink/contrast AND both add the artifacts the user then rejects
(corner rounding, snake-tongue, treads, and now the v4.9.8 hourglass band).
MINE = the maintainer's chosen side of that trade: **clean over aggressive**.
Recovering pure-black ink from a -r6 grey wash fundamentally requires
inventing colour mass, which is the ringing vocabulary the clean model
forbids. The honest fix for washed -r6 ink is a smaller -r, not a sharper
deblur.

## MINE's opt-in `-T` and the grey test
`-T` (texgain) does NOT recover the smiley ink (darkmean stays ~60 across
-T 0..0.6): the grey lives in flat washed plateau interiors, and -T's flat
guard correctly leaves flats alone. -T only crisps directed lattice texture
(checker/crosshatch), which is its intended use. So -T cannot paper over the
-r6 ink-wash — and it shouldn't (it would be the same artifact trade).

## Bottom line
- If the priority is **no artifacts on edges/curves** (your stated verdict on
  019fba18): MINE is clearly better (2-7x lower HG).
- If the priority is **pure-ink recovery on heavily-blurred BW art**: v4.9.8
  is better on that one metric, at the cost of the artifacts above.
- You can't have both with a -r6 base; the maintainer chose clean.
