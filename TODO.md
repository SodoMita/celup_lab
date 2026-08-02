# TODO — merge1 branch

## Completed ✓

### Branch merge
- All 13 feature branches merged into `merge1`
- Comparison sheets removed from repo history
- `merge1` branch force-pushed to origin (8 commits)

### All fixes applied
- jinc2_bilateral: IDENTICAL to 019fbf57
- xBR: real Hyllian xBR, IDENTICAL to 019fbf57
- adaptive parameters: IDENTICAL to 019fba1b
- dsdf: 3x3 consensus matches 019fba1b HEAD
- xbrz/xbr/hybrid dispatch: added

### Soft hull fix (v4.9.4)
- qconf-gated hull expansion allows deblur steepening on pixel art
- poor_smiley: MSE 213→176, speckle 1.19%→1.02%; cat unchanged

### Kernel floor restoration (v4.9.3)
- Restored bspline/box/gaussian kernel floors to 019fba18 values
- auto_tune now selects bspline (matching 019fba18)

### 019fba18 autodeblur_pass port
- Full 2003-line autodeblur_pass ported
- Combined with kernel floor fix, merge1 now matches 019fba18 on all images

### Gradient deblur method 4
- Ported 019fc1ba's global gradient walk + u-remap
- Best compromise: poor_smiley MSE 160, cat MSE 40

### Compress2x2 deblur method 5
- Ported 019fbf78's bilinear 2x2 block projection as method 5
- Best on photos (cat MSE 7.7), worst on pixel art (poor_smiley MSE 392.8)

### Feature ablation framework
- CELUP_NOHULL/CELUP_NOSPECKLE env vars + autodeblur_ablate.py

### Code quality: globals consolidation, warnings, dead code
- **34 file-scope statics** consolidated into a single `celup_config` struct
- Struct accessed via `g_cfg` pointer, set once in `main()` to stack-local storage
- Compiler can now track lifetime and optimize (no aliasing concerns)
- **9 compiler warnings fixed** (format string, unused vars, misleading indentation, operator precedence)
- **Dead code removed**: `deblur_texgain`, `autodeblur_is_photo`, `adb_keff_*` (3 vars)
- **Zero warnings** with `-Wall -Wextra -O3`

## Remaining items

### 1. 019fbfb9 branch — no action needed
- Build failure is a C code bug (missing braces), no unique content

### 2. Steepness penalty
- merge1: 0.55 (stronger, prevents staircase tracking)
- 019fba18: 0.30 (more permissive)
- Current 0.55 is intentional

### 3. Consider making gradient method (4) the default for autodeblur
- Best compromise: good on both pixel art and photos

### 4. Further optimization opportunities
- Pass `celup_config *cfg` as parameter instead of using `g_cfg` global pointer
  (would allow the compiler to prove no aliasing in the deepest loops)
- Add `__attribute__((pure))` to pure functions (e.g., `compress_curve`, `phi1`)
- Mark inner loop hot paths with `__builtin_expect` for branch prediction
- SIMD vectorization of the per-pixel inner loops in autodeblur_pass

## Deblur method comparison (proxy MSE, 2x downscale)

| Method        | poor_smiley | cat     | pikachu | Notes                                    |
|---------------|-------------|---------|---------|------------------------------------------|
| remap         | 130.2       | 7.9     | 8.6     | Good balance, default for auto           |
| push          | 133.0       | 8.0     | 8.9     | Similar to remap, slightly different     |
| analytical    | 3.5         | 5783.6  | 143.3   | Best on pixel art, catastrophic on photos|
| gradient      | 19.5        | 32.2    | 1.7     | Best compromise                          |
| compress2x2   | 392.8       | 7.7     | 10.3    | Best on photos, worst on pixel art       |
