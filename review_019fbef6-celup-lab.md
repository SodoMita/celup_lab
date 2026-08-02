# Branch Review: `origin/arena/019fbef6-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fbef6-celup-lab`
- **Task Group**: Group 2 — Autodeblur Core Evolution & Diagnostics (`v4.9.3` – `v4.9.9`, `-D` autodeblur)
- **Base / Merge-Base**: Direct descendant of `origin/master` (`f6466c9b5d5e49e6ea6dd70ba8b674f4bb102094`)
- **Total Commits Ahead of Master**: 10 unique commits
- **File Diff Summary**: 14 files changed, 1109 insertions(+), 3 deletions(-)
  - Adds comprehensive documentation (`POLAR_ARTIFACT_DIAGNOSIS.md`, `comparison_vs_019fba18.md`, `AUTODEBLUR_AGENT_WORK.md`, `texgain_results.md`).
  - Adds diagnostic scripts and regression suites (`make_polar_sheet.py`, `make_vs_18_sheets.py`, `tests/autodeblur_regression.py`).
  - Minor, surgical additions to `celup_lab.c` (+119/-3 lines).
- **Primary Domain**: Diagnosing the "polar artifact" (circle cardinal/diagonal edge pinching), proving the mathematical trade-offs of `-r` vs `-g`, introducing non-invasive tuning knobs (`-T` texture-gain, `CELUP_CAPFLOOR`), and conducting a rigorous head-to-head evaluation against `019fba18`.

---

## 2. Key Technical Contributions & Architectural Changes

### Diagnostic Breakthroughs & Non-Invasive Tuning Knobs
1. **Polar Artifact Diagnosis (`POLAR_ARTIFACT_DIAGNOSIS.md`)**:
   - Investigated why circle cardinal points deform differently than diagonal 45-degree points when autodeblur is applied.
   - Proved mathematically and empirically that circle edge pinching scales with assumed blur radius (`-r`), **not** with steepness (`-g`).
   - Demonstrated that default `-r 1.5` resolves the polar artifact to sub-pixel levels without altering the core ERF model.

2. **Opt-in Texture Gain (`-T`, `--texgain`) & `CELUP_CAPFLOOR` Knob**:
   - In commit `9b849e3`, added an opt-in texture-gain parameter (`-T`) for lattice scenes:
     - On `checker2` torture tests, `-T` improves MAE by **-0.0042** and reduces HG ringing by **-0.0004**.
   - Added environment variable `CELUP_CAPFLOOR` (`eaedb4d`) to allow tuning the anti-realias cap floor (defaulting to `0.6`, identical to `v4.9.2`).

3. **Head-to-Head Evaluation Suite vs `019fba18`**:
   - Created `comparison_vs_019fba18.md`, `make_vs_18_sheets.py`, and `tests/autodeblur_regression.py`.
   - Measured and documented the exact trade-off between `master` (`v4.9.2` conservative model) and `019fba18` (`v4.9.8` aggressive model):
     - Showed that while `019fba18` recovers 95.7% black ink on `-r 6` recipes, it pays a catastrophic **2.2x to 7.5x penalty in ringing/hourglass artifacts (`HG`)**.
     - Documented why `celup_lab`'s prime directive favors clean, artifact-free contours over aggressive color-mass invention.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 2)

This branch is the diagnostic anchor of Task Group 2, evaluated against **`origin/arena/019fba18-celup-lab`** and **`origin/arena/019fbcda-celup-lab`**.

| Evaluation Criterion | `origin/arena/019fbef6` (`master` + knobs) | `origin/arena/019fba18` (`v4.9.9`) | `origin/arena/019fbcda` (`hybrid`) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **Artifact Suppression (`HG` Metric)** | **Best-in-class**: <br>• Diagonal HG: `0.00166`<br>• Rings HG: `0.00374`<br>• Corner HG: `0.00203` | **Severe Ringing**: <br>• Diagonal HG: `0.01238`<br>• Rings HG: `0.01549`<br>• Corner HG: `0.00559` | **Clean**: <br>Reverted unstable depth to match master's envelope clamp. | **`019fbef6` wins decisively** on artifact avoidance and visual purity. |
| **Lattice & Texture Fidelity** | Adds opt-in `-T texgain` knob, measurably improving `checker2` MAE (`-0.0042`). | Degrades lattice textures with crosshatch ringing (`HG 0.01256`). | Integrates xBRZ/xBR for pixel art, but autodeblur math matches master. | **`019fbef6` wins on natural texture control**. |
| **Diagnostic & Documentation Value** | **Exceptional**: Adds 4 markdown reports, 3 custom Python generators, and a full regression harness. | Good test script (`metrics.py`), but commit messages obscure trade-off severities. | Good integration work, but light on analytical documentation. | **`019fbef6` is the gold standard for repository documentation**. |
| **Code Footprint & Risk** | Zero regression risk (+119/-3 lines in C; defaults are bit-identical to `v4.9.2`). | High risk (+thousands of lines modified over 9 iterations; complex state). | Medium risk (adds whole new classifier/upscaler modes). | **`019fbef6` is the safest, most maintainable branch**. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Provides conclusive mathematical root-cause analysis for user artifact complaints (polar circle distortion & grey wash).
  - Keeps default behavior 100% bit-exact to baseline `v4.9.2` while adding useful tuning knobs (`-T` texgain, `CELUP_CAPFLOOR`).
  - Outstanding test harnesses (`autodeblur_regression.py`, `make_vs_18_sheets.py`, `make_polar_sheet.py`).
- **Weaknesses**:
  - On `--mode autodeblur` with heavy `-r 6` user recipes, it does not invent pure-black ink (`0.0%` pure-black recovery on `-r 6` BW art), correctly requiring the user to specify a realistic blur radius (`-r 1.5`) instead.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **RECOMMENDED FOR MASTER (PUSH FULL BRANCH / ALL COMMIT ASSETS)**
- **Justification**:
  1. `origin/arena/019fbef6-celup-lab` is a model branch. It introduces zero regressions, keeps all defaults bit-identical to `v4.9.2`, and adds highly valuable tuning capabilities (`-T` texture gain and `CELUP_CAPFLOOR`).
  2. Its documentation (`POLAR_ARTIFACT_DIAGNOSIS.md`, `comparison_vs_019fba18.md`) and regression test suites (`tests/autodeblur_regression.py`, `make_polar_sheet.py`) are essential for future maintainers to prevent re-litigating the contrast-vs-ringing trade-off.
  3. **Action**: Merge/push `019fbef6-celup-lab` directly into `master`.
