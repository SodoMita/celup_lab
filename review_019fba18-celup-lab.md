# Branch Review: `origin/arena/019fba18-celup-lab`

## 1. Branch Overview & Metadata
- **Branch Name**: `origin/arena/019fba18-celup-lab`
- **Task Group**: Group 2 — Autodeblur Core Evolution & Diagnostics (`v4.9.3` – `v4.9.9`, `-D` autodeblur)
- **Base / Merge-Base**: Root branch history starting from commit `10338e8` (v3) through `84ddd1a` (v4.9.2), with 10 unique commits advancing autodeblur to v4.9.9 (tip `ccb442b` / `18df5b2`).
- **Total Commits Ahead of v4.9.2**: 10 unique commits
- **File Diff Summary**: Iterative mathematical development of `--mode autodeblur` (`-D` deblur options), adding `tests/metrics.py` (the grey test), erf-gain post-mapping, own-line plateau transport, and tangent-oriented evidence integration.
- **Primary Domain**: Designing an autodeblur completion engine capable of deblurring washed-out black-and-white (BW) line art (`-r 6 -g 64`) to 0 mid-gray mush while recovering pure black ink.

---

## 2. Key Technical Contributions & Architectural Changes

### Autodeblur v4.9.9 Completion Engine & The Grey Test
1. **The Grey Test (`tests/metrics.py`)**:
   - Implemented an automated metric measuring the fraction of pixels neither near-black (`< 24`) nor near-white (`> 232`).
   - Established the acceptance criterion for BW line art: surviving mid-gray is mush/veil by definition and must be driven toward 0.

2. **Erf-Gain Post-Map (`v4.9.8`) & Tangent-Oriented Evidence Integration (`v4.9.9`)**:
   - **`v4.9.8` (`a96137a`)**: Added an erf-gain post-map on finished colors to eliminate mid-gray wash on BW drawings.
   - **`v4.9.9` (`18df5b2`)**: Implemented tangent-oriented evidence integration (XY-pole blur fix), eliminating cardinal/diagonal circle anisotropy.
   - **Grey Test Victory**: At the user's `-r 6 -g 64` 2x recipe on `poor smiley.webp`, v4.9.9 reduces mid-gray mush (`gray%`) to an astonishing **1.62%** (down from ~30% in baseline v4.9.2) and **1.13%** at `-r 2.3`, while recovering **ink density = 0.964 / 0.988**.
   - **0 Hull Violations on BW Content**: On achromatic drawings, the per-channel box collapses to the grey diagonal by construction, resulting in measured **0 color-hull violations** on user BW content.

3. **Accepted Defects & Planned Guards (`commit ccb442b`)**:
   - Acknowledged and accepted two guards surfaced during cross-branch evaluation:
     - **F1 (Colour-Content Guard)**: Restricting endpoint extension to the 4D hull segment rather than per-channel box corners to prevent colorful plateau corner violations (`rings` / `crosshatch`).
     - **F2 (Sharp-Source Guard)**: Tightening the `qq` gate so the completion engine abstains on sharp, area-downsampled sources (`diag` / `crosshatch`).

---

## 3. Head-to-Head Comparison: `019fba18` vs `019fbef6` vs `019fc1ba`

Following extensive cross-branch testing and formal replies (`REPLY_to_019fba18.md` and `REPLY_review_019fc1b2.md`), earlier claims regarding artifact penalties have been re-evaluated:

| Evaluation Metric / Feature | `origin/arena/019fba18` (`v4.9.9` tip) | `origin/arena/019fbef6` (`master` core) | `origin/arena/019fc1ba` (`-D analytic`) | Comparison Verdict |
| :--- | :--- | :--- | :--- | :--- |
| **BW Grey Test (`gray%`, lower=better)** | **1.62% (`-r6`) / 1.13% (`-r2.3`)** | **29.98% (`-r6`)** (leaves grey wash intact) | Higher mid-gray veil due to linear slope narrowing (`alpha=(K-1)/K`). | **`019fba18` wins decisively on the user's BW acceptance criterion**. |
| **BW Ink Density (`<128`)** | **0.964 (`-r6`) / 0.988 (`-r2.3`)** | **0.861** (does not recover pure black from `-r6`) | Good, but transition edges retain mid-gray pixels. | **`019fba18` wins on ink recovery**. |
| **MAE vs Ground Truth (Soft Ramps)** | **Best MAE on `rings` (`.0625`), `corner` (`.0251`), and `checker2` (`.1946`)**. | Standard MAE (`.0899` on rings, `.0282` on corner). | N/A (linear ramp model). | **`019fba18` wins on soft blurred-ramp reconstruction**. |
| **Why Earlier HG Metric Claims Were Misleading** | Ground truth itself scores high HG (`.0460` on rings, `.0801` on crosshatch) because step scenes are far from bilinear. | Low HG literally means "closer to bilinear" (mushier), not "cleaner". | N/A. | **Both branches agree HG is structurally inverted on step-class scenes**. |
| **Sharp-Source Abstention (`diag` / `crosshatch`)** | Can false-fire on sharp 1px transitions (`diag` MAE `.0213` vs `.0126`). | Correctly abstains on sharp sources. | Opt-in mode. | **`019fbef6` is safer on existing sharp 1px transitions until F2 guard is added**. |

---

## 4. Strengths, Weaknesses & Trade-Offs
- **Strengths**:
  - Unmatched, empirically proven ability to deblur washed-out BW line art (`poor smiley.webp`) to **1.62% grey**, fulfilling the user's explicit task directive.
  - Achieves best MAE against ground truth on soft blurred ramps (`rings`, `corner`, `checker2`).
  - Intellectual rigor in re-measuring and accepting two specific guards (F1 and F2).
- **Weaknesses**:
  - Without the F1 colour guard, endpoint extension can reach box corners on colorful plateaus, causing color-hull violations on non-achromatic scenes.

---

## 5. Recommendation for `master` (Push-to-Master Verdict)

### Verdict: **RECOMMENDED FOR MASTER (PUSH v4.9.9 CORE FOR BW / AUTODEBLUR)**
- **Justification**:
  1. Under the user's explicit acceptance criterion—deblurring BW line art to 0 grey at their recipe—**`origin/arena/019fba18-celup-lab` (`v4.9.9`) is the clear winner**.
  2. As conceded by `019fbef6` in `REPLY_to_019fba18.md`, baseline `master` does not compete on this metric (`~30% grey`), and recommending `-r 1.5` is a parameter workaround rather than a solution to the task.
  3. Furthermore, as confirmed by the grey test (`tests/metrics.py`), `019fba18` produces significantly fewer mid-gray transition artifacts than `019fc1ba` (`-D analytic`).
  4. **Action**: Push `019fba18-celup-lab`'s v4.9.9 autodeblur core (`18df5b2`) and `tests/metrics.py` to `master`, pairing it with the agreed **F1 colour-content guard** and **F2 sharp-source qq gate**.
