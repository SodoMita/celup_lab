# Branch Review: `origin/arena/019fba1b-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fba1b-celup-lab`
- **Task Group**: Group 4 — Signed Distance Field (SDF) & Vector Upscaling (`--mode msdf`, `--mode dsdf`, C1 consensus)
- **Base / Merge-Base**: Root branch history starting from commit `10338e8` (v3) through `84ddd1a` (v4.9.2), with 10 unique commits advancing to v4.9.4.
- **Total Commits Ahead of v4.9.2**: 10 unique commits (`d3bf043` -> `1067d91`).
- **File Diff Summary**: Extensive enhancements to SDF and vector rendering in `celup_lab.c`, plus Python benchmark harnesses and 32/36-mode comparison sheets in lossless WebP.
- **Primary Domain**: Signed Distance Field (SDF) edge reconstruction, adding multi-channel (`msdf`) and directional (`dsdf`) native C modes, C1-continuous 3x3 consensus DSDF, and memory-safety bugfixes.

---

## 2. Key Technical Contributions & Architectural Changes

### Critical Memory Safety Bugfix (`commit 31f87f7`)
1. **ARM64 / Android Heap Out-of-Bounds Read Fix in `suppress_speckle_pm`**:
   - In `suppress_speckle_pm()` (speckle suppression after hourglass removal), the domino pass checks a 10-pixel ring around pixel pairs `(x, y)` and `(x+vert, y+1-vert)`.
   - The original loop bounds `for (int x = 1; x + 1 < dw; x++)` allowed `x` to reach `dw - 2`. When `vert == 1` (horizontal pair occupying `x` and `x+1`), checking the outer frame of the 4x3 box accessed `x + 2 = dw`, causing a heap out-of-bounds read segfault on ARM64 and Android builds.
   - Fixed the loop bounds to:
     ```c
     for (int y = 1; y + 2 - vert < dh; y++)
       for (int x = 1; x + 1 + vert < dw; x++)
     ```
   - Eliminates memory corruption and segfaults across all architectures.

### Native C SDF & Vector Upscalers (`msdf`, `dsdf`, C1 consensus)
2. **Native C Implementations of `--mode msdf` and `--mode dsdf` (`commits e7f487f` & `1067d91`)**:
   - **`msdf` (Multi-channel SDF)**: Reconstructs crisp vector-like corners by fitting multi-channel distance fields across edge junctions.
   - **`dsdf` (Directional SDF)**: Implements C1-continuous 3x3 consensus directional SDF across arbitrary upscales, ensuring smooth curve continuity without polygonal segmentation.
   - Fixed SDF line-angle invariance, junction/entrance-corner splatting, and the 10-pixel border fade bug (`374e9f4`).

3. **32/36-Mode Comparison Sheet Generator & Benchmarks**:
   - Expanded comparison sheet generators to render up to 36 modes across staircase, smiley, cat, and pikachu fixtures in lossless WebP.
   - Added `py:msdf` and `py:dsdf` benchmark harnesses (`4cd5ec2`).

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 4)

This branch is the premier SDF/vector upscaling branch, compared against **`origin/arena/019fbcda-celup-lab`** (which also touched SDF corner rounding and halos).

| Evaluation Criterion | `origin/arena/019fba1b` (`msdf` / `dsdf` vector) | `origin/arena/019fbcda` (`hybrid` mode) | Comparison Verdict |
| :--- | :--- | :--- | :--- |
| **Memory Safety & Robustness** | **Essential Bugfix**: Fixes heap out-of-bounds read segfault in `suppress_speckle_pm`. | Uses baseline `suppress_speckle_pm` loop bounds (vulnerable to out-of-bounds read). | **`019fba1b` is mandatory for codebase stability**. |
| **SDF Feature Completeness** | **Native C `msdf` & `dsdf`**: Adds multi-channel and C1-continuous directional SDF modes with line-angle invariance. | Reverts autodeblur depth to prevent SDF halos, but does not implement `msdf` or `dsdf`. | **`019fba1b` wins decisively** on vector/SDF capabilities. |
| **Corner & Junction Splatting** | Resolves entrance-corner splatting and border fade (`commit 374e9f4`). | Reverts mass-conserving depth to prevent corner rounding. | Both branches prioritize crisp, artifact-free corners. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Contains a critical, non-negotiable heap memory-safety bugfix (`commit 31f87f7`).
  - Brings professional vector/SDF upscaling (`msdf`, `dsdf`) natively into `celup_lab.c`.
  - Excellent visual proof across 36-mode lossless WebP sheets.
- **Weaknesses**:
  - `msdf` and `dsdf` are computationally heavier than simple polynomial filters (`mitchell`, `lanczos3`), though they remain fast in native C.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **RECOMMENDED FOR MASTER (PUSH BUGFIX IMMEDIATELY; MERGE SDF MODES)**
- **Justification**:
  1. Commit **`31f87f7`** (ARM64 / Android out-of-bounds heap read segfault fix in `suppress_speckle_pm`) is an **immediate, unconditional MUST-PUSH to `master`**.
  2. The native C implementations of **`--mode msdf`** and **`--mode dsdf`** (`e7f487f`, `1067d91`), along with the SDF line-angle invariance and border fade fixes (`374e9f4`), should be merged into `master` to complete `celup_lab`'s vector-reconstruction suite.
