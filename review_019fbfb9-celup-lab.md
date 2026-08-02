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

### Clean CLI & Function Architecture for Analytical Mode
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
   - Routes `method == 3` through `autodeblur_pass()` without altering existing safety gates or blending pipelines.

3. **Comparison Sheet Auto-Splitting for Large Layouts**:
   - In `make_lab_comparison_sheets.py`, adds intelligent image-dimension checks when generating combined multi-mode sheets.
   - If a composite sheet exceeds WebP's maximum canvas dimension (`16383px` in either width or height), the generator automatically splits the sheet into multi-part WebP files (`sheet_part1.webp`, `sheet_part2.webp`, etc.) rather than crashing or falling back to bloated PNG files.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 1)

This branch shares its domain with **`origin/arena/019fbf78-celup-lab`**.

| Evaluation Criterion | `origin/arena/019fbfb9-celup-lab` | `origin/arena/019fbf78-celup-lab` | Comparison Verdict |
| :--- | :--- | :--- | :--- |
| **Algorithmic Math (`method 3`)** | **Placeholder / Delegation**: Calls `autodeblur_pass(..., 3)` but does not implement S-curve crease-free mapping or 2x2 bilinear projection inside `autodeblur_pass`. | **Fully implemented math**: Explicit 2x2 bilinear projection, crease-free S-curve mapping, and contrast range gating. | **`019fbf78` wins** on algorithm completeness. |
| **Code Cleanliness & History** | **Surgical 2-commit history** directly on top of `master`; zero merge clutter or unintended side effects. | Contains merge commits and experimental `v4.9.9 WIP` code from `019fba18`. | **`019fbfb9` wins** on git hygiene and isolation. |
| **Sheet Generator Utilities** | Adds **auto-splitting for oversized WebP sheets** (`> 16383px`), solving a real libwebp limitation. | Prunes modes to best-scored methods but lacks automatic dimension splitting. | **`019fbfb9` wins** on Python generator robustness. |

### Quantitative & Qualitative Assessment
- **Why `019fbfb9`'s Python script improvement matters**: When stacking 30+ upscaling modes across large source images (like `miya_normal.webp`), total canvas height easily exceeds `16383px`. Without `019fbfb9`'s auto-split handling, `WebPEncodeLosslessRGBA` fails with an invalid-dimension error.
- **Why `019fbfb9`'s C implementation is incomplete**: Because `019fbfb9` only added the CLI parser and `analytical_deblur_pass()` wrapper without adding `if (method == 3)` math inside `autodeblur_pass()`, running `-D analytical` on `019fbfb9` produces output identical to standard autodeblur.

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

### Verdict: **CHERRY-PICK RECOMMENDED (Python Sheet Auto-Split & CLI Boilerplate)**
- **Justification**:
  1. Cherry-pick commit **`1293c23`** (`make_lab_comparison_sheets.py` auto-splitting for large lossless WebP sheets) into `master` immediately. This is a pure utility upgrade with zero downsides.
  2. For the `-D analytical` C code, adopt `019fbfb9`'s clean CLI parser and documentation string from commit **`ed6ae66`**, but pair it with the actual mathematical engine (`2x2 bilinear projection` and `crease-free S-curve mapping`) from **`019fbf78`**.
