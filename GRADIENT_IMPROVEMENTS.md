# Gradient Handling Improvements for xBR/xBRZ

## Academic Research Summary

### Key Papers Found

1. **"Extensible Implementation of Reliable Pixel Art Interpolation"**
   - Authors: Paweł M. Stasik, Julian Balcerek
   - Published: Foundations of Computing and Decision Sciences, Vol. 44, No. 2, 2019
   - DOI: 10.2478/fcds-2019-0011
   - Key contributions:
     - **PBCC (Proximity-Based Coefficient Correction)**: Adjusts interpolation coefficients based on pixel proximity measures
     - **TAR (Transition Area Restriction)**: Restricts interpolation in transition areas to preserve sharp edges
     - **p-lin interpolation**: New kernel designed for reliable bitmap scaling
   - Findings: xBRZ and hqx both rely on binary pixel similarity tests, which work well for edges but can struggle with smooth gradients

2. **"Structure-Aware Pixel Art Scaling via Block Size Detection"**
   - Authors: Jun Won Seo, Jun Won Lee, Jong Hyuck Lee, Jun Beom Kim, Jin-Woo Jung
   - Published: Applied Sciences (MDPI), Vol. 16, No. 5, 2026
   - Key findings:
     - xBRZ preserves original colors in straight line segments (0% color loss)
     - xBRZ only applies interpolation to regions identified as curves
     - xBRZ exhibits exceptional performance with ~1% color loss on larger coherent regions
     - Edge-directed logic is key to preserving pixel art aesthetics

3. **Super xBR (Hyllian, 2015)**
   - Not formally published but documented in shader repositories
   - Two-pass algorithm:
     - Pass 0: Calculates diagonal pixels using edge detection
     - Pass 1: Fills remaining pixels using linear filtering combinations
   - Specifically designed to handle subtle gradients and broader texture preservation
   - Extends applicability beyond strict pixel art to general low-resolution images

## Implementation

### New Modes Added

1. **super_xbr** - Super xBR gradient-aware upscaling
   - Detects gradients vs edges using local variance calculation
   - Uses bicubic interpolation for smooth gradient areas
   - Uses nearest-neighbor for sharp edges/pixel art
   - Variance threshold: 100.0 (tunable)

2. **pbcc_xbr** - Proximity-Based Coefficient Correction
   - Calculates luminance difference between center pixel and 8 neighbors
   - Threshold: 30.0 luminance units
   - Edge areas (diff > 30): bilinear interpolation
   - Smooth areas (diff ≤ 30): bicubic interpolation

### Algorithm Details

#### Super xBR (Variance-Based)
```c
variance = calculate_local_variance(pixel, 5x5_window)
if variance > threshold:
    output = bicubic_interpolation(pixel)
else:
    output = nearest_neighbor(pixel)
```

#### PBCC (Proximity-Based)
```c
lum_center = luminance(center_pixel)
lum_avg = average_luminance(8_neighbors)
diff = abs(lum_center - lum_avg)
if diff > threshold:
    output = bilinear_interpolation(pixel)
else:
    output = bicubic_interpolation(pixel)
```

## Comparison with Original xBR/xBRZ

### Original xBR (Hyllian, 2011)
- Uses YUV color space for perceptual distance
- 5×5 neighborhood pattern detection
- Complex blend ratios (1/8, 1/4, 3/8, 1/2, 5/8, 3/4, 7/8)
- Optimized for sharp edges and pixel art
- Can introduce artifacts on smooth gradients

### Original xBRZ (Zenju, 2012)
- CPU-optimized rewrite of xBR
- Multi-core support, 64-bit compatible
- Better transparency handling
- Still primarily edge-focused

### New Gradient-Aware Modes
- **super_xbr**: Better for images with mixed content (gradients + edges)
- **pbcc_xbr**: Academic approach, more conservative gradient handling
- Both preserve sharp edges while smoothing gradients
- Less "blocky" appearance on gradient-heavy images

## Usage Examples

```bash
# Standard xBR (best for pure pixel art)
./celup_lab input.webp output.webp 4 --mode xbr

# Super xBR (better for gradients)
./celup_lab input.webp output.webp 4 --mode super_xbr

# PBCC (academic approach)
./celup_lab input.webp output.webp 4 --mode pbcc_xbr

# xBRZ (CPU-optimized, multi-core)
./celup_lab input.webp output.webp 4 --mode xbrz
```

## Performance Characteristics

- **xbr**: Fast, optimized for pixel art
- **xbrz**: Fastest, multi-core optimized
- **super_xbr**: Moderate, variance calculation adds overhead
- **pbcc_xbr**: Moderate, neighbor calculation adds overhead

## Recommendations

- **Pure pixel art**: Use `xbr` or `xbrz`
- **Mixed content (gradients + edges)**: Use `super_xbr`
- **Academic/research**: Use `pbcc_xbr`
- **Performance-critical**: Use `xbrz`

## Future Work

- Implement TAR (Transition Area Restriction) from Stasik & Balcerek
- Add p-lin interpolation kernel
- Combine with Super xBR two-pass approach
- Add machine learning-based gradient detection (NNEDI3/waifu2x approach)
