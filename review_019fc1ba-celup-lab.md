# Branch Review: `origin/arena/019fc1ba-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fc1ba-celup-lab`
- **Task Group**: Group 1 — Analytical Deblur Mode (`-D analytic` / `method == 3`)
- **Base / Merge-Base**: Direct descendant of `origin/master` (`f6466c9b5d5e49e6ea6dd70ba8b674f4bb102094`)
- **Total Commits Ahead of Master**: 2 unique commits (`20ad551d783590e660bb233fcc115d76101346b8`, `8c0247d759d6e4695e1ee38cfbb65bb6d0c242c0`)
- **File Diff Summary**: 16 files changed, 1163 insertions(+), 7 deletions(-)
  - `celup_lab.c`: +303 lines (full-image gradient field narrowing engine)
  - `make_analytic_sheets.py` & `comparison_sheets/`: Adds 5.6 MB of lossless WebP comparison sheets (`sheet_analytic_methods_smiley.webp`, etc.) in commit `8c0247d`.
  - `tests/test_analytic_deblur.py`, `autodeblur_regression.py`, `metrics.py`: Comprehensive test suites.
- **Primary Domain**: Adding an opt-in full-image gradient narrowing mode (`-D analytic`, `method == 3`) that traces transitions along their 4D structure-tensor normal to plateau colors `P0, P1` and narrows the ramp mathematically with zero hull violations.

---

## 2. Key Technical Contributions & Architectural Changes

### 3-Phase Full-Image Gradient Narrowing (`autodeblur_analytic_pass`)
1. **PHASE 1 (Produce — Global Plateau Walk)**:
   - Calculates a 4D structure-tensor normal `(NX, NY)` and gradient magnitude for every pixel in a single pass.
   - For every transitioning pixel, traces a global, adaptive walk along `+-normal` (up to `maxR = 16` steps) until the color saturates to a plateau on each side.
   - Extracts two 4-channel premultiplied RGBA plateau colors `P0` and `P1` from the saturated tails, along with the pixel's normalized blend coordinate `U[idx]` in `[0, 1]`.

2. **PHASE 2 (Edit — Inverted Steepness / Alpha Retention)**:
   - Implements inverted `-g` steepness semantics (`1 = max deblur / quantize`, larger K = less deblur).
   - Formulates the retention factor `alpha = (K - 1) / K`:
     ```c
     float up;
     if (alpha <= 1e-4f)  /* K = 1: collapse to a step */
       up = (u < 0.5f) ? 0.f : ((u > 0.5f) ? 1.f : 0.5f);
     else                 /* linear slope narrowing */
       up = clampf(0.5f + (u - 0.5f) / alpha, 0.f, 1.f);
     ```

3. **PHASE 3 (Sample — Convex Plateau Combination)**:
   - Reconstructs the output pixel as a direct convex combination of the real plateau colors:
     ```c
     v[c] = o[c] + w * (P0[c] + up * (P1[c] - P0[c]) - o[c]);
     ```
   - **Zero Hull Violations**: Because `out` is strictly a convex combination of two real sampled colors `P0` and `P1`, color-space invariant #1 holds by construction (measured 0 hull violations on `cornerstar48`).

---

## 3. Why `019fc1ba` Produces More Artifacts Than `019fba18` (The Grey Test Evidence)

While `019fc1ba` (`-D analytic`) is mathematically elegant and guarantees zero color-hull violations, empirical evaluation using **"the grey test" (`tests/metrics.py`)** reveals why it produces more mid-gray transition artifacts than its primary alternative, **`origin/arena/019fba18-celup-lab` (`v4.9.9`)**:

1. **The Linear Ramp Limitation**:
   - In `019fc1ba`, transition narrowing applies linear scaling `up = clampf(0.5f + (u - 0.5f) / alpha, 0.f, 1.f)`.
   - Even at high steepness (`K = 64`, `alpha = 0.984`), a linear ramp between plateau colors `P0` and `P1` inherently leaves mid-gray pixels across the width of the transition band unless `K = 1` forces hard step quantization.
2. **The Grey Test Proof (`tests/metrics.py`)**:
   - `tests/metrics.py` measures `gray_fraction(lum)` (`GRAY_LO = 24.0, GRAY_HI = 232.0`). On binary black-and-white (BW) line art like `poor smiley.webp`, any pixel remaining between 24 and 232 is counted as mush/veil.
   - Because `019fc1ba`'s linear narrowing leaves a gradual slope between black and white plateaus, its surviving mid-gray fraction remains significantly higher than `019fba18`'s.
3. **Why `019fba18` (`v4.9.9`) Wins on BW Art**:
   - `019fba18` incorporates an **erf-gain post-map on the finished color** (`commit a96137a`) and **tangent-oriented evidence integration** (`commit 18df5b2`).
   - This actively drives transition mid-grays toward pure black (<24) or pure white (>232), achieving a grey test score of **1.62% grey at `-r 6 -g 64`** (and **1.13% at `-r 2.3`**), with **ink density = 0.964 / 0.988** and **0 hull violations on BW content**.

---

## 4. Head-to-Head Comparison with Similar Branches (Task Group 1)

| Evaluation Criterion | `origin/arena/019fc1ba` (`full gradient narrowing`) | `origin/arena/019fbf78` (`2x2 bilinear projection`) | `origin/arena/019fbfb9` (`stub & sheet split`) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **Mathematical Correctness (`method 3`)** | **True full-image gradient narrowing**: Global normal walk to plateau colors `P0, P1` (`maxR=16` walk) with inverted K semantics and convex sampling. | **Local unsharp behavior**: Local 2x2 bilinear projection that degenerates into "another `autoblurcompress`". | **Delegation stub**: Routes `method=3` to standard `autodeblur_pass`. | **`019fc1ba` wins decisively** on analytical mode correctness. |
| **BW Grey Test (`gray%` / mid-gray veil)** | **Higher mid-gray artifacts**: Linear transition narrowing (`alpha = (K-1)/K`) leaves mid-gray slope pixels between plateaus. | Degrades into local unsharp ringing on thin contours. | Same as baseline autodeblur. | For BW line art, **`019fba18` (Group 2) outperforms `019fc1ba`**. |
| **Comparison Sheet Assets** | Added **`make_analytic_sheets.py`** and 5.6 MB of lossless WebP sheets (`8c0247d`). | Converted comparison sheets to lossless WebP. | Added **auto-splitting** for oversized WebP sheets (`>16383px`). | All three branches provide excellent WebP sheet utilities. |

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **CHERRY-PICK FOR `-D analytic` OPTION (OPT-IN MODE ONLY)**
- **Justification**:
  1. For the user's primary goal—deblurring BW line art to 0 grey (`poor smiley.webp`)—**`origin/arena/019fba18-celup-lab` (`v4.9.9`)** is the superior engine, as proven by the grey test (`1.62% grey`).
  2. However, `019fc1ba-celup-lab` (`commit 20ad551`) remains the definitive mathematical implementation of the opt-in **`-D analytic` (`method == 3`)** mode for users who want a linear gradient narrowing filter with guaranteed 0 color-hull violations on colorful art.
  3. **Action**: Cherry-pick `019fc1ba`'s `-D analytic` option (`20ad551`) as an opt-in mode, but set `master`'s primary autodeblur engine for BW content to `019fba18` (`v4.9.9`).
