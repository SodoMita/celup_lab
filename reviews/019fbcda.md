# Review: `arena/019fbcda-celup-lab`

## Scope and delta

Tip `82b5ffc` claims to fix corner rounding and SDF halos by reverting unstable mass-conserving depth, returning to ERF profile fitting, and adding a local 3×3 envelope clamp. It also adds xBR/xBRZ sources, include-path WebP headers, fixture generation, ctypes corner/stair tests, and a sheet script (14 files, +1,359/−489); it deletes the two historical autodeblur notes.

## Comparison

- It directly conflicts in direction with `019fba1b`'s DSDF consensus: this branch favors a constrained local clamp and stable ERF path rather than a broad consensus DSDF rewrite.
- It shares source-layout additions with `019fba12` and `019fbf57`; `019fbf57` is the later resampler line if Jinc2 is desired, so duplicate xBR/header work should not be integrated twice.
- `019fba18` attacks washed/pole artifacts in tangent-oriented evidence integration, not SDF halos. Both may be useful, but their interaction must be measured.

## Review finding

**Hold; potentially valuable focused repair.** The stated rollback is a good sign of recognizing instability, and independent ctypes checks could improve testability. However, this branch bundles unrelated resampler/build additions, removes useful documentation, and has no demonstrated comparison against the final `019fba18` behavior.

## Integration option

First port only the ERF/envelope-clamp change plus portable corner/stair tests. Do not take xBR files or local headers unless separately selected. Test it against master and the `019fba18` candidate, especially acute corners, dark/transparent edges, and smooth gradients.
