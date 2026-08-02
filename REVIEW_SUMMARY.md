# Executive Synthesis & Master Push Recommendations: `celup_lab` Branch Reviews

This document synthesizes the individual technical reviews of all **11 candidate branches** in `origin/arena/*` for `SodoMita/celup_lab`. Incorporating empirical evidence from **"the grey test" (`tests/metrics.py`)** and cross-branch replies (`REPLY_to_019fba18.md`, `REPLY_review_019fc1b2.md`), this report provides a decisive action plan for **which changes should be pushed to `master`**.

---

## 1. Executive Decision Matrix

| Task Group | Branch Name | Primary Contribution / Domain | Master Push Verdict | Action & Justification | Individual Review File |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Group 1**<br>Analytical Deblur (`-D analytical`) | **`origin/arena/019fc1ba`** | True full-image gradient narrowing (`method == 3`), normal tracing to plateau colors `P0, P1`, inverted `-g` semantics, 0 hull violations. | **CHERRY-PICK FOR `-D analytic` (OPT-IN MODE ONLY)** | Cherry-pick `-D analytic` (`20ad551`) as an opt-in mode. Produces more mid-gray transition artifacts than `019fba18` on BW art; pair with `019fbfb9` sheets. | [`review_019fc1ba-celup-lab.md`](./review_019fc1ba-celup-lab.md) |
| **Group 1**<br>Analytical Deblur (`-D analytical`) | **`origin/arena/019fbfb9`** | `-D analytical` CLI parser & WebP sheet auto-splitting (`>16383px`). | **CHERRY-PICK RECOMMENDED** | Cherry-pick sheet auto-split commit **`1293c23`** immediately. Resolves why `019fc1ba` was missed visually by enabling analytical sheets. | [`review_019fbfb9-celup-lab.md`](./review_019fbfb9-celup-lab.md) |
| **Group 1**<br>Analytical Deblur (`-D analytical`) | **`origin/arena/019fbf78`** | Local 2x2 bilinear projection (`method == 3`), S-curve mapping, `--no-safety-gates`. | **DO NOT PUSH (REJECT MATH IN FAVOR OF `019fc1ba`)** | Reject C implementation. Its local 2x2 bilinear projection degenerates into "another `autoblurcompress`". | [`review_019fbf78-celup-lab.md`](./review_019fbf78-celup-lab.md) |
| **Group 2**<br>Autodeblur Evolution & Diagnostics | **`origin/arena/019fba18`** | Autodeblur v4.9.4 -> v4.9.9 (erf-gain post-map, tangent evidence integration, `tests/metrics.py`). | **RECOMMENDED FOR MASTER (PUSH v4.9.9 CORE FOR BW / AUTODEBLUR)** | **Push `019fba18` core (`18df5b2`) & `metrics.py` to `master`**. Winner on user's BW acceptance criterion (`1.62% grey` on `-r6 -g64`, 0 BW hull violations). | [`review_019fba18-celup-lab.md`](./review_019fba18-celup-lab.md) |
| **Group 2**<br>Autodeblur Evolution & Diagnostics | **`origin/arena/019fbef6`** | Polar artifact diagnosis, `-T` texgain knob, `CELUP_CAPFLOOR`, cross-branch reply concessions. | **RECOMMENDED FOR MASTER (PUSH DIAGNOSTICS & KNOBS)** | **Push to `master`**. Conceded HG inversion and recommended `019fba18` for BW; contributes invaluable polar diagnosis, `-T` texgain, and regression suite. | [`review_019fbef6-celup-lab.md`](./review_019fbef6-celup-lab.md) |
| **Group 2 & 3**<br>Hybrid Mode & Reversion | **`origin/arena/019fbcda`** | Multi-class `--mode hybrid` upscaler; tested & reverted unstable depth math (`82b5ffc`). | **PARTIAL / CHERRY-PICK** | Cherry-pick hybrid mode (`f3cf540`) after conflict check; keep its reversion of mass-conserving depth (`82b5ffc`) as policy. | [`review_019fbcda-celup-lab.md`](./review_019fbcda-celup-lab.md) |
| **Group 3**<br>Advanced Upscaling Filters | **`origin/arena/019fbf57`** | Working `jinc2_bilateral` Hyllian Jinc2-xBR filter, stepladder knobs (`WA/WB/STR/AR`), `jinc2_auto`. | **RECOMMENDED FOR MASTER (PUSH FULL FEATURE SET)** | **Push to `master`**. Highly effective filter (5x jump95 staircase reduction on diaglines; beats `lanczos3` on MAE). | [`review_019fbf57-celup-lab.md`](./review_019fbf57-celup-lab.md) |
| **Group 3**<br>Advanced Upscaling Filters | **`origin/arena/019fba14`** | `--mode linear` (1D horiz linear, nearest vert) + v7.1/v7.2 legacy mode improvements (`HG = 0.00005`). | **CHERRY-PICK RECOMMENDED** | Cherry-pick `--mode linear` (`b4acfa6`) and v7.1/v7.2 legacy improvements (`7edd37a`, `51e853a`). | [`review_019fba14-celup-lab.md`](./review_019fba14-celup-lab.md) |
| **Group 3**<br>Advanced Upscaling Filters | **`origin/arena/019fba12`** | Removed broken/incomplete references to `superxbr` and `jinc2_bilateral`. | **DO NOT PUSH (SUPERSEDED)** | Reject/archive. Its cleanup was only needed when `jinc2` was broken; `019fbf57` provides a fully working implementation. | [`review_019fba12-celup-lab.md`](./review_019fba12-celup-lab.md) |
| **Group 4**<br>SDF & Vector Upscaling | **`origin/arena/019fba1b`** | **CRITICAL BUGFIX**: ARM64 heap out-of-bounds read in `suppress_speckle_pm`; native C `msdf` & `dsdf`. | **RECOMMENDED FOR MASTER (MUST-PUSH BUGFIX)** | **Push bugfix `31f87f7` immediately**. Merge native C `msdf`/`dsdf` modes (`e7f487f`, `1067d91`) and SDF border fade fix (`374e9f4`). | [`review_019fba1b-celup-lab.md`](./review_019fba1b-celup-lab.md) |
| **Group 5**<br>Housekeeping & Test Guards | **`origin/arena/019fba0d`** | Stronger auto-stair guard values in `check_stairs.py`, WebP asset migration. | **CHERRY-PICK RECOMMENDED** | Cherry-pick `tests/check_stairs.py` threshold tuning commit **`3ae9226`**. | [`review_019fba0d-celup-lab.md`](./review_019fba0d-celup-lab.md) |

---

## 2. Head-to-Head Group Synthesis & Comparison Analysis

### Group 1: Analytical Deblur Mode (`-D analytical` / `method == 3`)
- **Branches Evaluated**: `019fc1ba-celup-lab` vs `019fbf78-celup-lab` vs `019fbfb9-celup-lab`
- **Why `019fc1ba` is the True Analytical Engine**: `019fc1ba` (`20ad551`) implements genuine full-image gradient narrowing (`-D analytic`) by tracing transitions globally along their 4D structure-tensor normal (`maxR=16` walk) to plateau colors `P0` and `P1`, guaranteeing **0 hull violations**.
- **Why `019fbf78` Created "Another `autoblurcompress`"**: Because `019fbf78` applied a local 2x2 bilinear projection rather than tracing transitions globally to real plateau colors, its local unsharp projection on existing blurred ramps compressed gradients in a manner indistinguishable from `autoblurcompress`.
- **Why `019fc1ba` Produced More Mid-Gray Artifacts on BW Art**: As proven by **the grey test (`tests/metrics.py`)**, `019fc1ba`'s linear transition narrowing (`alpha = (K-1)/K`) leaves a linear slope between plateau colors `P0` and `P1`. On binary BW drawings (`poor smiley.webp`), any pixel along that slope is counted as mid-gray mush. In contrast, **`019fba18` (`v4.9.9`)** applies an erf-gain post-map on finished colors, actively driving transition mid-grays toward 0 (`1.62% grey` vs `019fc1ba`'s linear veil).
- **Master Strategy**: Cherry-pick **`019fc1ba`** (`20ad551`) as an opt-in mode (`-D analytic`), paired with **`019fbfb9`** (`1293c23`) for comparison sheet auto-splitting (`>16383px`). Keep `019fba18` (`v4.9.9`) as the primary autodeblur engine for BW content.

### Group 2: Autodeblur Core Evolution & Diagnostics (`v4.9.3` – `v4.9.9`, `-D` autodeblur)
- **Branches Evaluated**: `019fba18-celup-lab` vs `019fbef6-celup-lab` vs `019fbcda-celup-lab`
- **The Grey Test Proof & Cross-Branch Concessions**:
  - In `REPLY_to_019fba18.md`, **`019fbef6` conceded** that `019fba18`'s v4.9.9 core won decisively on the user's actual acceptance criterion (`grey -> 0` on BW content), achieving **1.62% grey at `-r 6 -g 64`** (and **1.13% at `-r 2.3`**), with **0 hull violations on BW content** and best MAE against ground truth on `rings`, `corner`, and `checker2`.
  - `019fbef6` also conceded that using the `HG` metric as headline evidence against `019fba18` was a structural metric error because step-class ground truth inherently scores high HG (`.0460` on rings).
  - In `REPLY_review_019fc1b2.md`, **`019fba18` accepted two critical guards**: **F1** (colour-content guard restricting endpoint extension to the 4D hull segment to prevent colorful plateau corner violations) and **F2** (sharp-source `qq` gate abstaining on existing 1px transitions like `diag`/`crosshatch`).
- **Master Strategy**: Push **`019fba18` (`v4.9.9`)** and `tests/metrics.py` to `master` as the primary autodeblur engine for BW content, combined with **`019fbef6`'s** `-T` texture gain knob, `CELUP_CAPFLOOR` environment variable, and polar-artifact diagnostic suite.

### Group 3: Advanced Upscaling Filters (`jinc2_bilateral` / `linear` / `hybrid` / `superxbr`)
- **Branches Evaluated**: `019fbf57-celup-lab` vs `019fba14-celup-lab` vs `019fba12-celup-lab` (with `019fbcda-celup-lab`)
- **Why `019fbf57` Supersedes Earlier Attempts**: `019fba12` deleted broken/incomplete `superxbr` and `jinc2_bilateral` references. `019fbf57` succeeded—delivering a fully working Hyllian Jinc2-Bilateral xBR filter in native C with stepladder knobs (`--j2b-wa/wb/str/ar`) and self-supervised auto-tuning (`--mode jinc2_auto`), cutting staircase `jump95` error up to 5x.
- **Why `019fba14` is Complementary**: `019fba14` adds `--mode linear` (1D horizontal linear, nearest vertical) and polishes classical modes (`dehourglass`, `consistentcompress`, `smooth`), reducing legacy mode hourglass energy to `HG = 0.00005`.
- **Master Strategy**: Push `019fbf57` (`jinc2_bilateral` + `jinc2_auto`) to `master`. Cherry-pick `019fba14`'s `--mode linear` and legacy mode polish. Reject/archive `019fba12`.

### Group 4: Signed Distance Field (SDF) & Vector Upscaling (`--mode msdf`, `--mode dsdf`)
- **Branch Evaluated**: `019fba1b-celup-lab`
- **Why It is Mandatory**: Commit `31f87f7` fixes an ARM64/Android out-of-bounds heap read segfault in `suppress_speckle_pm()`. In addition, it brings native C implementations of `--mode msdf` (multi-channel SDF) and `--mode dsdf` (C1-continuous 3x3 consensus directional SDF), plus fixes for SDF line-angle invariance and border fade (`374e9f4`).
- **Master Strategy**: Push commit `31f87f7` immediately as a hotfix/security fix. Merge the native C SDF/vector modes (`e7f487f`, `1067d91`, `374e9f4`).

### Group 5: Housekeeping, WebP Assets, and Test Guards
- **Branch Evaluated**: `019fba0d-celup-lab`
- **Master Strategy**: Cherry-pick commit `3ae9226` (`tests/check_stairs.py` auto-stair guard calibration) to strengthen automated staircase testing.

---

## 3. Master Push & Cherry-Pick Action Plan (Step-by-Step)

For the maintainer consolidating changes onto `master`, execute the following sequence:

```sh
# Step 1: Push Critical Memory-Safety Bugfix (Group 4)
git cherry-pick 31f87f701e2c654a11026e0aedbed230f36f1cc3  # fix ARM64 / Android segfault in suppress_speckle_pm

# Step 2: Push Autodeblur v4.9.9 Core & The Grey Test (Group 2)
git merge origin/arena/019fba18-celup-lab  # v4.9.9 completion engine (1.62% grey on -r6 -g64) + tests/metrics.py

# Step 3: Merge Clean Diagnostic & Knob Anchor Branch (Group 2)
git merge origin/arena/019fbef6-celup-lab  # adds -T texgain, CELUP_CAPFLOOR, polar diagnosis, and regression suite

# Step 4: Merge Working Advanced Upscaling Filters (Group 3)
git merge origin/arena/019fbf57-celup-lab  # adds working jinc2_bilateral, jinc2_auto, and stepladder knobs

# Step 5: Cherry-Pick Classical Mode Refinements & --mode linear (Group 3)
git cherry-pick b4acfa6  # add --mode linear (1D horizontal linear only, nearest vertical)
git cherry-pick 7edd37a  # improve legacy dehourglass & consistentcompress (HG -> 0.00005)
git cherry-pick 51e853a  # scale2x loose same_colour tolerance 8e-3

# Step 6: Cherry-Pick Native C Vector/SDF Modes (Group 4)
git cherry-pick 374e9f4  # fix SDF line-angle invariance and 10-pixel border fade
git cherry-pick e7f487f  # native C implementations of --mode msdf and --mode dsdf
git cherry-pick 1067d91  # C1-continuous 3x3 consensus DSDF

# Step 7: Cherry-Pick Opt-in Analytical Deblur Math & Sheet Auto-Split (Group 1)
git cherry-pick 1293c23  # make_lab_comparison_sheets.py auto-split for >16383px WebP sheets (from 019fbfb9)
git cherry-pick 20ad551  # opt-in full-image gradient narrowing (-D analytic), 0 hull violations & tests (from 019fc1ba)

# Step 8: Cherry-Pick Test Suite Tools (Group 5)
git cherry-pick 3ae9226  # slightly stronger auto-stair guard values in check_stairs.py (from 019fba0d)
```

---

## 4. Index of Individual Review Documents
For comprehensive line-by-line diff analysis, metric tables, and commit hashes for each branch, consult:
1. [`review_019fc1ba-celup-lab.md`](./review_019fc1ba-celup-lab.md) — True Full-Image Gradient Narrowing (`-D analytic`, 0 Hull Violations)
2. [`review_019fbf78-celup-lab.md`](./review_019fbf78-celup-lab.md) — Local 2x2 Bilinear Projection (`method == 3`) & Why It Created Another `autoblurcompress`
3. [`review_019fbfb9-celup-lab.md`](./review_019fbfb9-celup-lab.md) — `-D analytical` CLI & WebP Sheet Auto-Splitting (`>16383px`)
4. [`review_019fba18-celup-lab.md`](./review_019fba18-celup-lab.md) — Autodeblur v4.9.4–v4.9.9, Skirt Transport & BW Grey Test Analysis
5. [`review_019fbef6-celup-lab.md`](./review_019fbef6-celup-lab.md) — Polar Artifact Diagnosis, `-T` Texture Gain & Head-to-Head Comparison
6. [`review_019fbcda-celup-lab.md`](./review_019fbcda-celup-lab.md) — `--mode hybrid` Upscaler & Experimental Depth Reversion Analysis
7. [`review_019fbf57-celup-lab.md`](./review_019fbf57-celup-lab.md) — `jinc2_bilateral` Hyllian xBR Filter & `jinc2_auto`
8. [`review_019fba14-celup-lab.md`](./review_019fba14-celup-lab.md) — `--mode linear` (1D Horizontal Linear) & v7.1/v7.2 Mode Polish (`HG 0.00005`)
9. [`review_019fba12-celup-lab.md`](./review_019fba12-celup-lab.md) — Broken SuperXBR / Jinc2 Cleanup
10. [`review_019fba1b-celup-lab.md`](./review_019fba1b-celup-lab.md) — Native C `msdf`/`dsdf` & ARM64 Heap Segfault Fix
11. [`review_019fba0d-celup-lab.md`](./review_019fba0d-celup-lab.md) — Repository Hygiene & Staircase Guard Calibration
