# Branch Review: `origin/arena/019fba14-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fba14-celup-lab`
- **Task Group**: Group 3 — Advanced Upscaling Filters (`linear` mode, v7.1/v7.2 classical upscaler improvements)
- **Base / Merge-Base**: Root branch history starting from commit `10338e8` (v3) through `84ddd1a` (v4.9.2), with 12 unique commits advancing to v7.2.
- **Total Commits Ahead of v4.9.2**: 12 unique commits (`1db106b` -> `8cc3726`).
- **File Diff Summary**: Added `--mode linear` to `celup_lab.c`, refined classical/pixel-art modes (`dehourglass`, `consistentcompress`, `smooth`, `scale2x`), and added lossless WebP comparison sheets.
- **Primary Domain**: Classical 1D interpolation variants (`--mode linear`), pixel-art rule refinements (`scale2x`), hourglass removal improvements, and WebP sheet generation.

---

## 2. Key Technical Contributions & Architectural Changes

### Linear 1D Mode (`--mode linear`) & v7.1 / v7.2 Mode Polish
1. **Requested `--mode linear` Variant (`commit b4acfa6` & `28c79ed`)**:
   - Implements a user-requested asymmetric upscaling mode:
     - **Horizontal axis**: 1D linear interpolation across neighbor columns.
     - **Vertical axis**: Nearest-neighbor replication (no vertical division by 2).
   - Produces visibly sharper, less blurry output than standard 2D bilinear interpolation on horizontal scanline art and retro sprites.

2. **v7.1 / v7.2 Mode Polish & Metric Improvements (`commits 7edd37a` -> `e1eecd2`)**:
   - **`dehourglass`**: Improved bilinear checker blending, reducing hourglass artifact energy (`HG` metric) from `0.00400` to a near-zero **`0.00005`**.
   - **`consistentcompress`**: Gated classmap 100% bilinear checker blending, reducing `HG` from `0.05300` to **`0.00005`**.
   - **`smooth`**: Upgraded 4x4..8x8 bilinear blending, reducing `HG` from `0.14000` to **`0.00005`** while eliminating staircase steps.
   - **`scale2x`**: Fixed diagonal rule corner cases (`B != H`) and introduced a loose `same_colour` tolerance (`8e-3`) for more visible diagonal rules without breaking pixel-art fidelity.

3. **Lossless WebP Comparison Sheets (`commit 8cc3726`)**:
   - Updated comparison sheet scripts to include `--mode linear` and generate annotated lossless WebP sheets for `miya face`, `berry`, and `smiley`.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 3)

This branch focuses on classical/retro upscalers, compared against **`origin/arena/019fbf57-celup-lab`** (advanced Jinc2/xBR windowed filters) and **`origin/arena/019fba12-celup-lab`** (cleanup).

| Evaluation Criterion | `origin/arena/019fba14` (`linear` & v7.2) | `origin/arena/019fbf57` (`jinc2_bilateral`) | Comparison Verdict |
| :--- | :--- | :--- | :--- |
| **Retro / Pixel-Art Rules (`scale2x` vs `jinc2`)** | Upgraded **`scale2x`** with loose `8e-3` color tolerance; excellent for hard 2x integer scaling. | Implements **`jinc2_bilateral`** and **`jinc2_auto`**; provides continuous stepladder control across arbitrary scales. | **`019fbf57` wins on arbitrary scaling**, while `019fba14` is excellent for strict integer scale2x. |
| **New Upscaling Modes** | Adds **`--mode linear`** (1D horizontal linear, nearest vertical). | Adds **`--mode jinc2_bilateral`** and **`--mode jinc2_auto`**. | **Orthogonal features**: both modes serve distinct user workflows and do not conflict. |
| **Hourglass Suppression in Legacy Modes** | Achieves **`HG = 0.00005`** across `dehourglass`, `consistentcompress`, and `smooth`. | Does not alter legacy mode blending rules. | **`019fba14` wins on legacy mode polish**. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Direct response to user requests for `--mode linear` (no vertical 2x blur).
  - Dramatic quantitative improvements in hourglass suppression across legacy modes (`dehourglass`, `consistentcompress`, `smooth`).
  - Zero regression risk to autodeblur or SDF pipelines.
- **Weaknesses**:
  - `--mode linear` is inherently asymmetric; on images with strong vertical diagonal contours, nearest-neighbor vertical replication can look blocky compared to 2D filters.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **RECOMMENDED FOR MASTER (CHERRY-PICK / MERGE COMPATIBLE FEATURES)**
- **Justification**:
  1. The new **`--mode linear`** mode and the v7.1/v7.2 legacy mode improvements (`dehourglass`, `consistentcompress`, `smooth`, `scale2x`) from commits **`b4acfa6`**, **`7edd37a`**, and **`51e853a`** provide exceptional value with zero architectural conflicts.
  2. These classical upscaling refinements complement `019fbf57`'s Jinc2 filter perfectly.
  3. **Action**: Cherry-pick or merge `019fba14-celup-lab`'s mode additions into `master`.
