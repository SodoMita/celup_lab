# Branch Review: `origin/arena/019fbcda-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fbcda-celup-lab`
- **Task Group**: Group 2 (Autodeblur Evolution) & Group 3 (Advanced Upscaling Filters — Hybrid Mode)
- **Base / Merge-Base**: Root branch history starting from commit `10338e8` (v3) through `84ddd1a` (v4.9.2), with 3 unique commits (`f3cf540`, `07223a7`, `82b5ffc`).
- **Total Commits Ahead of v4.9.2**: 3 unique commits
- **File Diff Summary**: Modified core engine and test suite to add a hybrid upscaler class.
- **Primary Domain**: Designing a multi-class hybrid upscaler (`--mode hybrid`) integrating xBRZ/xBR for pixel art, smooth mode for gradients, and autodeblur for natural edges, alongside testing and subsequently reverting experimental autodeblur depth math.

---

## 2. Key Technical Contributions & Architectural Changes

### Multi-Class Hybrid Upscaler (`--mode hybrid`) & Architectural Reversion
1. **Hybrid Upscaler Integration (`commit f3cf540`)**:
   - Implements a multi-class upscaling mode that dynamically selects the rendering engine per pixel based on local neighborhood classification:
     - **Pixel-Art / Checkers**: Uses xBR/xBRZ rules to preserve sharp diagonal stepladders without inventing colors.
     - **Coherent Edges**: Applies autodeblur with fitted ERF profile sharpening.
     - **Smooth Gradients**: Routes to smooth bilinear/lowpass blending to prevent staircase quantization.
   - Adds corresponding test gates and validation scripts.

2. **Experimental Mass-Conserving Depth & Rigorous Reversion (`commits 07223a7` -> `82b5ffc`)**:
   - In commit `07223a7`, incorporated `v4.9.5` mass-conserving deblur depth into the hybrid upscaler to test if contrast could be boosted across multi-class edges.
   - In commit `82b5ffc`, explicitly **reverted** the unstable mass-conserving deblur depth after discovering it caused severe corner rounding and Signed Distance Field (SDF) halos.
   - Reinstated stable ERF profile fitting and local 3x3 envelope clamping as the permanent safety boundary.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Groups 2 & 3)

This branch sits at the intersection of autodeblur algorithm design (Group 2) and advanced upscaling modes (Group 3).

| Evaluation Criterion | `origin/arena/019fbcda` (`hybrid`) | `origin/arena/019fba18` (`v4.9.9` autodeblur) | `origin/arena/019fbf57` (`jinc2_bilateral`) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **Autodeblur Edge Math & Stability** | **Conservative / Reverted**: Tried `v4.9.5` mass-conserving depth, observed corner rounding/SDF halos, and reverted to stable ERF + 3x3 envelope clamp. | **Aggressive / Retained**: Kept mass-conserving depth through v4.9.9, accepting 2.2x – 7.5x worse ringing (HG) to boost BW ink. | **Orthogonal**: Uses standard autodeblur; focuses on Hyllian Jinc2-Bilateral xBR filter for pixel art. | **`019fbcda` demonstrates superior algorithmic discipline** by reverting unstable depth math. |
| **Pixel-Art / Staircase Handling** | Uses multi-class classification to route checker/edge cells to xBRZ/xBR. | Relies purely on autodeblur `-D` modes; can cause rounding on pixel-art corners. | Implements **`jinc2_bilateral`** with continuous stepladder knobs (`--j2b-wb`, `--j2b-str`). | **`019fbf57` wins on continuous pixel-art filtering**, while `019fbcda` provides good discrete classification. |
| **Complexity vs Maintenance** | Medium complexity; introduces classification routing logic across multiple upscaler backends. | High complexity; deep changes to core autodeblur math and carrier transport. | Focused complexity; self-contained `jinc2` filter and `jinc2_auto` tuner. | **`019fbf57` is more cohesive** as a standalone upscaling mode. |

### Quantitative & Qualitative Assessment
- **The Value of Commit `82b5ffc`**: `019fbcda`'s explicit reversion of mass-conserving depth provides independent empirical confirmation of `019fbef6`'s diagnosis. It proves that across different upscaling pipelines (SDF, hybrid, standard autodeblur), mass-conserving depth consistently degrades corners and introduces halos.

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Unifies multiple upscaling paradigms (xBRZ/xBR, smooth, autodeblur) into a single adaptive `--mode hybrid` command.
  - Exemplifies rigorous engineering quality control by testing and reverting unstable autodeblur depth math when halos appeared.
- **Weaknesses**:
  - Classification boundaries between xBR, smooth, and autodeblur can occasionally produce subtle transition discontinuities on mixed-media images compared to a unified continuous filter like `jinc2_bilateral`.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **PARTIAL / CHERRY-PICK RECOMMENDED (Cherry-Pick Hybrid Classifier & Reversion Notes)**
- **Justification**:
  1. Commit **`82b5ffc`** serves as a vital historical guardrail and confirms that `master`'s stable ERF profile fitting and 3x3 envelope clamp should remain the default autodeblur math.
  2. The multi-class **`--mode hybrid`** upscaler (`f3cf540`) is a strong feature addition for users handling mixed anime/pixel-art sources. It should be **cherry-picked** into `master` after verifying that its CLI flags do not conflict with new analytical or jinc2 modes.
