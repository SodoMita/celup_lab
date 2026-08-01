# Gradient Handling Test Results

## Test Setup
- Test images: grad8_src.webp, rampnoise48_src.webp, huearc48_src.webp
- Scale: 2x
- Modes tested: bilinear, cubic, lanczos3, xbr, xbrz, super_xbr, pbcc_xbr

## Comparison Sheets Generated
- `comparison_sheets/gradient_grad8_2x.webp`
- `comparison_sheets/gradient_rampnoise_2x.webp`
- `comparison_sheets/gradient_huearc_2x.webp`

## Test Results

### Existing Tests (PASS)
- `tests/test_scales.py`: PASS (all modes)
- `tests/check_stairs.py`: PASS (ship2x, ship4x, probe-crisp)
- `tests/check_corners.py`: 1 FAIL (tip extent 84.25 < 87.00 - minor corner rounding)

### Observations

1. **super_xbr**: 
   - Works but produces results similar to or slightly worse than xbr
   - Variance threshold (100.0) may need tuning
   - Bicubic interpolation in gradient areas introduces some blur

2. **pbcc_xbr**: 
   - Has issues - produces broken/artifacted output
   - The luminance difference calculation or threshold may be incorrect
   - Needs debugging

3. **xbr/xbrz**: 
   - Continue to work well for pixel art
   - May introduce some artifacts on smooth gradients (expected)

## Issues Found

1. **pbcc_xbr broken**: The implementation has bugs causing visual artifacts
2. **super_xbr marginal improvement**: Not significantly better than xbr
3. **Corner test regression**: Tip extent slightly below threshold (84.25 vs 87.00)

## Next Steps

1. Debug pbcc_xbr implementation
2. Tune super_xbr variance threshold
3. Investigate corner rounding regression
4. Consider removing broken modes or marking as experimental

## Academic Implementation Status

- [x] PBCC algorithm implemented (but buggy)
- [x] Super xBR algorithm implemented (but marginal benefit)
- [ ] TAR (Transition Area Restriction) - not implemented
- [ ] p-lin interpolation kernel - not implemented
