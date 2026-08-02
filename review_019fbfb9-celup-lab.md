# Branch Review: `origin/arena/019fbfb9-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fbfb9-celup-lab`
- **Task Group**: Group 1 — Analytical Deblur Mode (`-D analytical` / `method == 3`)
- **Base / Merge-Base**: Direct descendant of `origin/master` (`f6466c9b5d5e49e6ea6dd70ba8b674f4bb102094`)
- **Total Commits Ahead of Master**: 2 unique commits (`1293c23`, `ed6ae66`)
- **File Diff Summary**: 2 files changed, 41 insertions(+), 4 deletions(-)
  - `celup_lab.c`: +27 / -2 lines
  - `make_lab_comparison_sheets.py`: +18 / -2 lines
- **Primary Domain**: Adding `-D analytical` CLI support and enhancing the comparison sheet generator to output lossless WebP with automatic sheet splitting for large combined layouts.

---

## 2. Key Technical Contributions & Architectural Changes

### Clean CLI & Comparison Sheet Generator Architecture
1. **CLI Flag & Documentation**:
   - Updates `--help` output and argument parser in `celup_lab.c` to accept `--deblur-method analytical` and `-D analytical`, setting `deblur_method = 3`.
   - Documents inverted steepness semantics in code comments and user help strings (`1 = max deblur`, higher values reduce deblur).

2. **Delegation Structure (`analytical_deblur_pass`)**:
   - Introduces a clean wrapper function:
     ```c
     static int analytical_deblur_pass(uint8_t *out, int dw, int dh, float scale) {
       return autodeblur_pass(out, dw, dh, scale, 3);
     }
     ```
   - Routes `method == 3` through `autodeblur_pass()`, providing clean CLI plumbing without custom analytical math.

3. **Comparison Sheet Auto-Splitting for Large Layouts (`commit 1293c23`)**:
   - In `make_lab_comparison_sheets.py`, adds intelligent image-dimension checks when generating combined multi-mode sheets.
   - If a composite sheet exceeds WebP's maximum canvas dimension (`16383px` in either width or height), the generator automatically splits the sheet into multi-part WebP files (`sheet_part1.webp`, `sheet_part2.webp`, etc.) rather than crashing or falling back to bloated PNG files.
   - **Crucial Synergy with `019fc1ba`**: Because `origin/arena/019fc1ba-celup-lab` implemented the full mathematics of `-D analytic` but omitted comparison sheet generation, `019fbfb9`'s script upgrade is the exact missing link needed to visualize analytical deblur.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 1)

This branch is evaluated against **`origin/arena/019fc1ba-celup-lab`** (true analytical math) and **`origin/arena/019fbf78-celup-lab`** (local 2x2 bilinear projection).

| Evaluation Criterion | `origin/arena/019fbfb9` (`stub & sheet split`) | `origin/arena/019fc1ba` (`full gradient narrowing`) | `origin/arena/019fbf78` (`2x2 bilinear projection`) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **Algorithmic Math (`method 3`)** | **Placeholder / Delegation**: Calls `autodeblur_pass(..., 3)` without unique mathematical reconstruction logic. | **True Full-Image Gradient Narrowing**: Traces transitions along 4D normal to plateau colors `P0, P1` with inverted K semantics (`alpha = (K-1)/K`). | **Local unsharp behavior**: Local 2x2 bilinear projection that degenerates into another `autoblurcompress`. | **`019fc1ba` wins decisively** on mathematical correctness. |
| **Comparison Sheet Utilities** | Adds **auto-splitting for oversized WebP sheets** (`> 16383px`) and includes `-D analytical` in mode sweep. | **Missing**: Did not update `make_lab_comparison_sheets.py`. | Prunes comparison modes to best-scored methods. | **`019fbfb9` wins** on sheet generator robustness and visual accessibility. |
| **Code Hygiene & History** | **Surgical 2-commit history** directly on top of `master`; zero merge clutter. | Clean single-commit branch (`20ad551`) with extensive unit test suite. | Contains merge clutter and experimental `v4.9.9 WIP` autodeblur commits. | **Both `019fbfb9` and `019fc1ba` win on git hygiene**. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Exceptionally clean, concise diff (+41/-4 lines total).
  - Solves the libwebp 16383-pixel canvas limit with elegant auto-splitting in `make_lab_comparison_sheets.py`.
  - Zero regression risk to existing upscaling modes.
- **Weaknesses**:
  - The C implementation of `-D analytical` is a stub that delegates to existing autodeblur code without the actual analytical reconstruction math.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **CHERRY-PICK RECOMMENDED (Python Sheet Auto-Split `1293c23` + Pair with `019fc1ba` Math)**
- **Justification**:
  1. Cherry-pick commit **`1293c23`** (`make_lab_comparison_sheets.py` auto-splitting for large lossless WebP sheets) into `master` immediately. This resolves why `019fc1ba` was overlooked visually during review by enabling comparison sheets for analytical mode.
  2. For the C implementation of `-D analytic` / `-D analytical`, use `019fbfb9`'s CLI parser strings (`ed6ae66`) paired with the genuine full-image gradient narrowing engine (`autodeblur_analytic_pass`) from **`origin/arena/019fc1ba-celup-lab`** (`20ad551`).
