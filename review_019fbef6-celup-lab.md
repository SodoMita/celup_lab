# Branch Review: `origin/arena/019fbef6-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fbef6-celup-lab`
- **Task Group**: Group 2 — Autodeblur Core Evolution & Diagnostics (`v4.9.3` – `v4.9.9`, `-D` autodeblur)
- **Base / Merge-Base**: Direct descendant of `origin/master` (`f6466c9b5d5e49e6ea6dd70ba8b674f4bb102094`)
- **Total Commits Ahead of Master**: 11 unique commits (including reply commit `eca222d`)
- **File Diff Summary**: 15 files changed, 1251 insertions(+), 3 deletions(-)
  - Adds comprehensive documentation (`POLAR_ARTIFACT_DIAGNOSIS.md`, `comparison_vs_019fba18.md`, `REPLY_to_019fba18.md`, `AUTODEBLUR_AGENT_WORK.md`, `texgain_results.md`).
  - Adds diagnostic scripts and regression suites (`make_polar_sheet.py`, `make_vs_18_sheets.py`, `tests/autodeblur_regression.py`).
  - Minor, surgical additions to `celup_lab.c` (+119/-3 lines).
- **Primary Domain**: Diagnosing the "polar artifact" (circle cardinal/diagonal edge pinching), introducing non-invasive tuning knobs (`-T` texture-gain, `CELUP_CAPFLOOR`), and providing objective cross-branch evaluation and reply documentation.

---

## 2. Key Technical Contributions & Architectural Changes

### Diagnostic Breakthroughs & Intellectual Honesty (`REPLY_to_019fba18.md`)
1. **Polar Artifact Diagnosis (`POLAR_ARTIFACT_DIAGNOSIS.md`)**:
   - Investigated why circle cardinal points deform differently than diagonal 45-degree points when autodeblur is applied.
   - Proved mathematically and empirically that circle edge pinching scales with assumed blur radius (`-r`), **not** with steepness (`-g`).

2. **Opt-in Texture Gain (`-T`, `--texgain`) & `CELUP_CAPFLOOR` Knob**:
   - Added an opt-in texture-gain parameter (`-T`) for lattice scenes (`checker2` MAE `-0.0042`).
   - Added environment variable `CELUP_CAPFLOOR` to allow tuning the anti-realias cap floor (defaulting to `0.6`).

3. **Cross-Branch Reply & Concessions (`commit eca222d` / `REPLY_to_019fba18.md`)**:
   - Demonstrated exemplary engineering integrity in replying to `019fba18`'s counter-analysis:
     - **Conceded HG Metric Inversion**: Acknowledged that HG measures saddle-basis energy against a bilinear reference (`HG=0`), meaning sharp step-class ground truth inherently scores high HG. Conceded that using HG as headline evidence against `019fba18` was a structural metric error.
     - **Conceded BW Task Superiority**: Acknowledged that under the user's explicit directive (`grey -> 0` on BW content at their recipe), `019fba18` (`v4.9.9`) won decisively (`1.62% grey`), and recommending `-r 1.5` was a parameter workaround rather than a solution to the task.
     - **Recommended Merging `019fba18`**: Explicitly recommended merging `019fba18`'s v4.9.9 core for BW content, combined with `019fbef6`'s diagnostic harnesses and polar-artifact measurement suite.

---

## 3. Head-to-Head Comparison with Similar Branches (Task Group 2)

| Evaluation Criterion | `origin/arena/019fbef6` (`master` + knobs) | `origin/arena/019fba18` (`v4.9.9` autodeblur) | Comparison Verdict |
| :--- | :--- | :--- | :--- |
| **BW Grey Test (`gray%` on `-r 6 -g 64`)** | `29.98% grey` (leaves grey wash intact). | **`1.62% grey`** (recovers pure black/white plateaus). | **`019fba18` wins on BW line art deblurring**, as conceded by `019fbef6`. |
| **Sharp-Source Abstention (`diag` / `crosshatch`)** | **Superior abstention**: Abstains cleanly on existing 1px transitions (`diag` MAE `.0126`). | Can false-fire on sharp area-downsampled sources (`diag` MAE `.0213`) until F2 guard is applied. | **`019fbef6` wins on sharp-source preservation**. |
| **Diagnostic & Regression Harnesses** | **Unrivaled**: Adds `POLAR_ARTIFACT_DIAGNOSIS.md`, `REPLY_to_019fba18.md`, `tests/autodeblur_regression.py`, and polar sheet generator. | Adds `tests/metrics.py` (the grey test evaluator). | Both branches provide essential, complementary test infrastructure. |
| **Code Risk & Default Stability** | Zero regression risk (+119/-3 lines in C; defaults are bit-identical to `v4.9.2`). | Extensive core math modifications across 10 commits. | `019fbef6` is the cleanest, lowest-risk branch. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Sets the gold standard for intellectual honesty and technical debate in `REPLY_to_019fba18.md`.
  - Provides conclusive mathematical root-cause analysis for polar circle distortion.
  - Keeps default behavior 100% bit-exact to baseline while adding useful tuning knobs (`-T`, `CELUP_CAPFLOOR`).
- **Weaknesses**:
  - Does not attempt to solve the user's BW ink recovery task (`0.0%` pure-black recovery on `-r 6` BW art).

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **RECOMMENDED FOR MASTER (PUSH DIAGNOSTIC SUITE, KNOBS & REPLICAS)**
- **Justification**:
  1. While `019fba18` (`v4.9.9`) is the recommended core engine for BW autodeblur, `origin/arena/019fbef6-celup-lab` is indispensable for its diagnostic suite and documentation.
  2. Push `019fbef6`'s **`-T` texture gain** knob, **`CELUP_CAPFLOOR`** environment variable, **`tests/autodeblur_regression.py`** harness, and **`POLAR_ARTIFACT_DIAGNOSIS.md`** / **`REPLY_to_019fba18.md`** documentation directly to `master`.
  3. These tools ensure that future maintainers have direction-independent subpixel radius measurement tools and a complete record of the BW vs sharp-source trade-offs.
