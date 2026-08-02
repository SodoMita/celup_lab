# Branch Review: `origin/arena/019fbf78-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fbf78-celup-lab`
- **Task Group**: Group 1 — Analytical Deblur Mode (`-D analytical` / `method == 3`)
- **Base / Merge-Base**: Diverged from `origin/master` (`f6466c9b5d5e49e6ea6dd70ba8b674f4bb102094`)
- **Total Commits Ahead of Master**: 10 unique commits
- **File Diff Summary**: 14 files changed, 72 insertions(+), 20 deletions(-) (relative to parent commit in PR series; full diff vs master spans 20+ files including WebP asset migration).
- **Primary Domain**: Adding a new whole-image consistent analytical deblur method (`-D analytical`, `method == 3`) with inverted steepness semantics (`1 = max deblur`), S-curve mapping, and 2x2 bilinear-projected linear deblur.

---

## 2. Key Technical Contributions & Architectural Changes

### Core Algorithm: True 2x2 Bilinear-Projected Linear Deblur (`method == 3`)
1. **Mathematical Model & Inverted Semantics**:
   - Implements `--deblur-method analytical` (`-D analytical`), mapped internally to `method == 3`.
   - Inverts traditional steepness semantics: `deblur_steepness == 1.0` represents maximum deblur strength (pushing to a single point), whereas higher values (`> 1.0`) progressively reduce deblur intensity.
   - Reconstructs images as linear gradients of 4 premultiplied RGBA endpoint colors, avoiding alpha-premultiplied blending artifacts.

2. **Crease-Free S-Curve Mapping & Contrast Range Gating**:
   - In commit `3fa9b93`, optimized the analytical deblur using a crease-free S-curve mapping (`phi1(k * z0)` with dynamic steepness scaling `k_ana = 1e5f` when `K <= 1.0001f`).
   - Added robust contrast range gating to prevent glass-sawtooth mixed-pixel artifacts along diagonal step transitions.

3. **Bypassing Case-Specific Safety Gates (`--no-safety-gates`)**:
   - Added a CLI flag `--no-safety-gates` and conditional logic inside `autodeblur_pass`:
     ```c
     float coh = (disable_safety_gates || method == 3) ? 1.f : (1.f - ss01((rho - .10f) * (1.f / .20f)));
     ```
   - For `method == 3`, `coh` is forced to `1.f`, enforcing uniform contours across the image without triggering localized junction or plateau safety fallbacks.

4. **WebP Lossless Comparison Sheets**:
   - Converted large generated comparison sheets in `comparison_sheets/` from PNG to lossless WebP (`comparison_sheet.webp`, `comparison_rings.webp`, etc.), significantly cutting repository asset footprint while preserving bit-exact visual evidence.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 1)

This branch shares its primary task with **`origin/arena/019fbfb9-celup-lab`** (which also implemented `-D analytical` and updated comparison sheet generation).

| Evaluation Criterion | `origin/arena/019fbf78-celup-lab` | `origin/arena/019fbfb9-celup-lab` | Comparison Verdict |
| :--- | :--- | :--- | :--- |
| **Algorithm Completeness** | **Full implementation** of true 2x2 bilinear projection, crease-free S-curve mapping, and contrast gating in `celup_lab.c`. | **Stub/Wrapper delegation** (`analytical_deblur_pass()` simply calls `autodeblur_pass(..., 3)` without unique mathematical reconstruction logic). | **`019fbf78` wins decisively** on algorithmic depth and accuracy. |
| **CLI & Option Handling** | Implements `-D analytical` and `--no-safety-gates`. | Implements `-D analytical` argument parsing and help strings. | **`019fbf78` wins** by providing the safety-gate override flag. |
| **Comparison Sheet Script** | Prunes comparison modes to base, nearest, and best-scored methods; outputs lossless WebP. | Adds **auto-splitting** into parts for large combined WebP sheets when exceeding WebP dimension limits (`16383px`). | **`019fbfb9` wins** on sheet generator robustness for oversized composite sheets. |
| **Git History & Complexity** | Contains multiple merges and experimental checkpoints (including `v4.9.9 WIP` commits from `019fba18`). | Clean, surgical 2-commit branch directly on top of `master` (`f6466c9`). | **`019fbfb9` is cleaner**, whereas `019fbf78` has a more complex commit graph. |

### Qualitative & Quantitative Analysis
- **Artifact Resolution**: On staircase and diagonal line tests (`comparison_lines.webp`, `comparison_crossing.webp`), `019fbf78`'s 2x2 bilinear projection eliminates staircase treads and wrong-color-band artifacts that occur in standard `autodeblur` when safety gates disengage on thin contours.
- **Why `019fbfb9` fell short on math**: `019fbfb9` added the CLI option `-D analytical` (`method = 3`), but inside `autodeblur_pass()`, `method == 3` has no dedicated branch in `019fbfb9`—meaning it falls through to standard autodeblur behavior. `019fbf78` is the only branch that actually contains the mathematical engine for method 3.

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Delivers the actual mathematical specification for Analytical Deblur (`method == 3`).
  - Resolves glass-sawtooth mixed-pixel artifacts and staircase quantization by construction.
  - Successfully migrates heavy comparison sheets to compact lossless WebP.
- **Weaknesses**:
  - History is entangled with `origin/arena/019fba18-celup-lab` (`v4.9.9 WIP checkpoint`), which introduces experimental ringing/hourglass regressions from that branch if merged blindly without isolation.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **CHERRY-PICK RECOMMENDED (Core Algorithm from `875aa4b` & `3fa9b93`)**
- **Justification**: The core analytical deblur implementation (`method == 3`, S-curve mapping, 2x2 bilinear projection, and `--no-safety-gates`) from commits **`875aa4b`** and **`3fa9b93`** should be **cherry-picked** into `master`.
- **Merge Guidance**: Do **not** merge the entire branch tip directly into `master` to avoid dragging in the experimental `v4.9.9 WIP` skirt-transport commits (`5d5f7a8`, `a96137a`, `846d0ae`). Instead, extract the `celup_lab.c` analytical deblur logic from `019fbf78` and combine it with the clean comparison-sheet auto-split generator from `019fbfb9`.
