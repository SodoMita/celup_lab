# Branch Review: `origin/arena/019fbf57-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fbf57-celup-lab`
- **Task Group**: Group 3 — Advanced Upscaling Filters (`jinc2_bilateral`, `jinc2_auto`, xBR/xBRZ variants)
- **Base / Merge-Base**: Root branch history starting from commit `10338e8` (v3) through `84ddd1a` (v4.9.2), with 6 unique commits (`3d901a9`, `13e3dae`, `dcc0c42`, `0574d03`, `bb3e813`, `1618485`).
- **Total Commits Ahead of v4.9.2**: 6 unique commits
- **File Diff Summary**: Added full native C implementations of `jinc2_bilateral` and `jinc2_auto` to `celup_lab.c` (+227/-12 lines across `bb3e813` and `1618485`), added `make_user_comparison_sheets.py`, and updated `IMPROVEMENTS.md` and `tests/test_scales.py`.
- **Primary Domain**: Implementing Hyllian's Jinc2-Bilateral xBR upscaling filter (`--mode jinc2_bilateral`) with fine-grained stepladder tuning knobs (`WA/WB/STR/AR`), adding a self-supervised auto-tuner (`--mode jinc2_auto`), and generating user-focused comparison sheets.

---

## 2. Key Technical Contributions & Architectural Changes

### Hyllian Jinc2-Bilateral xBR Filter & Self-Supervised Auto-Tuner
1. **`jinc2_bilateral` Upscaling Engine (`commit 3d901a9` & `bb3e813`)**:
   - Fully implements the Hyllian Jinc2-Bilateral xBR windowed filter in native C.
   - Exposes four continuous shader tuning knobs via CLI flags and environment variables:
     - `--j2b-wa` (`CELUP_J2B_WA`): Window weight A (default `0.25`).
     - `--j2b-wb` (`CELUP_J2B_WB`): Window weight B / stepladder range term (default `0.125`, up to `0.80`).
     - `--j2b-str` (`CELUP_J2B_STR`): Stepladder strength / edge re-quantization intensity (default `0.8`).
     - `--j2b-ar` (`CELUP_J2B_AR`): Anti-ringing clamp threshold.
   - Proved on the 45-degree diagline staircase fixture (`jump95` metric, lower=better):
     - Baseline: `2x 0.034`, `4x 0.146`, `8x 0.219`.
     - `--j2b-wb 0.80`: `2x 0.021`, `4x 0.027`, `8x 0.042` (**up to 5x reduction in staircase error**).
     - `--j2b-str 0.2`: `4x 0.063`, achieving the **best overall MAE** across diag/curves/axis/shallow/corner/parallel test suites (beating `lanczos3`).

2. **`jinc2_auto` Self-Supervised Parameter Selector (`commit 1618485`)**:
   - Implements `--mode jinc2_auto`, which runs a self-supervised 2x-downscale proxy to pick the optimal `(WB, STR)` pair per image.
   - Formulates the optimization objective as:
     ```
     Objective = MSE * (1 + 2.5 * hardness * STR)
     ```
     where `hardness` is the duplicate-link fraction of the source image.
   - On smooth natural gradients, `hardness -> 0` and the tuner picks sharp MSE-optimal parameters; on hard pixel art, it penalizes high `STR` and picks low stepladder strength, avoiding lattice quantization artifacts.

3. **Comparison Sheets for Real Source Assets (`commit dcc0c42` & `0574d03`)**:
   - Added `make_user_comparison_sheets.py` to generate stacked comparisons across 10 modes (`nearest`, `bilinear`, `cubic`, `mitchell`, `lanczos3`, `jinc2_bilateral`, `superxbr`, `adaptive`, `autodeblur`, `sdf`) for `miya face`, `poor smiley`, `cat`, and `femlineart`.
   - Adopts `master`'s `.gitignore` policy so generated WebP sheets remain tracked and inspectable.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 3)

This branch is compared directly against **`origin/arena/019fba12-celup-lab`** (which deleted broken superxbr/jinc2 code) and **`origin/arena/019fba14-celup-lab`** (which focused on linear mode and v7.1/v7.2 pixel-art/checker improvements).

| Evaluation Criterion | `origin/arena/019fbf57` (`jinc2_bilateral` / `auto`) | `origin/arena/019fba12` (`clean-up` branch) | `origin/arena/019fba14` (`linear` & v7.2) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **`jinc2_bilateral` Implementation** | **100% Working, highly optimized**: Exposes WA/WB/STR/AR knobs + self-supervised auto-tuner. | **Removed**: Deleted broken/incomplete references to superxbr and jinc2_bilateral. | **Not present**: Focuses on classical 1D linear interpolation. | **`019fbf57` is the undisputed winner** for Jinc2/xBR filtering. |
| **Staircase & Diagonal Resolution (`jump95` / MAE)** | **Best overall MAE & jump95**: `--j2b-wb .80` cuts staircase jump95 from `.146` to `.027` at 4x; beats `lanczos3`. | N/A (uses baseline `v4.9.2` autodeblur). | Improves diagonal rules in `scale2x` (`loose 8e-3` same_color tolerance), but lacks continuous Jinc2 filtering. | **`019fbf57` wins on continuous upscaling quality**. |
| **User Parameter Adaptability** | **Self-supervised `--mode jinc2_auto`**: Automatically adapts stepladder strength based on image hardness. | N/A. | Manual mode selection (`--mode linear`, `--mode scale2x`). | **`019fbf57` wins on automation and user friendliness**. |
| **Asset & Sheet Generation** | Adds `make_user_comparison_sheets.py` covering 10 modes across real user images. | None. | Adds lossless WebP sheets for `linear` mode and v7.1/v7.2. | Both `019fbf57` and `019fba14` provide excellent sheet generators. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Turns a previously broken/experimental algorithm (`jinc2_bilateral`) into a best-in-class upscaler that outperforms `lanczos3` on diagonal MAE.
  - The `jinc2_auto` hardness metric elegantly bridges the gap between natural line art and hard pixel art without requiring user intervention.
  - Extremely well-tested and documented (`IMPROVEMENTS.md`).
- **Weaknesses**:
  - `jinc2_auto`'s self-supervised 2x downscale-then-upscale optimization pass adds compute time during initial initialization compared to single-pass filters.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **RECOMMENDED FOR MASTER (PUSH FULL FEATURE SET)**
- **Justification**:
  1. Commit series **`3d901a9` -> `bb3e813` -> `1618485`** is a major feature success. While earlier branch `019fba12` had to remove broken jinc2_bilateral references, `019fbf57` solved the math, implemented clean C code, and added self-supervised auto-tuning.
  2. Its diagnostic proof on the 45-degree staircase fixture (`jump95` reduced by 5x) confirms its superiority for high-scaling line art.
  3. **Action**: Merge/push `019fbf57-celup-lab`'s `jinc2_bilateral` and `jinc2_auto` additions into `master`.
