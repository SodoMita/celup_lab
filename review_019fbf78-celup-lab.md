# Branch Review: `origin/arena/019fbf78-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fbf78-celup-lab`
- **Task Group**: Group 1 — Analytical Deblur Mode (`-D analytical` / `method == 3`)
- **Base / Merge-Base**: Diverged from `origin/master` (`f6466c9b5d5e49e6ea6dd70ba8b674f4bb102094`)
- **Total Commits Ahead of Master**: 10 unique commits
- **File Diff Summary**: 14 files changed, 72 insertions(+), 20 deletions(-) (relative to parent commit in PR series; full diff vs master spans 20+ files including WebP asset migration).
- **Primary Domain**: Attempting to add a new whole-image consistent analytical deblur method (`-D analytical`, `method == 3`) using S-curve mapping and a local 2x2 bilinear-projected linear deblur.

---

## 2. Key Technical Contributions & Architectural Changes

### Local 2x2 Bilinear-Projected Linear Deblur (`method == 3`)
1. **Mathematical Model & Why It Degenerated into "Another `autoblurcompress`"**:
   - Implements `--deblur-method analytical` (`-D analytical`), mapped internally to `method == 3`.
   - In commit `875aa4b` and `3fa9b93`, attempted to solve analytical deblur using a local 2x2 bilinear projection and crease-free S-curve mapping (`phi1(k * z0)`).
   - **Why it behaved like `autoblurcompress`**: Instead of producing a full-image gradient field and tracing transitions along their structure-tensor normal across their entire width to real plateau colors `P0, P1` (as required by the analytical spec), `019fbf78` applied a localized 2x2 bilinear projection and local S-curve steepening on the neighborhood pixels. Mathematically, local unsharp projection on existing blurred ramps acts like a local compression filter—creating a visual effect nearly identical in character to `autoblurcompress`.

2. **Bypassing Case-Specific Safety Gates (`--no-safety-gates`)**:
   - Added a CLI flag `--no-safety-gates` and conditional logic inside `autodeblur_pass`:
     ```c
     float coh = (disable_safety_gates || method == 3) ? 1.f : (1.f - ss01((rho - .10f) * (1.f / .20f)));
     ```
   - For `method == 3`, `coh` is forced to `1.f`, disabling junction and plateau safety fallbacks.

3. **WebP Lossless Comparison Sheets**:
   - Converted large generated comparison sheets in `comparison_sheets/` from PNG to lossless WebP (`comparison_sheet.webp`, `comparison_rings.webp`, etc.), cutting repository asset footprint.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 1)

This branch is evaluated directly against **`origin/arena/019fc1ba-celup-lab`** (the true full-image gradient narrowing implementation) and **`origin/arena/019fbfb9-celup-lab`** (the sheet-splitting and CLI branch).

| Evaluation Criterion | `origin/arena/019fbf78` (`2x2 bilinear projection`) | `origin/arena/019fc1ba` (`full gradient narrowing`) | `origin/arena/019fbfb9` (`stub & sheet split`) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **Algorithm Compliance to Spec** | **Local unsharp / compress behavior**: Uses local 2x2 bilinear projection and S-curve mapping; degenerates into another `autoblurcompress`. | **True full-image gradient narrowing**: Traces transitions along 4D normal to plateau colors `P0, P1` (`maxR=16` walk) with inverted K semantics and convex sampling. | **Delegation stub**: Routes `method=3` to standard `autodeblur_pass`. | **`019fc1ba` wins decisively** on mathematical correctness and specification fidelity. |
| **Color Hull Violations** | Can introduce mixed-pixel/sawtooth artifacts on thin contours when safety gates are bypassed. | **0 hull violations** by construction (convex combination of real plateau colors `P0` and `P1`). | Same as baseline autodeblur. | **`019fc1ba` wins on color safety**. |
| **Test Coverage** | Modifies `check_stairs.py` thresholds. | Adds **`tests/test_analytic_deblur.py`**, `autodeblur_regression.py`, and `metrics.py`. | None. | **`019fc1ba` wins on unit testing**. |
| **Comparison Sheet Visibility** | Prunes comparison modes and outputs lossless WebP sheets. | **Missing**: Did not update `make_lab_comparison_sheets.py` (why it was overlooked visually). | Adds **auto-splitting** for oversized WebP sheets (`>16383px`) and includes `-D analytical`. | **`019fbfb9` and `019fbf78` win on visual assets**, compensating for `019fc1ba`'s missing sheets. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Successfully migrated heavy comparison sheets to compact lossless WebP.
  - Added the `--no-safety-gates` CLI option for experimental testing.
- **Weaknesses**:
  - **Fundamental algorithmic misstep**: By relying on local 2x2 bilinear projection rather than global normal tracing to plateau colors, the deblur method created another `autoblurcompress` rather than an analytical gradient narrowing filter.
  - Entangled commit history with experimental `v4.9.9 WIP` autodeblur commits (`019fba18`).

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **DO NOT PUSH TO MASTER (REJECT MATH IN FAVOR OF `019fc1ba`; CHERRY-PICK ASSETS ONLY)**
- **Justification**:
  1. Reject `019fbf78-celup-lab`'s C implementation of `method == 3` (`875aa4b`, `3fa9b93`). As confirmed by comparative analysis, its local 2x2 bilinear projection degenerates into another `autoblurcompress` and fails to achieve true full-image gradient narrowing.
  2. For the core mathematical implementation of `-D analytic` / `-D analytical`, push **`origin/arena/019fc1ba-celup-lab`** (`commit 20ad551`) to `master`.
  3. **Cherry-pick exception**: The lossless WebP asset conversions in `comparison_sheets/` (`f52fc6f`) can be cherry-picked if needed alongside `019fbfb9`'s sheet auto-splitter.
