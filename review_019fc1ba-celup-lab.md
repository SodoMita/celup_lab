# Branch Review: `origin/arena/019fc1ba-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fc1ba-celup-lab`
- **Task Group**: Group 1 — Analytical Deblur Mode (`-D analytic` / `method == 3`)
- **Base / Merge-Base**: Direct descendant of `origin/master` (`f6466c9b5d5e49e6ea6dd70ba8b674f4bb102094`)
- **Total Commits Ahead of Master**: 1 unique commit (`20ad551d783590e660bb233fcc115d76101346b8`)
- **File Diff Summary**: 9 files changed, 937 insertions(+), 7 deletions(-)
  - `celup_lab.c`: +303 lines (full-image gradient field narrowing engine)
  - `tests/test_analytic_deblur.py`: +217 lines (automated test suite for analytic deblur)
  - `tests/autodeblur_regression.py`: +230 lines (numeric fingerprint harness)
  - `tests/metrics.py`: +46 lines (smiley metrics helper)
  - `README_lab.md`, `AUTODEBLUR_NOTES.md`, `handoff.md`, `MANIFEST.txt`: Complete documentation
- **Primary Domain**: Adding an opt-in full-image gradient narrowing mode (`-D analytic`, `method == 3`) that traces transitions along their 4D structure-tensor normal to plateau colors `P0, P1` and narrows the ramp mathematically with zero hull violations.

---

## 2. Key Technical Contributions & Architectural Changes

### 3-Phase Full-Image Gradient Narrowing (`autodeblur_analytic_pass`)
1. **PHASE 1 (Produce — Global Plateau Walk)**:
   - Calculates a 4D structure-tensor normal `(NX, NY)` and gradient magnitude for every pixel in a single pass.
   - For every transitioning pixel, traces a global, adaptive walk along `+-normal` (up to `maxR = 16` steps) until the color saturates to a plateau on each side.
   - Extracts two 4-channel premultiplied RGBA plateau colors `P0` and `P1` from the saturated tails, along with the pixel's normalized blend coordinate `U[idx]` in `[0, 1]`.
   - **Why this is unique**: Unlike per-pixel 2x2 or 6x6 local windows (which only see local neighborhood curvature), this global walk traces a wide blurred ramp end-to-end so even tail pixels see both plateaus and get correctly narrowed.

2. **PHASE 2 (Edit — Inverted Steepness / Alpha Retention)**:
   - Implements the requested inverted `-g` steepness semantics:
     - `K = 1`: Maximum deblur (collapse gradient to a single step = quantize).
     - `K > 1`: Progressively less deblur (`K -> infinity` is identity).
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
   - **Zero Hull Violations**: Because `out` is strictly a convex combination of two real sampled colors `P0` and `P1`, color-space invariant #1 holds by construction (measured **0 hull violations** on `cornerstar48`, compared to 57 violations in standard remap).

4. **Automated Test Suite (`tests/test_analytic_deblur.py`)**:
   - Added a 217-line unit test verifying monotone steepening, exact quantization at `K=1`, identity convergence at large `K`, zero hull violations, and flat-region preservation.
   - Verified that `check_corners`, `check_stairs`, and all 204 `test_scales` sweeps remain unchanged and pass.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 1)

This branch is compared directly against **`origin/arena/019fbf78-celup-lab`** (which used a local 2x2 bilinear projection) and **`origin/arena/019fbfb9-celup-lab`** (which added CLI boilerplate and comparison-sheet auto-splitting).

| Evaluation Criterion | `origin/arena/019fc1ba` (`full gradient narrowing`) | `origin/arena/019fbf78` (`2x2 bilinear projection`) | `origin/arena/019fbfb9` (`stub & sheet split`) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **Mathematical Accuracy to Spec** | **Exact match to request**: Global normal walk to plateau colors `P0, P1`, inverted K semantics (`alpha = (K-1)/K`), convex sampling. | **Local unsharp behavior**: Uses local 2x2 bilinear projection and local S-curve mapping that behaves like another `autoblurcompress`. | **Delegation stub**: Routes `method=3` to standard `autodeblur_pass` without custom analytical math. | **`019fc1ba` is the undisputed winner** on mathematical correctness and algorithm design. |
| **Color Hull Violations** | **0 hull violations** by construction (convex combination of `P0` and `P1`). | Can introduce mixed-pixel/sawtooth artifacts on thin contours if safety gates are bypassed. | Same as baseline autodeblur. | **`019fc1ba` wins on color safety**. |
| **Automated Test Suites** | Adds **`tests/test_analytic_deblur.py`**, `autodeblur_regression.py`, and `metrics.py`. | Relies on existing tests; updates `check_stairs.py`. | None. | **`019fc1ba` wins on test coverage**. |
| **Comparison Sheet Generation** | **Missing**: Did not update `make_lab_comparison_sheets.py` or commit visual comparison sheets to `comparison_sheets/`. | Converted comparison sheets to lossless WebP. | Added **auto-splitting** for oversized combined WebP sheets (`>16383px`) and added `-D analytical` to sheet generator. | **`019fbfb9` wins on sheet generation**, explaining why `019fc1ba` was overlooked visually. |

### Quantitative & Qualitative Assessment
- **Why `019fc1ba` was missed during visual review**: While `019fc1ba` delivered the complete mathematical specification for `-D analytic` and verified it through automated Python unit tests (`test_analytic_deblur.py`), it did not hook `-D analytic` into `make_lab_comparison_sheets.py` or generate visual `.webp`/`.png` sheets. When reviewing branches via visual comparison sheets, `019fc1ba`'s output was invisible.
- **Why `019fbf78` fell short mathematically**: As noted in user feedback, `019fbf78`'s local 2x2 bilinear projection and local S-curve mapping compressed local gradient transitions in a manner indistinguishable from another `autoblurcompress`. `019fc1ba` avoids this by tracing transitions across their entire width up to 16 pixels along the structure-tensor normal.

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Implements the true 3-phase full-image gradient narrowing algorithm (`-D analytic`).
  - Zero color hull violations and exact inverted steepness semantics (`1 = quantize/max deblur`).
  - Self-contained in a single, surgical commit (`20ad551`) on top of `master` with extensive unit test coverage.
- **Weaknesses**:
  - Lacked integration with `make_lab_comparison_sheets.py`, meaning visual inspection required running manual CLI commands.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **RECOMMENDED FOR MASTER (PUSH CORE MATH `20ad551` + PAIR WITH `019fbfb9` SHEETS)**
- **Justification**:
  1. Commit **`20ad551`** from `origin/arena/019fc1ba-celup-lab` is the **definitive mathematical winner** for Task Group 1 (Analytical Deblur Mode). It must be pushed/cherry-picked into `master`.
  2. To resolve `019fc1ba`'s lack of comparison sheets, cherry-pick commit **`1293c23`** from `origin/arena/019fbfb9-celup-lab` (which updates `make_lab_comparison_sheets.py` to support analytical mode and auto-split large lossless WebP sheets).
  3. Reject `019fbf78-celup-lab`'s C implementation (`method == 3`), as its localized 2x2 bilinear projection is superseded by `019fc1ba`'s global normal walk.
