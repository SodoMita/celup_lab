# Branch Review: `origin/arena/019fba18-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fba18-celup-lab`
- **Task Group**: Group 2 — Autodeblur Core Evolution & Diagnostics (`v4.9.3` – `v4.9.9`, `-D` autodeblur)
- **Base / Merge-Base**: Root branch history starting from commit `10338e8` (v3) through `84ddd1a` (v4.9.2), with 9 unique commits advancing autodeblur to v4.9.9.
- **Total Commits Ahead of v4.9.2**: 9 unique commits (`18df5b2`, `5d5f7a8`, `a96137a`, `846d0ae`, `1a433ae`, `fc2d0ed`, `36b2bbf`, `41d7896`, `b063738`).
- **Primary Domain**: Iterative mathematical development of `--mode autodeblur` (`-D` deblur options), focusing on stroke interior filling, thin-line amplitude restoration, skirt transport, and black-ink recovery on black-and-white (BW) line art.

---

## 2. Key Technical Contributions & Architectural Changes

### Autodeblur Mathematical Pipeline (v4.9.3 – v4.9.9)
1. **Accumulate-Mass Deblur & Stroke Interior Filling (`v4.9.4`)**:
   - Implemented mass-correct restoration depth and stroke interior filling (`commit 41d7896`), raising membership gate threshold from `0.78` to `0.70`.
   - On the `poor smiley.webp` user test (`-r 6 -g 64`), improved ink density retention inside strokes from `0.879` to `0.912`.

2. **Mass-Conserving Deblur Depth (`v4.9.5`)**:
   - Formulated restoration factor as:
     ```
     f = k * (w_src + 2.83 * sig) / (k * w_src + 2.83 * sig)
     ```
   - Resolved mouth-line doubling in `miya_normal` 4x upscaling and reduced jump95 step errors on `ship2x` from `0.263` to `0.089`.

3. **Value-Gated Skirt Transport (`v4.9.6`) & Own-Line Plateau Transport (`v4.9.7`)**:
   - Added carrier skirt transport to move mid-grey veil mass into adjacent dark lines (`fc2d0ed`).
   - Fixed a severe black-ring regression from v4.9.6 by restricting plateau transport taps (`846d0ae`): taps are no longer color sources, movement is strictly constrained along the pixel's own step line, and foreign hue contamination is eliminated.

4. **The Grey Test & Erf-Gain Post-Map (`v4.9.8` – `v4.9.9`)**:
   - Added `tests/metrics.py` ("the grey test") to quantify the fraction of pixels neither near-black nor near-white.
   - Introduced an erf-gain post-map on finished colors (`a96137a`) and tangent-oriented evidence integration (`18df5b2`) to eliminate XY-pole blur anisotropy.
   - Achieved dramatic quantitative ink recovery on `poor smiley.webp` at `-r 6`:
     - Mid-gray mush (`gray%`) dropped from **17.6% -> 2.5%** (vs ~30% in baseline v4.9.2).
     - Pure ink fraction (`<128`) increased from **0.861 -> 0.960**.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 2)

This branch represents the aggressive algorithmic push for autodeblur, contrasted directly against **`origin/arena/019fbef6-celup-lab`** (diagnostic/stability focus) and **`origin/arena/019fbcda-celup-lab`** (hybrid integration).

| Evaluation Metric / Feature | `origin/arena/019fba18` (`v4.9.9`) | `origin/arena/019fbef6` (`master` core) | `origin/arena/019fbcda` (`hybrid`) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **BW Ink Recovery (`ink <128`)** | **0.957 – 0.960** (excellent black-ink density on `-r 6 -g 64` line art). | **0.861** (conservative model leaves `-r 6` grey wash intact). | **0.861** (reverted to conservative ERF profile fit). | **`019fba18` wins on ink density** for heavy-blur recipes. |
| **Mid-Grey Mush (`gray%`)** | **2.5% – 4.99%** (sharp black/white bimodal histogram). | **29.98%** (retains gradual blur transition). | **~30%** (conservative envelope clamp). | **`019fba18` wins on contrast**. |
| **Hourglass / Ringing Artifacts (`HG` metric, lower=better)** | **Severe ringing penalty**: <br>• Diagonal: `0.01238` (**7.5x worse**)<br>• Rings: `0.01549` (**4.1x worse**)<br>• Crosshatch: `0.01256` (**2.2x worse**) | **Cleanest rendering**: <br>• Diagonal: `0.00166`<br>• Rings: `0.00374`<br>• Crosshatch: `0.00567` | **Clean rendering**: <br>Explicitly **reverted** mass-conserving depth (`82b5ffc`) due to SDF halos. | **`019fbef6` and `019fbcda` win decisively on artifact avoidance**. |
| **Architectural Stability** | High algorithmic complexity; required multiple follow-up commits to patch black-ring and double-line regressions. | Stable, bounded hull clamp; zero ringing vocabulary. | Reverted unstable depth math; added stable hybrid classifier. | **Conservative branches win on maintainability and stability**. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Unmatched black-ink recovery on washed-out, high-radius (`-r 6`) black-and-white line art.
  - Comprehensive mathematical exploration of mass conservation and skirt transport.
  - Added valuable test harnesses (`tests/metrics.py` grey test).
- **Weaknesses**:
  - **Severe artifact trade-off**: In order to reconstruct dark ink from a `-r 6` grey wash, the mass-conserving depth and erf-gain post-map invent color mass, causing noticeable ringing, corner rounding, and snake-tongue artifacts across curves and lattices (quantified by `019fbef6`'s head-to-head analysis as 2.2x to 7.5x worse HG metric).

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **DO NOT PUSH TO MASTER (REJECT CORE ARCHITECTURE; CHERRY-PICK METRICS ONLY)**
- **Justification**:
  1. The core architectural changes (`mass-conserving depth`, `skirt transport`, and `erf-gain post-map`) should **not** be pushed to `master`. As proven in `019fbef6` and confirmed by `019fbcda`'s explicit reversion, these mechanisms violate `celup_lab`'s prime directive: **clean, artifact-free rendering over aggressive contrast invention**.
  2. Recovering ink from an over-blurred `-r 6` recipe is fundamentally better solved by reducing the user's input blur radius (`-r 1.5`) rather than introducing a ringing vocabulary into the deblur engine.
  3. **Cherry-pick exception**: The test script `tests/metrics.py` (the grey test evaluator) is a valuable diagnostic tool and should be preserved in the benchmark suite.
