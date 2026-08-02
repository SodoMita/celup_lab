# Branch integration reviews — 2026-08-02

One review exists for every remotely visible Arena branch **except the branch the requester explicitly excluded**.  These are desk reviews: commit history, tip-to-`master` file delta, and inter-branch ancestry/difference were inspected; claims in branch commit messages are reported as claims, not independently reproduced test results.

## Important comparison limitation

`master` is a shallow/grafted snapshot (`f6466c9`), while most candidate branches retain an older parent history. Consequently Git cannot find a merge base for several candidates. “Delta from master” in the reviews therefore means the practical snapshot/file delta at the tips, **not** a safe merge/cherry-pick range. Integration should be by a deliberately selected commit or by porting a narrowly reviewed feature, followed by a clean build and the relevant test suite.

| Branch | Area | Recommendation |
|---|---|---|
| `019fba0d` | housekeeping | Do not merge |
| `019fba12` | xBR cleanup | Do not merge as a whole; superseded by `019fbf57` for its useful baseline |
| `019fba14` | v7 resampler rewrite | Hold/reject pending isolated evaluation |
| `019fba18` | autodeblur v4.9.9 | Strong candidate, but integrate separately from analytical work |
| `019fba1b` | DSDF consensus | Hold for A/B evaluation |
| `019fbcda` | corner/SDF repair | Hold; overlaps with the xBR line and needs regression review |
| `019fbef6` | texture gain/cap knob | Cherry-pick/port only the small knob after test; do not take evidence artifacts |
| `019fbf57` | Jinc2 auto-tuning | Candidate only if Jinc2 is wanted; test and clean asset churn |
| `019fbf78` | revised analytical deblur | Prefer over `019fbfb9` for the same experiment; still experimental |
| `019fbfb9` | first analytical deblur | Do not merge; superseded by `019fbf78` |
