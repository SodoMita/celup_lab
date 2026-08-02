# Executive Synthesis & Master Push Recommendations: `celup_lab` Branch Reviews

This document synthesizes the individual technical reviews of all **10 branches** in `origin/arena/*` for `SodoMita/celup_lab`. By grouping branches that tackled similar tasks, comparing their mathematical models, code cleanliness, test metrics, and artifact risks, this report provides a decisive action plan for **which changes should be pushed to `master`**.

---

## 1. Executive Decision Matrix

| Task Group | Branch Name | Primary Contribution / Domain | Master Push Verdict | Action & Justification | Individual Review File |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Group 1**<br>Analytical Deblur (`-D analytical`) | **`origin/arena/019fbf78`** | True 2x2 bilinear projection (`method == 3`), S-curve mapping, `--no-safety-gates`. | **CHERRY-PICK RECOMMENDED** | Cherry-pick core math commits **`875aa4b`** & **`3fa9b93`**. Do not merge branch tip directly to avoid experimental v4.9.9 WIP commits. | [`review_019fbf78-celup-lab.md`](./review_019fbf78-celup-lab.md) |
| **Group 1**<br>Analytical Deblur (`-D analytical`) | **`origin/arena/019fbfb9`** | `-D analytical` CLI parser & WebP sheet auto-splitting (`>16383px`). | **CHERRY-PICK RECOMMENDED** | Cherry-pick sheet auto-split commit **`1293c23`** immediately. Use its CLI parser (`ed6ae66`) with `019fbf78`'s math. | [`review_019fbfb9-celup-lab.md`](./review_019fbfb9-celup-lab.md) |
| **Group 2**<br>Autodeblur Evolution & Diagnostics | **`origin/arena/019fbef6`** | Polar artifact diagnosis, proof that `-r 6` causes grey wash, `-T` texgain knob, `CELUP_CAPFLOOR`. | **RECOMMENDED FOR MASTER (PUSH FULL BRANCH)** | **Push to `master`**. Zero regressions (+119/-3 lines, defaults bit-identical to v4.9.2); adds essential docs & tuning knobs. | [`review_019fbef6-celup-lab.md`](./review_019fbef6-celup-lab.md) |
| **Group 2**<br>Autodeblur Evolution & Diagnostics | **`origin/arena/019fba18`** | Autodeblur v4.9.3 -> v4.9.9 (mass-conserving depth, skirt transport, grey test). | **DO NOT PUSH (REJECT CORE ARCHITECTURE)** | Do **not** push core autodeblur math (causes 2.2x–7.5x worse HG ringing). Cherry-pick `tests/metrics.py` only. | [`review_019fba18-celup-lab.md`](./review_019fba18-celup-lab.md) |
| **Group 2 & 3**<br>Hybrid Mode & Reversion | **`origin/arena/019fbcda`** | Multi-class `--mode hybrid` upscaler; tested & reverted unstable depth math (`82b5ffc`). | **PARTIAL / CHERRY-PICK** | Cherry-pick hybrid mode (`f3cf540`) after conflict check; keep its reversion of mass-conserving depth (`82b5ffc`) as policy. | [`review_019fbcda-celup-lab.md`](./review_019fbcda-celup-lab.md) |
| **Group 3**<br>Advanced Upscaling Filters | **`origin/arena/019fbf57`** | Working `jinc2_bilateral` Hyllian Jinc2-xBR filter, stepladder knobs (`WA/WB/STR/AR`), `jinc2_auto`. | **RECOMMENDED FOR MASTER (PUSH FULL FEATURE SET)** | **Push to `master`**. Highly effective filter (5x jump95 staircase reduction on diaglines; beats `lanczos3` on MAE). | [`review_019fbf57-celup-lab.md`](./review_019fbf57-celup-lab.md) |
| **Group 3**<br>Advanced Upscaling Filters | **`origin/arena/019fba14`** | `--mode linear` (1D horiz linear, nearest vert) + v7.1/v7.2 legacy mode improvements (`HG = 0.00005`). | **CHERRY-PICK RECOMMENDED** | Cherry-pick `--mode linear` (`b4acfa6`) and v7.1/v7.2 legacy improvements (`7edd37a`, `51e853a`). | [`review_019fba14-celup-lab.md`](./review_019fba14-celup-lab.md) |
| **Group 3**<br>Advanced Upscaling Filters | **`origin/arena/019fba12`** | Removed broken/incomplete references to `superxbr` and `jinc2_bilateral`. | **DO NOT PUSH (SUPERSEDED)** | Reject/archive. Its cleanup was only needed when `jinc2` was broken; `019fbf57` provides a fully working implementation. | [`review_019fba12-celup-lab.md`](./review_019fba12-celup-lab.md) |
| **Group 4**<br>SDF & Vector Upscaling | **`origin/arena/019fba1b`** | **CRITICAL BUGFIX**: ARM64 heap out-of-bounds read in `suppress_speckle_pm`; native C `msdf` & `dsdf`. | **RECOMMENDED FOR MASTER (MUST-PUSH BUGFIX)** | **Push bugfix `31f87f7` immediately**. Merge native C `msdf`/`dsdf` modes (`e7f487f`, `1067d91`) and SDF border fade fix (`374e9f4`). | [`review_019fba1b-celup-lab.md`](./review_019fba1b-celup-lab.md) |
| **Group 5**<br>Housekeeping & Test Guards | **`origin/arena/019fba0d`** | Stronger auto-stair guard values in `check_stairs.py`, WebP asset migration. | **CHERRY-PICK RECOMMENDED** | Cherry-pick `tests/check_stairs.py` threshold tuning commit **`3ae9226`**. | [`review_019fba0d-celup-lab.md`](./review_019fba0d-celup-lab.md) |

---

## 2. Head-to-Head Group Synthesis & Comparison Analysis

### Group 1: Analytical Deblur Mode (`-D analytical` / `method == 3`)
- **Branches**: `019fbf78-celup-lab` vs `019fbfb9-celup-lab`
- **Why `019fbf78` wins on mathematics**: `019fbfb9` added `-D analytical` to CLI parsing and created a wrapper function (`analytical_deblur_pass`), but inside `autodeblur_pass()`, `method == 3` has no dedicated math in `019fbfb9`—falling through to standard autodeblur. By contrast, `019fbf78` implements true 2x2 bilinear-projected linear deblur, crease-free S-curve mapping, contrast range gating, and the `--no-safety-gates` override flag.
- **Why `019fbfb9` wins on sheet generation**: `019fbfb9` solved a real libwebp limitation by adding automatic sheet-splitting to `make_lab_comparison_sheets.py` whenever a combined canvas exceeds `16383px`.
- **Master Strategy**: Combine `019fbfb9`'s Python sheet auto-splitting (`1293c23`) and CLI boilerplate (`ed6ae66`) with `019fbf78`'s core mathematical engine (`875aa4b`, `3fa9b93`).

### Group 2: Autodeblur Core Evolution & Diagnostics (`v4.9.3` – `v4.9.9`, `-D` autodeblur)
- **Branches**: `019fbef6-celup-lab` vs `019fba18-celup-lab` vs `019fbcda-celup-lab`
- **Why `019fbef6` wins on architectural vision**: `019fba18` pushed autodeblur aggressively to recover 95.7% black ink on `-r 6 -g 64` line art (vs 86.1% in master). However, `019fbef6`'s head-to-head analysis proved that `019fba18` pays a **2.2x to 7.5x penalty in ringing/hourglass artifacts (`HG`)** (`0.01238` vs `0.00166` on diagonal lines).
- **The Empirical Proof of Reversion**: `019fbcda` independently tested `v4.9.5` mass-conserving depth in a hybrid upscaler, observed corner rounding and SDF halos, and explicitly **reverted** it in commit `82b5ffc`.
- **Master Strategy**: Push `019fbef6` directly to `master` (it keeps defaults bit-exact to `v4.9.2` while adding `-T` texgain, `CELUP_CAPFLOOR`, and invaluable diagnostic proof). Reject `019fba18`'s core autodeblur math, but cherry-pick its `tests/metrics.py` grey-test script.

### Group 3: Advanced Upscaling Filters (`jinc2_bilateral` / `linear` / `hybrid` / `superxbr`)
- **Branches**: `019fbf57-celup-lab` vs `019fba14-celup-lab` vs `019fba12-celup-lab` (with `019fbcda-celup-lab`)
- **Why `019fbf57` supersedes earlier attempts**: `019fba12` was a cleanup branch that deleted broken/incomplete `superxbr` and `jinc2_bilateral` references. `019fbf57` succeeded where earlier attempts failed—delivering a fully working Hyllian Jinc2-Bilateral xBR filter in native C with stepladder knobs (`--j2b-wa/wb/str/ar`) and self-supervised auto-tuning (`--mode jinc2_auto`), cutting staircase `jump95` error up to 5x.
- **Why `019fba14` is complementary**: `019fba14` adds `--mode linear` (1D horizontal linear, nearest vertical) and polishes classical modes (`dehourglass`, `consistentcompress`, `smooth`), reducing legacy mode hourglass energy to `HG = 0.00005`.
- **Master Strategy**: Push `019fbf57` (`jinc2_bilateral` + `jinc2_auto`) to `master`. Cherry-pick `019fba14`'s `--mode linear` and legacy mode polish. Reject/archive `019fba12`.

### Group 4: Signed Distance Field (SDF) & Vector Upscaling (`--mode msdf`, `--mode dsdf`)
- **Branch**: `019fba1b-celup-lab`
- **Why it is mandatory**: Commit `31f87f7` fixes an ARM64/Android out-of-bounds heap read segfault in `suppress_speckle_pm()`. In addition, it brings native C implementations of `--mode msdf` (multi-channel SDF) and `--mode dsdf` (C1-continuous 3x3 consensus directional SDF), plus fixes for SDF line-angle invariance and border fade (`374e9f4`).
- **Master Strategy**: Push commit `31f87f7` immediately as a hotfix/security fix. Merge the native C SDF/vector modes (`e7f487f`, `1067d91`, `374e9f4`).

### Group 5: Housekeeping, WebP Assets, and Test Guards
- **Branch**: `019fba0d-celup-lab`
- **Master Strategy**: Cherry-pick commit `3ae9226` (`tests/check_stairs.py` auto-stair guard calibration) to strengthen automated staircase testing.

---

## 3. Master Push & Cherry-Pick Action Plan (Step-by-Step)

For the maintainer consolidating changes onto `master`, execute the following sequence:

```sh
# Step 1: Push Critical Memory-Safety Bugfix (Group 4)
git cherry-pick 31f87f701e2c654a11026e0aedbed230f36f1cc3  # fix ARM64 / Android segfault in suppress_speckle_pm

# Step 2: Merge Clean Diagnostic & Knob Anchor Branch (Group 2)
git merge origin/arena/019fbef6-celup-lab  # adds -T texgain, CELUP_CAPFLOOR, polar diagnosis, and regression suite

# Step 3: Merge Working Advanced Upscaling Filters (Group 3)
git merge origin/arena/019fbf57-celup-lab  # adds working jinc2_bilateral, jinc2_auto, and stepladder knobs

# Step 4: Cherry-Pick Classical Mode Refinements & --mode linear (Group 3)
git cherry-pick b4acfa6  # add --mode linear (1D horizontal linear only, nearest vertical)
git cherry-pick 7edd37a  # improve legacy dehourglass & consistentcompress (HG -> 0.00005)
git cherry-pick 51e853a  # scale2x loose same_colour tolerance 8e-3

# Step 5: Cherry-Pick Native C Vector/SDF Modes (Group 4)
git cherry-pick 374e9f4  # fix SDF line-angle invariance and 10-pixel border fade
git cherry-pick e7f487f  # native C implementations of --mode msdf and --mode dsdf
git cherry-pick 1067d91  # C1-continuous 3x3 consensus DSDF

# Step 6: Cherry-Pick Analytical Deblur Method 3 & Sheet Auto-Split (Group 1)
git cherry-pick 1293c23  # make_lab_comparison_sheets.py auto-split for >16383px WebP sheets
git cherry-pick 875aa4b  # true 2x2 bilinear-projected linear deblur (method == 3)
git cherry-pick 3fa9b93  # crease-free S-curve mapping and contrast range gating

# Step 7: Cherry-Pick Test Suite Tools (Groups 2 & 5)
git cherry-pick 3ae9226  # slightly stronger auto-stair guard values in check_stairs.py
# (Extract tests/metrics.py from 019fba18 without its core autodeblur regressions)
```

---

## 4. Index of Individual Review Documents
For comprehensive line-by-line diff analysis, metric tables, and commit hashes for each branch, consult:
1. [`review_019fbf78-celup-lab.md`](./review_019fbf78-celup-lab.md) — Analytical Deblur Mode (`method == 3`, S-curve mapping)
2. [`review_019fbfb9-celup-lab.md`](./review_019fbfb9-celup-lab.md) — `-D analytical` CLI & WebP Sheet Auto-Splitting
3. [`review_019fbef6-celup-lab.md`](./review_019fbef6-celup-lab.md) — Polar Artifact Diagnosis & `-T` Texture Gain
4. [`review_019fba18-celup-lab.md`](./review_019fba18-celup-lab.md) — Autodeblur v4.9.4–v4.9.9 & The Grey Test
5. [`review_019fbcda-celup-lab.md`](./review_019fbcda-celup-lab.md) — `--mode hybrid` & Depth Reversion Policy
6. [`review_019fbf57-celup-lab.md`](./review_019fbf57-celup-lab.md) — `jinc2_bilateral` Hyllian xBR Filter & `jinc2_auto`
7. [`review_019fba14-celup-lab.md`](./review_019fba14-celup-lab.md) — `--mode linear` & Legacy Mode Polish (HG `0.00005`)
8. [`review_019fba12-celup-lab.md`](./review_019fba12-celup-lab.md) — Broken SuperxBR / Jinc2 Cleanup
9. [`review_019fba1b-celup-lab.md`](./review_019fba1b-celup-lab.md) — Native C `msdf`/`dsdf` & ARM64 Heap Segfault Fix
10. [`review_019fba0d-celup-lab.md`](./review_019fba0d-celup-lab.md) — Repository Hygiene & Staircase Guard Tuning
