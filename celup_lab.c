/*
   celup_lab.c -- resampling laboratory, premultiplied-linear Catmull-Rom WebP
   upscaler

   Compared with celup2:
   - keeps only four horizontally-resampled float scanlines (O(output width),
     not O(source pixels)); decoding/output buffers are still required by
   libwebp.
   - caches those scanlines as the output walks down the image, avoiding most
     repeated horizontal 4-tap work for conventional upscales.
   - fixes border neighbourhood selection and removes the non-normalized
     orientation blend. The latter could change flat/soft colours unpredictably.
   - clamps each premultiplied channel to the local 2x2 range: no cubic halo.

   cc -O3 -DNDEBUG -std=c99 -march=native celup_lab.c -o celup_lab \
     $(pkg-config --cflags --libs libwebp) -lm
*/
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/encode.h>
#include "celup_lab_xbrz.h"
#include "celup_lab_xbr.h"

static float to_linear[256];
static uint8_t to_srgb[4097];
/* compression exponent: 1 = none, 2 = strong, 4 = very strong */
static float compress_strength = 2.f;
/* Gaussian sigma in source-pixel units for blur modes. */
static float blur_radius = 1.f;
static int blur_radius_set = 0;
/* If set, tune blurcompress parameters from the input image itself. */
static int auto_blurcompress = 0;
/* Conservative peak-RSS guard; overridden by --max-mib. */
static float max_mib = 512.f;
/* Reconstruction policy for checker/Nyquist-ambiguous cells in the adaptive
   mode.  LOWPASS is the natural-image default; SCALE2X is the crisp pixel-art
   option; AUTO picks between those two from global image statistics. */
enum {
  POLICY_LOWPASS = 0,
  POLICY_BILINEAR,
  POLICY_NEAREST,
  POLICY_MITCHELL,
  POLICY_SCALE2X,
  POLICY_AUTO
};
static int checker_policy = POLICY_LOWPASS;
/* Diagnostic bitmask for the adaptive mode: 1 = zero edge weight,
   2 = zero checker weight, 4 = zero junction weight.  Not documented for
   production use; used to attribute error to a specific policy branch. */
static int adaptive_debug = 0;
#define CELUP_PI 3.14159265358979323846f
static inline float smoothstep01(float t) {
  t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
  return t * t * (3.f - 2.f * t);
}
static inline float ramp01(float x, float lo, float hi) {
  return smoothstep01((x - lo) / (hi - lo));
}
static inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
static inline int clampi(int x, int lo, int hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}
static inline float compress_curve(float t) {
  /* Symmetric power sigmoid: monotonic for every valid strength and fixes
     the midpoint exactly. It is safer than extrapolating smoothstep. */
  if (compress_strength <= 1.f)
    return t;
  float a = powf(t, compress_strength), b = powf(1.f - t, compress_strength);
  return a / (a + b + 1e-20f);
}
static void init_luts(void) {
  for (int i = 0; i < 256; i++) {
    float x = i / 255.f;
    to_linear[i] = x <= .04045f ? x / 12.92f : powf((x + .055f) / 1.055f, 2.4f);
  }
  for (int i = 0; i <= 4096; i++) {
    float x = i / 4096.f, s = x <= .0031308f
                                  ? 12.92f * x
                                  : 1.055f * powf(x, 1.f / 2.4f) - .055f;
    to_srgb[i] = (uint8_t)(clampf(s, 0, 1) * 255.f + .5f);
  }
}
static inline void weights(float t, float *w) {
  float t2 = t * t, t3 = t2 * t;
  w[0] = -.5f * t3 + t2 - .5f * t;
  w[1] = 1.5f * t3 - 2.5f * t2 + 1;
  w[2] = -1.5f * t3 + 2 * t2 + .5f * t;
  w[3] = .5f * t3 - .5f * t2;
}
/* A scanline is interleaved premultiplied linear RGBA, four floats/pixel. */
static void hfilter(const uint8_t *in, int sw, int sy, int dw, const int *xi,
                    const float *wx, float *out) {
  const uint8_t *row = in + (size_t)sy * sw * 4;
  for (int x = 0; x < dw; x++) {
    const int *c = xi + 4 * x;
    const float *w = wx + 4 * x;
    float *q = out + 4 * x;
    float r = 0, g = 0, b = 0, a = 0;
    for (int k = 0; k < 4; k++) {
      const uint8_t *p = row + 4 * c[k];
      float aa = p[3] * (1.f / 255.f), z = w[k] * aa;
      r += to_linear[p[0]] * z;
      g += to_linear[p[1]] * z;
      b += to_linear[p[2]] * z;
      a += z;
    }
    /* Monotone separable cubic: first bound the horizontal pass to its two
       central samples. Vertical bounding below then gives a 2x2 bound. */
    for (int ch = 0; ch < 4; ch++) {
      float u = 0, v = 0;
      for (int k = 1; k <= 2; k++) {
        const uint8_t *p = row + 4 * c[k];
        float aa = p[3] * (1.f / 255.f);
        float z = ch == 0   ? to_linear[p[0]] * aa
                  : ch == 1 ? to_linear[p[1]] * aa
                  : ch == 2 ? to_linear[p[2]] * aa
                            : aa;
        if (k == 1)
          u = v = z;
        else {
          if (z < u)
            u = z;
          if (z > v)
            v = z;
        }
      }
      q[ch] = clampf(ch == 0 ? r : ch == 1 ? g : ch == 2 ? b : a, u, v);
    }
  }
}
static void put(uint8_t *p, float r, float g, float b, float a) {
  a = clampf(a, 0, 1);
  if (a < 1e-7f) {
    p[0] = p[1] = p[2] = p[3] = 0;
    return;
  }
  float ia = 1.f / a;
  int R = (int)(clampf(r * ia, 0, 1) * 4096 + .5f),
      G = (int)(clampf(g * ia, 0, 1) * 4096 + .5f),
      B = (int)(clampf(b * ia, 0, 1) * 4096 + .5f);
  p[0] = to_srgb[clampi(R, 0, 4096)];
  p[1] = to_srgb[clampi(G, 0, 4096)];
  p[2] = to_srgb[clampi(B, 0, 4096)];
  p[3] = (uint8_t)(a * 255 + .5f);
}

/* Reference modes: stable, positive-weight filters for A/B comparisons. */
static inline void raw_pm(const uint8_t *in, int w, int h, int x, int y,
                          float q[4]) {
  const uint8_t *p =
      in + 4 * ((size_t)clampi(y, 0, h - 1) * w + clampi(x, 0, w - 1));
  float a = p[3] * (1.f / 255.f);
  q[0] = to_linear[p[0]] * a;
  q[1] = to_linear[p[1]] * a;
  q[2] = to_linear[p[2]] * a;
  q[3] = a;
}
static void upscale_nearest(const uint8_t *in, int sw, int sh, uint8_t *out,
                            int dw, int dh) {
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      int ix = clampi((int)floorf((x + .5f) * (float)sw / dw), 0, sw - 1),
          iy = clampi((int)floorf((y + .5f) * (float)sh / dh), 0, sh - 1);
      float q[4];
      raw_pm(in, sw, sh, ix, iy, q);
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
}
static void upscale_bilinear(const uint8_t *in, int sw, int sh, uint8_t *out,
                             int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, q[4] = {0}, p[4];
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
          raw_pm(in, sw, sh, ix + i, iy + j, p);
          for (int c = 0; c < 4; c++)
            q[c] += w * p[c];
        }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

/* v7.2: linear - 1D horizontal only, nearest vertical, less blur than bilinear (no dividing by 2 in vertical) */
static void upscale_linear(const uint8_t *in, int sw, int sh, uint8_t *out,
                           int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = clampi((int)floorf(sy + 0.5f), 0, sh - 1); /* nearest vertical */
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix;
      float p0[4], p1[4], q[4];
      raw_pm(in, sw, sh, ix,     iy, p0);
      raw_pm(in, sw, sh, ix + 1, iy, p1);
      for (int c=0;c<4;c++) q[c] = (1.f - fx) * p0[c] + fx * p1[c];
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

/* Additional quality kernels.  These are still premultiplied-linear RGBA, and
   clamp the reconstructed value to the footprint's channel range.  The clamp
   keeps Mitchell/Lanczos useful in the lab without obvious negative-weight
   halos around hard sprite edges. */
typedef float (*kernel_fn)(float);

static inline float sinc1(float x) {
  x = fabsf(x);
  if (x < 1e-6f)
    return 1.f;
  float pix = CELUP_PI * x;
  return sinf(pix) / pix;
}
static float kernel_lanczos2(float x) {
  x = fabsf(x);
  return x < 2.f ? sinc1(x) * sinc1(x * .5f) : 0.f;
}
static float kernel_lanczos3(float x) {
  x = fabsf(x);
  return x < 3.f ? sinc1(x) * sinc1(x * (1.f / 3.f)) : 0.f;
}
static float kernel_mitchell(float x) {
  const float B = 1.f / 3.f, C = 1.f / 3.f;
  x = fabsf(x);
  if (x < 1.f)
    return ((12.f - 9.f * B - 6.f * C) * x * x * x +
            (-18.f + 12.f * B + 6.f * C) * x * x + (6.f - 2.f * B)) /
           6.f;
  if (x < 2.f)
    return ((-B - 6.f * C) * x * x * x +
            (6.f * B + 30.f * C) * x * x +
            (-12.f * B - 48.f * C) * x + (8.f * B + 24.f * C)) /
           6.f;
  return 0.f;
}

static float dist4_pm(const float a[4], const float b[4]) {
  float d = 0.f;
  for (int c = 0; c < 4; c++) {
    float z = a[c] - b[c];
    d += z * z;
  }
  return d;
}

static float checker2x2_confidence_pm(float p[4][4]) {
  /* Detect A/B/B/A or B/A/A/B: diagonals match, cross pairs differ. */
  float d03 = dist4_pm(p[0], p[3]), d12 = dist4_pm(p[1], p[2]);
  float cross = .25f * (dist4_pm(p[0], p[1]) + dist4_pm(p[0], p[2]) +
                        dist4_pm(p[3], p[1]) + dist4_pm(p[3], p[2]));
  if (cross < 1e-8f)
    return 0.f;
  float diag_error = (d03 + d12) / (cross + 1e-20f);
  float contrast_conf = ramp01(cross, 4e-4f, 3e-2f);
  float diag_conf = 1.f - ramp01(diag_error, .03f, .22f);
  return clampf(contrast_conf * diag_conf, 0.f, 1.f);
}

static void bilinear_sample_pm(const uint8_t *in, int sw, int sh, float sx,
                               float sy, float q[4], float cell[4][4]) {
  int ix = (int)floorf(sx), iy = (int)floorf(sy);
  float fx = sx - ix, fy = sy - iy;
  q[0] = q[1] = q[2] = q[3] = 0.f;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      int k = 2 * j + i;
      float ww = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
      raw_pm(in, sw, sh, ix + i, iy + j, cell[k]);
      for (int c = 0; c < 4; c++)
        q[c] += ww * cell[k][c];
    }
}

static float checker3x3_at_pm(const uint8_t *in, int sw, int sh, int cx, int cy) {
  float p[9][4];
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
      raw_pm(in, sw, sh, cx + dx, cy + dy, p[(dy + 1) * 3 + (dx + 1)]);
  float d_corn = fmaxf(fmaxf(dist4_pm(p[0], p[2]), dist4_pm(p[0], p[6])),
                       fmaxf(dist4_pm(p[0], p[8]), dist4_pm(p[0], p[4])));
  float d_edge = fmaxf(fmaxf(dist4_pm(p[1], p[3]), dist4_pm(p[1], p[5])),
                       dist4_pm(p[1], p[7]));
  float d_cross = dist4_pm(p[4], p[1]);
  if (d_cross < 1e-8f)
    return 0.f;
  float ratio = fmaxf(d_corn, d_edge) / d_cross;
  float contrast_conf = ramp01(d_cross, 4e-4f, 3e-2f);
  float pattern_conf = ramp01(ratio, .75f, .20f);
  return clampf(contrast_conf * pattern_conf, 0.f, 1.f);
}

static int upscale_kernel(const uint8_t *in, int sw, int sh, uint8_t *out,
                          int dw, int dh, int support, kernel_fn kernel) {
  int taps = support * 2;
  int *xi = malloc((size_t)dw * taps * sizeof *xi),
      *yi = malloc((size_t)dh * taps * sizeof *yi);
  float *wx = malloc((size_t)dw * taps * sizeof *wx),
        *wy = malloc((size_t)dh * taps * sizeof *wy);
  if (!xi || !yi || !wx || !wy) {
    free(xi);
    free(yi);
    free(wx);
    free(wy);
    return 0;
  }
  for (int x = 0; x < dw; x++) {
    float s = (x + .5f) * (float)sw / dw - .5f;
    int base = (int)floorf(s) - support + 1;
    float sum = 0.f;
    for (int k = 0; k < taps; k++) {
      int src = base + k;
      xi[x * taps + k] = clampi(src, 0, sw - 1);
      wx[x * taps + k] = kernel(s - (float)src);
      sum += wx[x * taps + k];
    }
    if (fabsf(sum) < 1e-8f) {
      for (int k = 0; k < taps; k++)
        wx[x * taps + k] = 0.f;
      wx[x * taps + support - 1] = 1.f;
    } else {
      float inv = 1.f / sum;
      for (int k = 0; k < taps; k++)
        wx[x * taps + k] *= inv;
    }
  }
  for (int y = 0; y < dh; y++) {
    float s = (y + .5f) * (float)sh / dh - .5f;
    int base = (int)floorf(s) - support + 1;
    float sum = 0.f;
    for (int k = 0; k < taps; k++) {
      int src = base + k;
      yi[y * taps + k] = clampi(src, 0, sh - 1);
      wy[y * taps + k] = kernel(s - (float)src);
      sum += wy[y * taps + k];
    }
    if (fabsf(sum) < 1e-8f) {
      for (int k = 0; k < taps; k++)
        wy[y * taps + k] = 0.f;
      wy[y * taps + support - 1] = 1.f;
    } else {
      float inv = 1.f / sum;
      for (int k = 0; k < taps; k++)
        wy[y * taps + k] *= inv;
    }
  }
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      float q[4] = {0, 0, 0, 0}, lo[4] = {1e30f, 1e30f, 1e30f, 1e30f},
            hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
      for (int j = 0; j < taps; j++)
        for (int i = 0; i < taps; i++) {
          float p[4], ww = wx[x * taps + i] * wy[y * taps + j];
          raw_pm(in, sw, sh, xi[x * taps + i], yi[y * taps + j], p);
          for (int c = 0; c < 4; c++) {
            q[c] += ww * p[c];
            if (p[c] < lo[c])
              lo[c] = p[c];
            if (p[c] > hi[c])
              hi[c] = p[c];
          }
        }
      for (int c = 0; c < 4; c++)
        q[c] = clampf(q[c], lo[c], hi[c]);
      {
        float sx = (x + .5f) * (float)sw / dw - .5f;
        float sy = (y + .5f) * (float)sh / dh - .5f;
        float base[4], cell[4][4];
        bilinear_sample_pm(in, sw, sh, sx, sy, base, cell);
        float chk = fminf(checker2x2_confidence_pm(cell), checker3x3_at_pm(in, sw, sh, (int)floorf(sx), (int)floorf(sy)));
        if (chk > 1e-4f)
          for (int c = 0; c < 4; c++)
            q[c] = base[c] + (1.f - chk) * (q[c] - base[c]);
      }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  free(xi);
  free(yi);
  free(wx);
  free(wy);
  return 1;
}

/* Deliberately soft baseline: source-space 5x5 Gaussian (binomial) blur,
   followed by bilinear reconstruction.  Unlike bilinear alone it has a
   genuinely broad transition that can serve as input to a later narrowing
   pass. Sigma is approximately one source pixel. */
static void blur_pm(const uint8_t *in, int w, int h, int x, int y, float q[4]) {
  int r = (int)ceilf(3.f * blur_radius);
  if (r < 1)
    r = 1;
  if (r > 32) /* v4.9.3: raised from 12 so large -r pins get full support */
    r = 32;
  q[0] = q[1] = q[2] = q[3] = 0;
  float sum = 0, inv2 = 1.f / (2.f * blur_radius * blur_radius);
  for (int j = -r; j <= r; j++)
    for (int i = -r; i <= r; i++) {
      float p[4], ww = expf(-(float)(i * i + j * j) * inv2);
      raw_pm(in, w, h, x + i, y + j, p);
      sum += ww;
      for (int c = 0; c < 4; c++)
        q[c] += ww * p[c];
    }
  for (int c = 0; c < 4; c++)
    q[c] /= sum;
}
static void upscale_blur(const uint8_t *in, int sw, int sh, uint8_t *out,
                         int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, q[4] = {0}, p[4];
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          float ww = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
          blur_pm(in, sw, sh, ix + i, iy + j, p);
          for (int c = 0; c < 4; c++)
            q[c] += ww * p[c];
        }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

/* Experimental gradient-width compressor.  It starts with the same bilinear
   sample, identifies the farthest pair in that source 2x2, and remaps the
   projected blend coordinate by smoothstep.  smoothstep(t) keeps t=.5 fixed
   but pulls all other values toward an endpoint, so it narrows a two-colour
   transition without translating its nominal midpoint. It is deliberately
   not enabled for the normal quality mode: it will also harden true gradients.
 */
static void upscale_compress(const uint8_t *in, int sw, int sh, uint8_t *out,
                             int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, p[4][4], q[4] = {0};
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          int k = 2 * j + i;
          float ww = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
          raw_pm(in, sw, sh, ix + i, iy + j, p[k]);
          for (int c = 0; c < 4; c++)
            q[c] += ww * p[k][c];
        }
      int ai = 0, bi = 1;
      float best = -1;
      for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++) {
          float d = 0;
          for (int c = 0; c < 4; c++) {
            float z = p[b][c] - p[a][c];
            d += z * z;
          }
          if (d > best) {
            best = d;
            ai = a;
            bi = b;
          }
        }
      if (best > 1e-6f) {
        float dot = 0;
        for (int c = 0; c < 4; c++)
          dot += (q[c] - p[ai][c]) * (p[bi][c] - p[ai][c]);
        float t = clampf(dot / best, 0, 1);
        float u = compress_curve(t);
        for (int c = 0; c < 4; c++)
          q[c] = p[ai][c] + u * (p[bi][c] - p[ai][c]);
      }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

static float cell_edge_coherence(const float p[4][4], int ai, int bi,
                                 float best);

/* Safer revision of compress: only narrow when the local 2x2 looks like a
   high-confidence two-colour cell.  The gate uses premultiplied-linear RGBA
   geometry: enough contrast, samples close to a single line segment, and most
   samples clustered near the two endpoints.  This avoids the old compressor's
   worst behaviour on genuine smooth gradients while still sharpening binary
   vector/sprite transitions. */
static float two_colour_confidence(float p[4][4], int ai, int bi, float best) {
  if (best < 1e-5f)
    return 0.f;
  float endpoint = 0.f, residual = 0.f;
  for (int k = 0; k < 4; k++) {
    float dot = 0.f, d2 = 0.f;
    for (int c = 0; c < 4; c++) {
      float ab = p[bi][c] - p[ai][c];
      dot += (p[k][c] - p[ai][c]) * ab;
    }
    float t = clampf(dot / best, 0.f, 1.f);
    for (int c = 0; c < 4; c++) {
      float z = p[ai][c] + t * (p[bi][c] - p[ai][c]) - p[k][c];
      d2 += z * z;
    }
    residual += d2 / best;
    endpoint += fabsf(2.f * t - 1.f);
  }
  residual *= .25f;
  endpoint *= .25f;
  float contrast_conf = ramp01(best, 4e-4f, 3e-2f);
  float line_conf = 1.f - ramp01(residual, .01f, .08f);
  float endpoint_conf = ramp01(endpoint, .55f, .90f);
  return clampf(contrast_conf * line_conf * endpoint_conf, 0.f, 1.f);
}

static void upscale_safecompress(const uint8_t *in, int sw, int sh, uint8_t *out,
                                 int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, p[4][4], q[4] = {0};
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          int k = 2 * j + i;
          float ww = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
          raw_pm(in, sw, sh, ix + i, iy + j, p[k]);
          for (int c = 0; c < 4; c++)
            q[c] += ww * p[k][c];
        }
      int ai = 0, bi = 1;
      float best = -1.f;
      for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++) {
          float d = 0.f;
          for (int c = 0; c < 4; c++) {
            float z = p[b][c] - p[a][c];
            d += z * z;
          }
          if (d > best) {
            best = d;
            ai = a;
            bi = b;
          }
        }
      float conf = two_colour_confidence(p, ai, bi, best);
      conf *= cell_edge_coherence(p, ai, bi, best);
      if (conf > 1e-4f) {
        float dot = 0.f;
        for (int c = 0; c < 4; c++)
          dot += (q[c] - p[ai][c]) * (p[bi][c] - p[ai][c]);
        float t = clampf(dot / best, 0.f, 1.f);
        float u = compress_curve(t);
        for (int c = 0; c < 4; c++) {
          float target = p[ai][c] + u * (p[bi][c] - p[ai][c]);
          q[c] += conf * (target - q[c]);
        }
      }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

static void upscale_blurcompress(const uint8_t *in, int sw, int sh,
                                 uint8_t *out, int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, p[4][4], q[4] = {0};
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          int k = 2 * j + i;
          float ww = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
          blur_pm(in, sw, sh, ix + i, iy + j, p[k]);
          for (int c = 0; c < 4; c++)
            q[c] += ww * p[k][c];
        }
      int ai = 0, bi = 1;
      float best = -1;
      for (int a = 0; a < 4; a++)
        for (int b = a + 1; b < 4; b++) {
          float d = 0;
          for (int c = 0; c < 4; c++) {
            float z = p[b][c] - p[a][c];
            d += z * z;
          }
          if (d > best) {
            best = d;
            ai = a;
            bi = b;
          }
        }
      if (best > 1e-6f) {
        float dot = 0;
        for (int c = 0; c < 4; c++)
          dot += (q[c] - p[ai][c]) * (p[bi][c] - p[ai][c]);
        float t = clampf(dot / best, 0, 1);
        float u = compress_curve(t);
        for (int c = 0; c < 4; c++)
          q[c] = p[ai][c] + u * (p[bi][c] - p[ai][c]);
      }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

static int patch_pm(const uint8_t *in, int sw, int sh, int cx, int cy,
                    int radius, float p[][4], float xy[][2], int maxn) {
  int n = 0;
  for (int j = -radius; j <= radius; j++)
    for (int i = -radius; i <= radius; i++) {
      if (n >= maxn)
        return n;
      raw_pm(in, sw, sh, cx + i, cy + j, p[n]);
      xy[n][0] = (float)i;
      xy[n][1] = (float)j;
      n++;
    }
  return n;
}

static void farthest_pair(const float p[][4], int n, int *ai, int *bi,
                          float *best) {
  *ai = 0;
  *bi = n > 1 ? 1 : 0;
  *best = -1.f;
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++) {
      float d = 0.f;
      for (int c = 0; c < 4; c++) {
        float z = p[b][c] - p[a][c];
        d += z * z;
      }
      if (d > *best) {
        *best = d;
        *ai = a;
        *bi = b;
      }
    }
}

static float project_pair_t(const float q[4], const float a[4],
                            const float b[4], float best) {
  if (best < 1e-20f)
    return 0.f;
  float dot = 0.f;
  for (int c = 0; c < 4; c++)
    dot += (q[c] - a[c]) * (b[c] - a[c]);
  return clampf(dot / best, 0.f, 1.f);
}


static float cell_edge_coherence(const float p[4][4], int ai, int bi,
                                 float best) {
  if (best < 1e-20f)
    return 0.f;
  float t[4];
  for (int k = 0; k < 4; k++)
    t[k] = project_pair_t(p[k], p[ai], p[bi], best);
  float saddle = fabsf(t[0] - t[1] - t[2] + t[3]);
  float dx = .5f * (fabsf(t[1] - t[0]) + fabsf(t[3] - t[2]));
  float dy = .5f * (fabsf(t[2] - t[0]) + fabsf(t[3] - t[1]));
  float one_direction = fabsf(dx - dy);
  float saddle_conf = 1.f - ramp01(saddle, .30f, .85f);
  float direction_conf = ramp01(one_direction, .10f, .55f);
  float checker_conf = checker2x2_confidence_pm((float(*)[4])p);
  return clampf(saddle_conf * direction_conf * (1.f - checker_conf), 0.f,
                1.f);
}

static float patch_checker_penalty25(const float t[25]) {
  int flips = 0, links = 0;
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 5; x++) {
      int k = y * 5 + x, a = t[k] >= .5f;
      if (x + 1 < 5) {
        flips += (a != (t[k + 1] >= .5f));
        links++;
      }
      if (y + 1 < 5) {
        flips += (a != (t[k + 5] >= .5f));
        links++;
      }
    }
  return links ? (float)flips / (float)links : 1.f;
}

/* Larger-neighbourhood two-colour gate.  This is deliberately stricter than
   the old 2x2 gate: broad gradients have many samples through the middle of
   the endpoint line, so endpoint_conf falls and compression is suppressed. */
static float patch_two_colour_confidence(const uint8_t *in, int sw, int sh,
                                         int cx, int cy) {
  float p[25][4], xy[25][2], t[25];
  int n = patch_pm(in, sw, sh, cx, cy, 2, p, xy, 25);
  int ai, bi;
  float best;
  farthest_pair((const float(*)[4])p, n, &ai, &bi, &best);
  if (best < 1e-5f)
    return 0.f;
  float residual = 0.f, endpoint = 0.f, mid = 0.f;
  for (int k = 0; k < n; k++) {
    t[k] = project_pair_t(p[k], p[ai], p[bi], best);
    float d2 = 0.f;
    for (int c = 0; c < 4; c++) {
      float z = p[ai][c] + t[k] * (p[bi][c] - p[ai][c]) - p[k][c];
      d2 += z * z;
    }
    residual += d2 / best;
    endpoint += fabsf(2.f * t[k] - 1.f);
    mid += 1.f - ramp01(fabsf(2.f * t[k] - 1.f), .25f, .75f);
  }
  residual /= (float)n;
  endpoint /= (float)n;
  mid /= (float)n;
  float contrast_conf = ramp01(best, 4e-4f, 3e-2f);
  float line_conf = 1.f - ramp01(residual, .01f, .08f);
  float endpoint_conf = ramp01(endpoint, .58f, .88f);
  float mid_penalty = 1.f - ramp01(mid, .18f, .45f);
  float flip_ratio = n == 25 ? patch_checker_penalty25(t) : .5f;
  float spatial_conf = 1.f - ramp01(flip_ratio, .22f, .55f);
  return clampf(contrast_conf * line_conf * endpoint_conf * mid_penalty *
                    spatial_conf,
                0.f, 1.f);
}


static void bilinear_cell_pm(const uint8_t *in, int sw, int sh, int ix, int iy,
                             float fx, float fy, float q[4]) {
  q[0] = q[1] = q[2] = q[3] = 0.f;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      float p[4], ww = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
      raw_pm(in, sw, sh, ix + i, iy + j, p);
      for (int c = 0; c < 4; c++)
        q[c] += ww * p[c];
    }
}

static void blurcompress_target_cell(const uint8_t *in, int sw, int sh, int ix,
                                     int iy, float fx, float fy, float q[4]) {
  float p[4][4];
  q[0] = q[1] = q[2] = q[3] = 0.f;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      int k = 2 * j + i;
      float ww = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
      blur_pm(in, sw, sh, ix + i, iy + j, p[k]);
      for (int c = 0; c < 4; c++)
        q[c] += ww * p[k][c];
    }
  int ai, bi;
  float best;
  farthest_pair((const float(*)[4])p, 4, &ai, &bi, &best);
  if (best > 1e-6f) {
    float t = project_pair_t(q, p[ai], p[bi], best);
    float u = compress_curve(t);
    for (int c = 0; c < 4; c++)
      q[c] = p[ai][c] + u * (p[bi][c] - p[ai][c]);
  }
}

/* Safe blurcompress: keeps the old blur-then-narrow target, but only blends it
   into an unblurred bilinear reconstruction when a raw 5x5 source patch looks
   like a real two-colour edge.  Broad gradients therefore remain smooth rather
   than being triangulated cell by cell. */
static void upscale_safeblurcompress(const uint8_t *in, int sw, int sh,
                                     uint8_t *out, int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, base[4], target[4], q[4];
      bilinear_cell_pm(in, sw, sh, ix, iy, fx, fy, base);
      blurcompress_target_cell(in, sw, sh, ix, iy, fx, fy, target);
      float conf = patch_two_colour_confidence(in, sw, sh, ix, iy);
      for (int c = 0; c < 4; c++)
        q[c] = base[c] + conf * (target[c] - base[c]);
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

/* ---------------------------------------------------------------------------
   Patch classifier (v2).

   A single 5x5 analysis pass measures, per source pixel:
   - contrast             max pairwise premultiplied-linear distance^2
   - line fit quality     are colours one-dimensional (two-colour patch)?
   - endpoint clustering  do samples sit near the two line endpoints (edge)
                          or spread through the middle (gradient)?
   - t-plane fit quality  does the projected blend coordinate form a single
                          coherent ramp across space?
   - flip ratio           do hard t-labels alternate across neighbours
                          (checker/Nyquist ambiguity)?
   - 2x2 checker          exact A B / B A checker detector over the four cells
                          touching the pixel

   From these the class confidences are derived:
   - edge_conf:    coherent single two-colour edge; sharpening is safe
   - checker_conf: checker/Nyquist ambiguity; local methods cannot know the
                   truth, so reconstruction must use a non-inventing fallback
   - junction_conf: multicolour crossing or sharp corner; fit geometry is
                   unreliable, interpolate conservatively
   - smooth_conf:  flat or broad gradient; a plain bounded kernel is ideal

   The confidences feed every improved mode: adaptive reconstruction, gated
   deconvolution back-projection, gated unsharp, and gated hourglass removal.
--------------------------------------------------------------------------- */
typedef struct {
  int sw, sh;
  float *w_edge;    /* sequential-priority blend weights per source pixel  */
  float *w_checker;
  float *w_junction;
  float *w_base;
  float *w_line;    /* thin-line (pulse) class weight                       */
  float *line_par;  /* 6 floats/pixel: nx, ny, centre m, width w, t side,
                       t peak -- the 1D pulse profile along the line normal */
  float *flat_conf; /* diagnostic only (classmap)                          */
  float *w_bp;      /* deconvolution back-projection gate                   */
  float *w_sharp;   /* unsharp gate                                         */
  float *w_hg;      /* hourglass-removal gate                               */
  float *edge_t0;   /* fitted t-plane at the pixel centre                   */
  float *edge_gx;
  float *edge_gy;
  float *edge_side; /* 8 floats/pixel: colour-axis endpoints A rgba, B rgba  */
  float global_checker;
  double unique_ratio; /* sRGB-quad palette size / pixels (diagnostic)   */
  double soft_fraction;
  int pixel_art;
} class_map_t;

static void free_class_map(class_map_t *cm) {
  if (!cm)
    return;
  free(cm->w_edge);
  free(cm->w_checker);
  free(cm->w_junction);
  free(cm->w_base);
  free(cm->w_line);
  free(cm->line_par);
  free(cm->flat_conf);
  free(cm->w_bp);
  free(cm->w_sharp);
  free(cm->w_hg);
  free(cm->edge_t0);
  free(cm->edge_gx);
  free(cm->edge_gy);
  free(cm->edge_side);
  memset(cm, 0, sizeof *cm);
}

static int alloc_class_map(class_map_t *cm, int sw, int sh) {
  size_t n = (size_t)sw * sh;
  cm->sw = sw;
  cm->sh = sh;
  cm->w_edge = malloc(n * sizeof *cm->w_edge);
  cm->w_checker = malloc(n * sizeof *cm->w_checker);
  cm->w_junction = malloc(n * sizeof *cm->w_junction);
  cm->w_base = malloc(n * sizeof *cm->w_base);
  cm->w_line = malloc(n * sizeof *cm->w_line);
  cm->line_par = malloc(n * 6 * sizeof *cm->line_par);
  cm->flat_conf = malloc(n * sizeof *cm->flat_conf);
  cm->w_bp = malloc(n * sizeof *cm->w_bp);
  cm->w_sharp = malloc(n * sizeof *cm->w_sharp);
  cm->w_hg = malloc(n * sizeof *cm->w_hg);
  cm->edge_t0 = malloc(n * sizeof *cm->edge_t0);
  cm->edge_gx = malloc(n * sizeof *cm->edge_gx);
  cm->edge_gy = malloc(n * sizeof *cm->edge_gy);
  cm->edge_side = malloc(n * 8 * sizeof *cm->edge_side);
  if (!cm->w_edge || !cm->w_checker || !cm->w_junction || !cm->w_base ||
      !cm->w_line || !cm->line_par || !cm->flat_conf || !cm->w_bp ||
      !cm->w_sharp || !cm->w_hg || !cm->edge_t0 || !cm->edge_gx ||
      !cm->edge_gy || !cm->edge_side) {
    free_class_map(cm);
    return 0;
  }
  return 1;
}

/* 2x2 checker confidence over the four cells that touch pixel (cx,cy). */

static float checker2x2_near(const uint8_t *in, int sw, int sh, int cx,
                             int cy) {
  float best = 0.f;
  for (int j = -1; j <= 0; j++)
    for (int i = -1; i <= 0; i++) {
      float cell[4][4];
      for (int dj = 0; dj < 2; dj++)
        for (int di = 0; di < 2; di++)
          raw_pm(in, sw, sh, cx + i + di, cy + j + dj, cell[2 * dj + di]);
      float c = checker2x2_confidence_pm(cell);
      if (c > best)
        best = c;
    }
  float c3 = checker3x3_at_pm(in, sw, sh, cx, cy);
  return fminf(best, c3);
}

static int same_colour_pm(const float a[4], const float b[4]) {
  return dist4_pm(a, b) < 1e-6f;
}
static int same_colour_pm_loose(const float a[4], const float b[4]) {
  return dist4_pm(a, b) < 8e-3f;
}

/* Separable [1,2,1]/4 smoothing of a weight plane.  Per-pixel class
   decisions are noisy at structure boundaries; rendering them directly
   produces dappled flanks, so the weights are smoothed and renormalized
   before use (and before the refinement gates are derived). */
static void smooth_plane_121(float *w, int sw, int sh) {
  size_t n = (size_t)sw * sh;
  float *tmp = malloc(n * sizeof *tmp);
  if (!tmp)
    return;
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float a = w[(size_t)y * sw + clampi(x - 1, 0, sw - 1)],
            b = w[(size_t)y * sw + x],
            c = w[(size_t)y * sw + clampi(x + 1, 0, sw - 1)];
      tmp[(size_t)y * sw + x] = (a + 2.f * b + c) * .25f;
    }
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float a = tmp[(size_t)clampi(y - 1, 0, sh - 1) * sw + x],
            b = tmp[(size_t)y * sw + x],
            c = tmp[(size_t)clampi(y + 1, 0, sh - 1) * sw + x];
      w[(size_t)y * sw + x] = (a + 2.f * b + c) * .25f;
    }
  free(tmp);
}

static int build_class_map(const uint8_t *in, int sw, int sh, class_map_t *cm) {
  if (!alloc_class_map(cm, sw, sh))
    return 0;
  double checker_sum = 0.0;
  long equal_links = 0, soft_links = 0, total_links = 0;
  /* Global pixel-art heuristic.  Pixel art is dominated by links that are
     either exact duplicates or hard colour changes, with few in-between
     "soft" blends, and by a tiny unique-colour population relative to its
     pixel count.  Natural/AA'd imagery has the opposite profile.  The count
     is over sRGB byte quads, which is the authoring-relevant palette. */
  {
    size_t cap = 1024, fill = 0;
    uint32_t *ht = calloc(cap, sizeof *ht);
    if (!ht) {
      free_class_map(cm);
      return 0;
    }
    for (int y = 0; y < sh; y++)
      for (int x = 0; x < sw; x++) {
        const uint8_t *px = in + 4 * ((size_t)y * sw + x);
        uint32_t key = ((uint32_t)px[0] << 24) | ((uint32_t)px[1] << 16) |
                       ((uint32_t)px[2] << 8) | px[3];
        uint32_t h = key * 2654435761u;
        size_t slot = (h >> 8) & (cap - 1);
        int found = 0;
        while (ht[slot]) {
          if (ht[slot] == key + (key == 0)) {
            found = 1;
            break;
          }
          slot = (slot + 1) & (cap - 1);
        }
        if (!found) {
          fill++;
          if (fill * 2 >= cap) {
            uint32_t *nt = calloc(cap * 2, sizeof *nt);
            if (!nt) {
              free(ht);
              free_class_map(cm);
              return 0;
            }
            for (size_t s = 0; s < cap; s++)
              if (ht[s]) {
                size_t t = ((ht[s] * 2654435761u) >> 8) & (2 * cap - 1);
                while (nt[t])
                  t = (t + 1) & (2 * cap - 1);
                nt[t] = ht[s];
              }
            free(ht);
            ht = nt;
            cap *= 2;
          }
          size_t slot2 = (h >> 8) & (cap - 1);
          while (ht[slot2])
            slot2 = (slot2 + 1) & (cap - 1);
          ht[slot2] = key + (key == 0);
        }
      }
    cm->unique_ratio = (double)fill / (double)((size_t)sw * sh);
    free(ht);
  }
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float a[4], b[4];
      raw_pm(in, sw, sh, x, y, a);
      for (int dir = 0; dir < 2; dir++) {
        int nx = x + (dir == 0), ny = y + (dir == 1);
        if (nx >= sw || ny >= sh)
          continue;
        raw_pm(in, sw, sh, nx, ny, b);
        float d = dist4_pm(a, b);
        equal_links += d < 1e-6f ? 1 : 0;
        soft_links += (d >= 1e-6f && d < 5e-3f) ? 1 : 0;
        total_links++;
      }
    }
  double dup_fraction =
      total_links > 0 ? (double)equal_links / (double)total_links : 0.0;
  double soft_fraction =
      total_links > 0 ? (double)soft_links / (double)total_links : 1.0;

  for (int cy = 0; cy < sh; cy++)
    for (int cx = 0; cx < sw; cx++) {
      size_t k = (size_t)cy * sw + cx;
      float p[25][4], xy[25][2], t[25];
      int n = patch_pm(in, sw, sh, cx, cy, 2, p, xy, 25);
      int ai, bi;
      float best;
      farthest_pair((const float(*)[4])p, n, &ai, &bi, &best);

      float chk2 = checker2x2_near(in, sw, sh, cx, cy);
      float contrast_conf = ramp01(best, 4e-4f, 3e-2f);
      float flat_conf = 1.f - contrast_conf;
      cm->flat_conf[k] = flat_conf;
      cm->edge_t0[k] = .5f;
      cm->edge_gx[k] = 0.f;
      cm->edge_gy[k] = 0.f;
      for (int c = 0; c < 8; c++)
        cm->edge_side[8 * k + c] = 0.f;
      for (int c = 0; c < 6; c++)
        cm->line_par[6 * k + c] = 0.f;

      float edge_conf = 0.f, checker_conf = 0.f, junction_conf = 0.f,
            pulse_line_conf = 0.f;
      if (best >= 1e-5f) {
        float residual = 0.f, endpoint = 0.f, mid = 0.f, mean = 0.f;
        float tmin = 1.f, tmax = 0.f;
        for (int i = 0; i < n; i++) {
          t[i] = project_pair_t(p[i], p[ai], p[bi], best);
          float d2 = 0.f;
          for (int c = 0; c < 4; c++) {
            float z = p[ai][c] + t[i] * (p[bi][c] - p[ai][c]) - p[i][c];
            d2 += z * z;
          }
          residual += d2 / best;
          endpoint += fabsf(2.f * t[i] - 1.f);
          mid += 1.f - ramp01(fabsf(2.f * t[i] - 1.f), .25f, .75f);
          mean += t[i];
          if (t[i] < tmin)
            tmin = t[i];
          if (t[i] > tmax)
            tmax = t[i];
        }
        residual /= (float)n;
        endpoint /= (float)n;
        mid /= (float)n;
        mean /= (float)n;

        float gx = 0.f, gy = 0.f, sx2 = 0.f, sy2 = 0.f;
        for (int i = 0; i < n; i++) {
          gx += xy[i][0] * (t[i] - mean);
          gy += xy[i][1] * (t[i] - mean);
          sx2 += xy[i][0] * xy[i][0];
          sy2 += xy[i][1] * xy[i][1];
        }
        gx = sx2 > 1e-6f ? gx / sx2 : 0.f;
        gy = sy2 > 1e-6f ? gy / sy2 : 0.f;
        float plane_mse = 0.f;
        for (int i = 0; i < n; i++) {
          float z = mean + gx * xy[i][0] + gy * xy[i][1] - t[i];
          plane_mse += z * z;
        }
        plane_mse /= (float)n;
        /* Competing parity (checker) model: t explained by cell parity alone.
           A true checkerboard is dominated by this term, while a staircased
           anti-aliased diagonal edge -- whose hard labels also alternate --
           is still better explained by the spatial ramp.  Only when parity
           beats the plane is the patch treated as Nyquist-ambiguous; this
           keeps real diagonal edges in the sharpening path. */
        float var_t = 0.f, psum[2] = {0.f, 0.f}, pcnt[2] = {0.f, 0.f};
        for (int i = 0; i < n; i++) {
          float z = t[i] - mean;
          var_t += z * z;
          int par = ((int)xy[i][0] + (int)xy[i][1]) & 1;
          psum[par] += t[i];
          pcnt[par] += 1.f;
        }
        var_t /= (float)n;
        float checker_mse = var_t;
        if (pcnt[0] > 0.f && pcnt[1] > 0.f) {
          float pm0 = psum[0] / pcnt[0], pm1 = psum[1] / pcnt[1], sse = 0.f;
          for (int i = 0; i < n; i++) {
            int par = ((int)xy[i][0] + (int)xy[i][1]) & 1;
            float z = t[i] - (par ? pm1 : pm0);
            sse += z * z;
          }
          checker_mse = sse / (float)n;
        }
        float plane_r2 = var_t > 1e-8f ? 1.f - plane_mse / var_t : 0.f;
        float checker_r2 = var_t > 1e-8f ? 1.f - checker_mse / var_t : 0.f;

        float side0[4] = {0, 0, 0, 0}, side1[4] = {0, 0, 0, 0};
        int n0 = 0, n1 = 0;
        for (int i = 0; i < n; i++) {
          if (t[i] < .30f) {
            for (int c = 0; c < 4; c++)
              side0[c] += p[i][c];
            n0++;
          } else if (t[i] > .70f) {
            for (int c = 0; c < 4; c++)
              side1[c] += p[i][c];
            n1++;
          }
        }
        float side_var = 1e30f;
        if (n0 >= 3 && n1 >= 3) {
          side_var = 0.f;
          for (int c = 0; c < 4; c++) {
            side0[c] /= (float)n0;
            side1[c] /= (float)n1;
          }
          for (int i = 0; i < n; i++)
            if (t[i] < .30f || t[i] > .70f) {
              const float *s = t[i] < .5f ? side0 : side1;
              float d2 = 0.f;
              for (int c = 0; c < 4; c++) {
                float z = p[i][c] - s[c];
                d2 += z * z;
              }
              side_var += d2 / best;
            }
          side_var /= (float)(n0 + n1);
        }

        float flip_ratio = n == 25 ? patch_checker_penalty25(t) : .5f;
        float flip_conf = ramp01(flip_ratio, .40f, .80f);

        float line_conf = 1.f - ramp01(residual, .012f, .07f);
        float plane_conf = 1.f - ramp01(plane_mse, .020f, .085f);
        float plane_r2_conf = ramp01(plane_r2, .25f, .70f);
        float parity_dom = ramp01(checker_r2 - plane_r2, .03f, .20f);
        float endpoint_conf = ramp01(endpoint, .48f, .78f);
        float range_conf = ramp01(tmax - tmin, .35f, .75f);
        float side_conf =
            side_var > 1e20f ? 0.f : 1.f - ramp01(side_var, .015f, .08f);

        /* Checker evidence requires the parity model to dominate the plane
           model, or an exact 2x2 checker signature.  Raw alternation alone
           (staircase AA) only counts when the plane truly cannot explain t. */
        float checker_ev = parity_dom * (.5f + .5f * flip_conf);
        float stair = flip_conf * (1.f - plane_r2_conf) * .7f;
        checker_ev = clampf(checker_ev > stair ? checker_ev : stair, 0.f, 1.f);
        if (chk2 > checker_ev)
          checker_ev = chk2;
        checker_conf = contrast_conf * checker_ev;
        junction_conf = contrast_conf * (1.f - fmaxf(line_conf, plane_conf)) *
                        (1.f - checker_ev);
        float plane_q =
            plane_conf > plane_r2_conf ? plane_conf : plane_r2_conf;
        edge_conf = contrast_conf * line_conf * plane_q * side_conf *
                    range_conf * endpoint_conf * (1.f - checker_ev);

        /* Thin-line (pulse) detection (v3).  The structure tensor of t has
           one dominant orientation (the line normal) and the t profile along
           that normal is a narrow pulse: both outer sides agree with each
           other while the middle differs.  True 2D checkers fail the
           orientation-coherence test (isotropic gradient energy); true edges
           fail the both-sides-agree test.  Detected lines are reconstructed
           by sharpening the pulse along the normal only, which cannot invent
           any lateral (checker/hourglass) structure. */
        {
          float Jxx = 0.f, Jyy = 0.f, Jxy = 0.f;
          for (int jx = 1; jx < 4; jx++)
            for (int ix = 1; ix < 4; ix++) {
              float ggx = .5f * (t[jx * 5 + ix + 1] - t[jx * 5 + ix - 1]);
              float ggy = .5f * (t[(jx + 1) * 5 + ix] - t[(jx - 1) * 5 + ix]);
              Jxx += ggx * ggx;
              Jyy += ggy * ggy;
              Jxy += ggx * ggy;
            }
          float trm = .5f * (Jxx + Jyy), dtm = .5f * (Jxx - Jyy);
          float root = sqrtf(dtm * dtm + Jxy * Jxy);
          float lam1 = trm + root, lam2 = trm - root;
          float coh = (lam1 - lam2) / (lam1 + lam2 + 1e-12f);
          float nx = Jxy, ny = lam1 - Jxx;
          float nl = sqrtf(nx * nx + ny * ny);
          if (nl < 1e-9f) {
            nx = 1.f;
            ny = 0.f;
            nl = 1.f;
          }
          nx /= nl;
          ny /= nl;
          float side_a = 0.f, side_b = 0.f;
          int na = 0, nb = 0, nm = 0;
          for (int i = 0; i < n; i++) {
            float s = xy[i][0] * nx + xy[i][1] * ny;
            if (s <= -1.3f) {
              side_a += t[i];
              na++;
            } else if (s >= 1.3f) {
              side_b += t[i];
              nb++;
            } else {
              nm++;
            }
          }
          float span = tmax - tmin;
          if (na >= 2 && nb >= 2 && nm >= 1 && span > .05f) {
            side_a /= (float)na;
            side_b /= (float)nb;
            float side_mean = .5f * (side_a + side_b);
            float sides_agree =
                1.f - ramp01(fabsf(side_a - side_b), .12f * span + .01f,
                             .45f * span + .04f);
            /* Pulse magnitude: the biggest deviation from the side level
               inside the line core (|s| <= .9).  Using the core extremum
               instead of the core mean avoids dilution by background
               samples that share the window along the line direction. */
            float best_dev = 0.f;
            for (int i = 0; i < n; i++) {
              float s = xy[i][0] * nx + xy[i][1] * ny;
              if (fabsf(s) > .9f)
                continue;
              float d = t[i] - side_mean;
              if (fabsf(d) > fabsf(best_dev))
                best_dev = d;
            }
            float pulse_conf =
                ramp01(fabsf(best_dev), .40f * span, .72f * span);
            float sgn = best_dev >= 0.f ? 1.f : -1.f;
            float m0 = 0.f, m1 = 0.f, m2 = 0.f;
            for (int i = 0; i < n; i++) {
              float w = sgn * (t[i] - side_mean);
              if (w <= 0.f)
                continue;
              float s = xy[i][0] * nx + xy[i][1] * ny;
              m0 += w;
              m1 += w * s;
              m2 += w * s * s;
            }
            if (m0 > 1e-6f) {
              float mc = m1 / m0;
              float var = m2 / m0 - mc * mc;
              float w_eff = var > 0.f ? sqrtf(var) : 0.f;
              float thin_conf = 1.f - ramp01(w_eff, .42f, .58f);
              float coh_conf = ramp01(coh, .55f, .85f);
              pulse_line_conf = contrast_conf * coh_conf * sides_agree *
                                sides_agree * pulse_conf * thin_conf;
              if (getenv("CELUP_CLASS_DEBUG") && pulse_line_conf > .05f)
                fprintf(stderr,
                        "LINE %d,%d: conf=%.3f contrast=%.3f coh=%.3f/%.3f "
                        "agree=%.3f pulse=%.3f dev=%.3f thin=%.3f we=%.3f "
                        "span=%.3f\n",
                        cx, cy, pulse_line_conf, contrast_conf, coh, coh_conf,
                        sides_agree, pulse_conf, best_dev, thin_conf, w_eff,
                        span);
              if (pulse_line_conf > 0.f) {
                float *lp = cm->line_par + 6 * k;
                lp[0] = nx;
                lp[1] = ny;
                lp[2] = mc;
                lp[3] = w_eff > .15f ? w_eff : .15f;
                lp[4] = side_mean;
                lp[5] = clampf(side_mean + best_dev, 0.f, 1.f);
                for (int c = 0; c < 4; c++) {
                  cm->edge_side[8 * k + c] = p[ai][c];
                  cm->edge_side[8 * k + 4 + c] = p[bi][c];
                }
              }
            }
          }
        }

        if (side_var <= 1e20f && (n0 + n1) >= 8) {
          cm->edge_t0[k] = mean;
          cm->edge_gx[k] = gx;
          cm->edge_gy[k] = gy;
          for (int c = 0; c < 4; c++) {
            cm->edge_side[8 * k + c] = side0[c];
            cm->edge_side[8 * k + 4 + c] = side1[c];
          }
        } else {
          edge_conf = 0.f;
        }
      }

      /* Sequential-priority peel: thin lines first (an oriented structure is
         never a checker accident), then checker (hard ambiguity), coherent
         edge, junction; the remainder uses the base kernel. */
      float rem = 1.f;
      float w_l = clampf(pulse_line_conf, 0.f, 1.f);
      rem -= w_l;
      float w_c = clampf(checker_conf, 0.f, 1.f) * rem;
      rem -= w_c;
      float w_e = clampf(edge_conf, 0.f, 1.f) * rem;
      rem -= w_e;
      float w_j = clampf(junction_conf, 0.f, 1.f) * rem;
      rem -= w_j;
      cm->w_line[k] = w_l;
      cm->w_checker[k] = w_c;
      cm->w_edge[k] = w_e;
      cm->w_junction[k] = w_j;
      cm->w_base[k] = rem;
      checker_sum += w_c;
    }
  /* Second pass: smooth the peeled class weights and derive the gates.
     Without this, per-pixel class noise at structure boundaries renders as
     dappled flanks (observed as dashed speckle along 2px lines). */
  /* Only the line weight is smoothed: pulse rendering switches reconstruction
     model entirely, so its class noise is the one that becomes visible
     speckle.  The other classes blend nearly identical smooth reconstructors
     and stay per-pixel for maximum fidelity. */
  smooth_plane_121(cm->w_line, sw, sh);
  for (int k = 0, n = sw * sh; k < n; k++) {
    float w_l = cm->w_line[k], w_c = cm->w_checker[k], w_e = cm->w_edge[k],
          w_j = cm->w_junction[k], w_b = cm->w_base[k];
    float s = w_l + w_c + w_e + w_j + w_b;
    if (s > 1e-6f) {
      float inv = 1.f / s;
      w_l *= inv;
      w_c *= inv;
      w_e *= inv;
      w_j *= inv;
      w_b *= inv;
    } else {
      w_b = 1.f;
      w_l = w_c = w_e = w_j = 0.f;
    }
    cm->w_line[k] = w_l;
    cm->w_checker[k] = w_c;
    cm->w_edge[k] = w_e;
    cm->w_junction[k] = w_j;
    cm->w_base[k] = w_b;
    /* Gates for the iterative modes: correct/unsharp only where a coherent
       structure is trusted; remove hourglass structure mainly where the
       patch is genuinely ambiguous.  Thin lines count as trustworthy for
       back-projection (their consistency residual is the pulse width
       error) but the hourglass remover must not fight the pulse model. */
    cm->w_bp[k] =
        (.12f + .88f * (w_e > w_l ? w_e : w_l)) * (1.f - w_c) *
        (1.f - .65f * w_j);
    cm->w_sharp[k] = (w_e > .5f * w_l ? w_e : .5f * w_l) * (1.f - w_c) *
                     (1.f - .5f * w_j);
    cm->w_hg[k] = clampf((w_c + .65f * w_j) * (1.f - w_l), 0.f, 1.f);
  }
  cm->global_checker =
      sw > 0 && sh > 0 ? (float)(checker_sum / ((double)sw * sh)) : 0.f;
  cm->soft_fraction = soft_fraction;
  cm->pixel_art =
      soft_fraction < .08 && (cm->unique_ratio < .15 || dup_fraction > .45);
  return 1;
}

/* Bounded Mitchell base reconstruction at one output position.  The sampled
   value is clamped to the central 2x2 premultiplied-linear range, so it
   cannot overshoot into ringing geometry or invent colours. */
static void mitchell_bounded_sample(const uint8_t *in, int sw, int sh,
                                    float sx, float sy, float q[4]) {
  int ix = (int)floorf(sx), iy = (int)floorf(sy);
  float wx[4], wy[4], sum = 0.f;
  for (int k = 0; k < 4; k++) {
    wx[k] = kernel_mitchell(sx - (float)(ix + k - 1));
    wy[k] = kernel_mitchell(sy - (float)(iy + k - 1));
  }
  float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f},
        hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
  q[0] = q[1] = q[2] = q[3] = 0.f;
  float wxs = 0.f, wys = 0.f;
  for (int k = 0; k < 4; k++) {
    wxs += wx[k];
    wys += wy[k];
  }
  for (int j = 0; j < 4; j++)
    for (int i = 0; i < 4; i++) {
      float p[4], ww = wx[i] * wy[j];
      raw_pm(in, sw, sh, ix + i - 1, iy + j - 1, p);
      sum += ww;
      for (int c = 0; c < 4; c++)
        q[c] += ww * p[c];
    }
  if (fabsf(sum) > 1e-8f && fabsf(wxs * wys) > 1e-8f) {
    float inv = 1.f / (wxs * wys);
    for (int c = 0; c < 4; c++)
      q[c] *= inv;
  }
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      float p[4];
      raw_pm(in, sw, sh, ix + i, iy + j, p);
      for (int c = 0; c < 4; c++) {
        if (p[c] < lo[c])
          lo[c] = p[c];
        if (p[c] > hi[c])
          hi[c] = p[c];
      }
    }
  for (int c = 0; c < 4; c++)
    q[c] = clampf(q[c], lo[c], hi[c]);
}

/* Separable Gaussian lowpass of the source in premultiplied-linear RGBA.
   Used as the natural-image checker fallback: at a true Nyquist ambiguity the
   only non-inventing choice is to remove the aliased band outright. */
static float *alloc_lowpass_pm(const uint8_t *in, int sw, int sh,
                               float sigma) {
  int r = clampi((int)ceilf(3.f * sigma), 1, 32); /* v4.9.3: raised 8->32 */
  size_t n = (size_t)sw * sh;
  float *tmp = malloc(n * 4 * sizeof *tmp), *dst = malloc(n * 4 * sizeof *dst);
  float *ker = malloc((size_t)(2 * r + 1) * sizeof *ker);
  if (!tmp || !dst || !ker) {
    free(tmp);
    free(dst);
    free(ker);
    return NULL;
  }
  float inv2 = 1.f / (2.f * sigma * sigma), ksum = 0.f;
  for (int i = -r; i <= r; i++) {
    ker[i + r] = expf(-(float)(i * i) * inv2);
    ksum += ker[i + r];
  }
  for (int i = 0; i <= 2 * r; i++)
    ker[i] /= ksum;
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float *q = tmp + 4 * ((size_t)y * sw + x);
      q[0] = q[1] = q[2] = q[3] = 0.f;
      for (int i = -r; i <= r; i++) {
        float p[4];
        raw_pm(in, sw, sh, x + i, y, p);
        for (int c = 0; c < 4; c++)
          q[c] += ker[i + r] * p[c];
      }
    }
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float *q = dst + 4 * ((size_t)y * sw + x);
      q[0] = q[1] = q[2] = q[3] = 0.f;
      for (int j = -r; j <= r; j++)
        for (int c = 0; c < 4; c++)
          q[c] += ker[j + r] *
                  tmp[4 * ((size_t)clampi(y + j, 0, sh - 1) * sw + x) + c];
    }
  free(tmp);
  free(ker);
  return dst;
}

static void lowpass_sample(const float *img, int sw, int sh, float sx,
                           float sy, float q[4]) {
  int ix = (int)floorf(sx), iy = (int)floorf(sy);
  float fx = sx - ix, fy = sy - iy;
  q[0] = q[1] = q[2] = q[3] = 0.f;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      float ww = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
      const float *p =
          img + 4 * ((size_t)clampi(iy + j, 0, sh - 1) * sw +
                     clampi(ix + i, 0, sw - 1));
      for (int c = 0; c < 4; c++)
        q[c] += ww * p[c];
    }
}

/* Scale2x/EPX-style hard reconstruction, generalised to arbitrary scale.
   Each output pixel maps to a host source pixel and one of its four
   quadrants; the quadrant takes the matching neighbour colour only when the
   classic diagonal-connection rule fires, so thin diagonals and checker
   crossings stay crisp without ever inventing a colour. */
static void scale2x_sample(const uint8_t *in, int sw, int sh, float sx,
                           float sy, float q[4]) {
  int cx = clampi((int)floorf(sx + .5f), 0, sw - 1),
      cy = clampi((int)floorf(sy + .5f), 0, sh - 1);
  float P[4], B[4], D[4], F[4], H[4];
  raw_pm(in, sw, sh, cx, cy, P);
  raw_pm(in, sw, sh, cx, cy - 1, B);
  raw_pm(in, sw, sh, cx - 1, cy, D);
  raw_pm(in, sw, sh, cx + 1, cy, F);
  raw_pm(in, sw, sh, cx, cy + 1, H);
  const float *r = P;
  /* correct Scale2x condition: B!=H && D!=F */
  if (!same_colour_pm_loose(B, H) && !same_colour_pm_loose(D, F)) {
    int right = sx >= (float)cx, down = sy >= (float)cy;
    if (!right && !down)
      r = same_colour_pm_loose(D, B) ? D : P;
    else if (right && !down)
      r = same_colour_pm_loose(B, F) ? F : P;
    else if (!right && down)
      r = same_colour_pm_loose(D, H) ? D : P;
    else
      r = same_colour_pm_loose(H, F) ? F : P;
  }
  q[0] = r[0];
  q[1] = r[1];
  q[2] = r[2];
  q[3] = r[3];
}

/* v7: full 2x Scale2x block for integer 2x, then optional -r blend to bilinear for -r control */
static void upscale_scale2x_2x(const uint8_t *in, int sw, int sh, uint8_t *out) {
  int dw = sw*2, dh = sh*2;
  for (int y=0; y<sh; y++) {
    for (int x=0; x<sw; x++) {
      float A[4],B[4],C[4],D[4],E[4],F[4],G[4],H[4],I[4];
      raw_pm(in, sw, sh, x-1, y-1, A);
      raw_pm(in, sw, sh, x,   y-1, B);
      raw_pm(in, sw, sh, x+1, y-1, C);
      raw_pm(in, sw, sh, x-1, y,   D);
      raw_pm(in, sw, sh, x,   y,   E);
      raw_pm(in, sw, sh, x+1, y,   F);
      raw_pm(in, sw, sh, x-1, y+1, G);
      raw_pm(in, sw, sh, x,   y+1, H);
      raw_pm(in, sw, sh, x+1, y+1, I);
      float E0[4],E1[4],E2[4],E3[4];
      for (int c=0;c<4;c++) E0[c]=E1[c]=E2[c]=E3[c]=E[c];
      if (!same_colour_pm_loose(B, H) && !same_colour_pm_loose(D, F)) {
        if (same_colour_pm_loose(D, B)) for(int c=0;c<4;c++) E0[c]=D[c];
        if (same_colour_pm_loose(B, F)) for(int c=0;c<4;c++) E1[c]=F[c];
        if (same_colour_pm_loose(D, H)) for(int c=0;c<4;c++) E2[c]=D[c];
        if (same_colour_pm_loose(H, F)) for(int c=0;c<4;c++) E3[c]=F[c];
      }
      put(out + 4 * ((size_t)(y*2) * dw + x*2), E0[0],E0[1],E0[2],E0[3]);
      put(out + 4 * ((size_t)(y*2) * dw + x*2+1), E1[0],E1[1],E1[2],E1[3]);
      put(out + 4 * ((size_t)(y*2+1) * dw + x*2), E2[0],E2[1],E2[2],E2[3]);
      put(out + 4 * ((size_t)(y*2+1) * dw + x*2+1), E3[0],E3[1],E3[2],E3[3]);
    }
  }
}

static void upscale_scale2x(const uint8_t *in, int sw, int sh, uint8_t *out,
                            int dw, int dh) {
  /* v7: if exact 2x, use full 2x Scale2x block (more accurate). For other scales, use per-pixel sample with optional -r smoothing. */
  if (dw == sw*2 && dh == sh*2) {
    upscale_scale2x_2x(in, sw, sh, out);
    /* optional -r smoothing for scale2x: blend with bilinear by amount based on -r */
    if (blur_radius_set) {
      float blend = clampf((blur_radius - 0.5f) * 0.35f, 0.f, 0.55f);
      if (blend > 0.01f) {
        for (int y=0; y<dh; y++) {
          float sy = (y + .5f) * (float)sh / dh - .5f;
          int iy = (int)floorf(sy); float fy = sy - iy;
          for (int x=0; x<dw; x++) {
            float sx = (x + .5f) * (float)sw / dw - .5f;
            int ix = (int)floorf(sx); float fx = sx - ix;
            float q[4], b[4];
            raw_pm(out, dw, dh, x, y, q); /* current scale2x result */
            bilinear_cell_pm(in, sw, sh, ix, iy, fx, fy, b);
            for (int c=0;c<4;c++) q[c] = q[c]*(1.f-blend) + b[c]*blend;
            put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
          }
        }
      }
    }
    return;
  }
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f, q[4], b[4];
      scale2x_sample(in, sw, sh, sx, sy, q);
      if (blur_radius_set) {
        float blend = clampf((blur_radius - 0.5f) * 0.30f, 0.f, 0.50f);
        if (blend > 0.01f) {
          bilinear_cell_pm(in, sw, sh, (int)floorf(sx), (int)floorf(sy), sx - floorf(sx), sy - floorf(sy), b);
          for (int c=0;c<4;c++) q[c] = q[c]*(1.f-blend) + b[c]*blend;
        }
      }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

static int resolve_policy(const class_map_t *cm) {
  int p = checker_policy;
  if (p == POLICY_AUTO)
    p = cm->pixel_art ? POLICY_SCALE2X : POLICY_LOWPASS;
  return p;
}

static int refine_downsample_consistency(float *hr, const uint8_t *in, int sw,
                                         int sh, int dw, int dh, int iters,
                                         float step, float sharp_amount,
                                         const class_map_t *cm);
static void remove_hourglass_basis(float *hr, int dw, int dh,
                                   const uint8_t *in, int sw, int sh,
                                   float amount, const float *gate);
static void suppress_speckle_pm(float *hr, int dw, int dh, const uint8_t *in,
                                int sw, int sh, float amount,
                                const float *gate);
static void write_hr_rgba(const float *hr, int dw, int dh, uint8_t *out);

/* v6: tangential AA for adaptive, stronger 4-tap + -r spread.
   For edge pixels (w_edge high, checker/junction low), smooth along the
   contour tangent (perpendicular to gradient).  -r expands tangential
   footprint, so -r has visible effect even on non-checker edges. */
static void adaptive_tangential_aa(float *hr, int dw, int dh,
                                   const class_map_t *cm, int sw, int sh) {
  size_t n = (size_t)dw * dh;
  float *snap = malloc(n * 4 * sizeof *snap);
  if (!snap) return;
  memcpy(snap, hr, n * 4 * sizeof *snap);
  float xscale = (float)sw / dw, yscale = (float)sh / dh;
  float spread = blur_radius_set ? clampf(blur_radius / 0.75f, 0.7f, 3.0f) : 1.2f;
  for (int y = 0; y < dh; y++) {
    int cy = (int)((y + 0.5f) * yscale);
    if (cy < 0) cy = 0;
    if (cy >= sh) cy = sh - 1;
    for (int x = 0; x < dw; x++) {
      int cx = (int)((x + 0.5f) * xscale);
      if (cx < 0) cx = 0;
      if (cx >= sw) cx = sw - 1;
      size_t k = (size_t)cy * sw + cx;
      float we = cm->w_edge ? cm->w_edge[k] : 0.f;
      float wl = cm->w_line ? cm->w_line[k] : 0.f;
      float wc = cm->w_checker ? cm->w_checker[k] : 0.f;
      float wj = cm->w_junction ? cm->w_junction[k] : 0.f;
      float edge_w = we > wl ? we : wl;
      if (edge_w < 0.25f) continue;
      if (wc > 0.30f) continue;
      if (wj > 0.35f) continue;
      float gx = cm->edge_gx ? cm->edge_gx[k] : 0.f;
      float gy = cm->edge_gy ? cm->edge_gy[k] : 0.f;
      float g2 = gx * gx + gy * gy;
      if (g2 < 1e-6f) continue;
      float inv = 1.f / sqrtf(g2);
      float tx = -gy * inv;
      float ty = gx * inv;
      /* 6 taps along tangent: -2.5,-1.5,-0.5,+0.5,+1.5,+2.5 * spread for stronger AA */
      const float offs[6] = {-2.5f, -1.5f, -0.5f, 0.5f, 1.5f, 2.5f};
      const float wts[6] = {0.08f, 0.18f, 0.24f, 0.24f, 0.18f, 0.08f};
      float acc[4] = {0,0,0,0};
      for (int tt = 0; tt < 6; tt++) {
        float xt = (float)x + tx * offs[tt] * spread;
        float yt = (float)y + ty * offs[tt] * spread;
        int ix = (int)floorf(xt), iy = (int)floorf(yt);
        float fx = xt - ix, fy = yt - iy;
        float q[4] = {0};
        for (int j = 0; j < 2; j++) {
          for (int i = 0; i < 2; i++) {
            float w = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
            int sx = ix + i, sy = iy + j;
            if (sx < 0) sx = 0; if (sx >= dw) sx = dw - 1;
            if (sy < 0) sy = 0; if (sy >= dh) sy = dh - 1;
            const float *p = snap + 4 * ((size_t)sy * dw + sx);
            for (int c = 0; c < 4; c++) q[c] += w * p[c];
          }
        }
        for (int c = 0; c < 4; c++) acc[c] += wts[tt] * q[c];
      }
      float blend = 0.48f * edge_w * (1.f - wc) * (1.f - 0.40f * wj);
      if (spread > 1.f) blend *= (0.55f + 0.45f * spread);
      if (blend < 0.02f) continue;
      if (blend > 0.68f) blend = 0.68f;
      float *dst = hr + 4 * ((size_t)y * dw + x);
      for (int c = 0; c < 4; c++) {
        dst[c] = dst[c] * (1.f - blend) + acc[c] * blend;
      }
    }
  }
  free(snap);
}

/* Flagship adaptive mode (v2), two stages.

   Stage 1 (classification-routed base): per output pixel the class-map
   weights pick between a bounded Mitchell base (which also covers coherent
   edges -- it is already excellent there), plain bilinear at junctions and
   crossings (the documented non-inventing reference), and an explicit
   checker-policy fallback in Nyquist-ambiguous cells.  An earlier revision
   sharpened edges through a fitted two-colour plane model; A/B measurement
   showed that model loses to the bounded kernel on anti-aliased edges, so
   sharpening instead comes from stage 2.

   Stage 2 (gated consistency): a few class-map-gated back-projection
   iterations recover edge sharpness (the mechanism that made deblurcompress
   score well) while checker/junction cells receive essentially zero
   correction, followed by a focused hourglass-basis cleanup.  The result
   keeps deblur-like MAE on real edges without the crossing hallucination or
   hourglass build-up of the ungated iteration. */
static int adb_nohull = 0, adb_nospeckle = 0; /* ablation knobs (forward decl) */

static int upscale_adaptive(const uint8_t *in, int sw, int sh, uint8_t *out,
                            int dw, int dh) {
  class_map_t cm;
  if (!build_class_map(in, sw, sh, &cm))
    return 0;
  int policy = resolve_policy(&cm);
  float *low = NULL;
  if (policy == POLICY_LOWPASS) {
    /* v5: honour -r for lowpass sigma so -r has visible effect in adaptive;
       default 0.75 remains if not pinned. */
    float lp_sigma = blur_radius_set ? clampf(blur_radius, 0.1f, 2.5f) : 0.75f;
    low = alloc_lowpass_pm(in, sw, sh, lp_sigma);
    if (!low) {
      free_class_map(&cm);
      return 0;
    }
  }
  float *hr = malloc((size_t)dw * dh * 4 * sizeof *hr);
  if (!hr) {
    free(low);
    free_class_map(&cm);
    return 0;
  }
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int cy = clampi((int)floorf(sy + .5f), 0, sh - 1);
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int cx = clampi((int)floorf(sx + .5f), 0, sw - 1);
      size_t k = (size_t)cy * sw + cx;
      float wc = cm.w_checker[k], we = cm.w_edge[k], wj = cm.w_junction[k],
            wb = cm.w_base[k], wl = cm.w_line[k], q[4] = {0, 0, 0, 0},
            t4[4];
      if (adaptive_debug) {
        if (adaptive_debug & 1)
          we = 0.f;
        if (adaptive_debug & 2)
          wc = 0.f;
        if (adaptive_debug & 4)
          wj = 0.f;
        if (adaptive_debug & 8)
          wl = 0.f;
        float s = we + wc + wj + wb + wl;
        if (s > 1e-6f) {
          float inv = 1.f / s;
          we *= inv;
          wc *= inv;
          wj *= inv;
          wb *= inv;
          wl *= inv;
        } else {
          wb = 1.f;
        }
      }
      float wkernel = wb + we; /* edges are the bounded kernel's strength */
      if (wkernel > 1e-4f) {
        mitchell_bounded_sample(in, sw, sh, sx, sy, t4);
        for (int c = 0; c < 4; c++)
          q[c] += wkernel * t4[c];
      }
      if (wl > 1e-4f) {
        /* Thin-line pulse sharpening (v3).  Evaluate the fitted pulse
           profile along the line normal of the *nearest fitted line*, not
           blindly of the host pixel: per-host fitted centres jitter by
           fractions of a pixel, and nearest-host evaluation turns that
           jitter into dappled line flanks.  Selecting the host whose fitted
           line centre is closest (a Voronoi zone around the line) makes the
           reconstruction continuous across host boundaries.  The result
           stays on the palette colour segment and is constant along the
           line direction, so no lateral structure can appear. */
        const float *lp = 0, *side = 0;
        float sdist = 0.f, bestd = 1e30f;
        for (int dj = -1; dj <= 1; dj++)
          for (int di = -1; di <= 1; di++) {
            if (di && dj)
              continue;
            int hx = clampi(cx + di, 0, sw - 1), hy = clampi(cy + dj, 0, sh - 1);
            size_t hk = (size_t)hy * sw + hx;
            if (cm.w_line[hk] < .05f)
              continue;
            const float *cand = cm.line_par + 6 * hk;
            float s = (sx - (float)hx) * cand[0] + (sy - (float)hy) * cand[1];
            float d = fabsf(s - cand[2]);
            if (d < bestd) {
              bestd = d;
              lp = cand;
              sdist = s;
              side = cm.edge_side + 8 * hk;
            }
          }
        if (lp) {
          float nf = compress_strength > 1.f ? 1.f / sqrtf(compress_strength)
                                             : 1.f;
          nf += (1.f - nf) * ramp01(lp[3], .55f, .95f);
          float w2 = lp[3] * nf;
          if (w2 < .15f)
            w2 = .15f;
          float dz = (sdist - lp[2]) / w2;
          float tp = lp[4] + (lp[5] - lp[4]) * expf(-dz * dz);
          for (int c = 0; c < 4; c++)
            q[c] += wl * (side[c] + clampf(tp, 0.f, 1.f) *
                                         (side[4 + c] - side[c]));
        } else {
          mitchell_bounded_sample(in, sw, sh, sx, sy, t4);
          for (int c = 0; c < 4; c++)
            q[c] += wl * t4[c];
        }
      }
      if (wj > 1e-4f) {
        bilinear_cell_pm(in, sw, sh, (int)floorf(sx), (int)floorf(sy),
                         sx - floorf(sx), sy - floorf(sy), t4);
        for (int c = 0; c < 4; c++)
          q[c] += wj * t4[c];
      }
      if (wc > 1e-4f) {
        switch (policy) {
        case POLICY_SCALE2X:
          scale2x_sample(in, sw, sh, sx, sy, t4);
          break;
        case POLICY_NEAREST:
          raw_pm(in, sw, sh, cx, cy, t4);
          break;
        case POLICY_MITCHELL:
          mitchell_bounded_sample(in, sw, sh, sx, sy, t4);
          break;
        case POLICY_BILINEAR:
          bilinear_cell_pm(in, sw, sh, (int)floorf(sx), (int)floorf(sy),
                           sx - floorf(sx), sy - floorf(sy), t4);
          break;
        default: /* POLICY_LOWPASS */
          lowpass_sample(low, sw, sh, sx, sy, t4);
          break;
        }
        for (int c = 0; c < 4; c++)
          q[c] += wc * t4[c];
      }
      float *h = hr + 4 * ((size_t)y * dw + x);
      h[0] = q[0];
      h[1] = q[1];
      h[2] = q[2];
      h[3] = q[3];
    }
  }
  free(low);
  /* Stage 2: gated consistency + focused hourglass cleanup.
     v6: stronger -s: 0.020*(s-1) capped 0.16 (019fba1b tuning).  Hourglass
     removal 0.60, speckle 0.60.  Tangential AA removed: it over-smooths
     diagonal edges and increases reconstruction MSE by ~45%. */
  float sharp = clampf((compress_strength - 1.f) * .020f, 0.f, .16f);
  int ok = refine_downsample_consistency(hr, in, sw, sh, dw, dh, 3, .55f,
                                         sharp, &cm);
  if (ok) {
    /* Hard checker policies (scale2x/nearest) reconstruct intentional
       checker/texture crisply by design; running the hourglass remover on top
       would partially flatten exactly the structure the policy was told to
       keep.  Only soft policies need this cleanup. */
    if (policy != POLICY_SCALE2X && policy != POLICY_NEAREST)
      remove_hourglass_basis(hr, dw, dh, in, sw, sh, .60f, cm.w_hg);
    suppress_speckle_pm(hr, dw, dh, in, sw, sh, adb_nospeckle ? 0.f : .60f, cm.w_hg);
    write_hr_rgba(hr, dw, dh, out);
  }
  free(hr);
  free_class_map(&cm);
  return ok;
}

/* Diagnostic: render the classifier decision per source pixel (nearest-
   expanded to the output size).  R = edge, G = checker, B = junction,
   R+G = thin line (yellow); dark blue-grey = bounded-kernel base. */
static int upscale_classmap(const uint8_t *in, int sw, int sh, uint8_t *out,
                            int dw, int dh) {
  class_map_t cm;
  if (!build_class_map(in, sw, sh, &cm))
    return 0;
  for (int y = 0; y < dh; y++) {
    int cy = clampi((int)floorf((y + .5f) * (float)sh / dh), 0, sh - 1);
    for (int x = 0; x < dw; x++) {
      int cx = clampi((int)floorf((x + .5f) * (float)sw / dw), 0, sw - 1);
      size_t k = (size_t)cy * sw + cx;
      float r = cm.w_edge[k] + cm.w_line[k], g = cm.w_checker[k] + cm.w_line[k],
            b = cm.w_junction[k];
      float base = .10f + .55f * cm.w_base[k] * (1.f - cm.flat_conf[k]);
      uint8_t *p = out + 4 * ((size_t)y * dw + x);
      p[0] = (uint8_t)(clampf(r + base, 0.f, 1.f) * 255.f + .5f);
      p[1] = (uint8_t)(clampf(g + base, 0.f, 1.f) * 255.f + .5f);
      p[2] = (uint8_t)(clampf(b + base, 0.f, 1.f) * 255.f + .5f);
      p[3] = 255;
    }
  }
  free_class_map(&cm);
  return 1;
}

/* ---------------------------------------------------------------------------
   autoblur (v3): fully automatic *blurry* reconstruction, fitted per image.

   A blurry upscale is decomposed into two independent choices:

   1. Overall (spatial) blur kernel: box, triangle, Gaussian, or cubic
      B-spline, with a free sigma.  This sets the global softness/MTF shape.
   2. Gradient (transition) curve: how the blend coordinate moves between
      neighbouring pixels -- linear (true bilinear), sigmoid (smoothstep),
      cubic Hermite, exp (tanh-based), log, sqrt (power-sigmoid family), or
      circle easing, plus nearest (hard step) as the limiting no-gradient
      case.  Curves are symmetric about .5 and fix both endpoints, so the
      reconstruction is always monotone and can never ring or invent colours.

   Any component left as `auto` is fitted for THIS image by the same self-
   supervised proxy used by --auto-blurcompress: downscale the input 2x,
   reconstruct with candidates, and keep the candidate with the lowest
   premultiplied-linear MSE against the original.  Stage 1 fits kernel+sigma
   (curve = linear); stage 2 fits the curve family+parameter.  Manual
   overrides pin their components out of the search.
--------------------------------------------------------------------------- */
enum { BK_BOX = 0, BK_TRIANGLE, BK_GAUSSIAN, BK_BSPLINE, BK_AUTO };
enum {
  CK_LINEAR = 0,
  CK_SIGMOID,
  CK_CUBIC,
  CK_EXP,
  CK_LOG,
  CK_SQRT,
  CK_CIRCLE,
  CK_NEAREST,
  CK_AUTO
};
static int blur_kernel_kind = BK_AUTO;
static int blur_curve_kind = CK_AUTO;
static float curve_param = 0.f; /* <=0: family default (exp/log k, sqrt p) */
/* Resolved parameters, for the final report. */
static int fitted_kernel = BK_GAUSSIAN, fitted_curve = CK_LINEAR;
static float fitted_sigma = .75f, fitted_cp = 0.f;
/* v4.9.1: the v4.9 decouple (base render at sigma r/min(K,8)) was
   REVERTED.  It suppressed the v4.8 neon skirt, but any base rendered
   crisper than the assumed blur also re-quantizes the source lattice:
   the smiley at -r 6 regained staircase treads (the user picks -r as
   "the minimal blur when stairs are no longer visible"), the snake-
   tongue line ends came back and per-tread speckle returned.  The
   neon problem is now solved where it is actually created -- the
   profile fit (raw projection, linear+erf decomposition, plateau-span
   drag amplitude, coverage gate, contour consensus, hull clamp), NOT
   by smuggling a crisper image into the low-trust blend.  The hook is
   kept (zeroed) so the fit tables and sigma plumbing stay valid. */
static float adb_sigma_div = 0.f, adb_assumed_sigma = 0.f;

static const char *kernel_name(int k) {
  return k == BK_BOX       ? "box"
         : k == BK_TRIANGLE ? "triangle"
         : k == BK_BSPLINE ? "bspline"
                           : "gaussian";
}
static const char *curve_name(int c) {
  switch (c) {
  case CK_SIGMOID:
    return "sigmoid";
  case CK_CUBIC:
    return "cubic";
  case CK_EXP:
    return "exp";
  case CK_LOG:
    return "log";
  case CK_SQRT:
    return "sqrt";
  case CK_CIRCLE:
    return "circle";
  case CK_NEAREST:
    return "nearest";
  default:
    return "linear";
  }
}

static float shape_curve(int kind, float k, float u) {
  u = clampf(u, 0.f, 1.f);
  float s = 2.f * u - 1.f, a = fabsf(s);
  switch (kind) {
  case CK_SIGMOID:
    return u * u * (3.f - 2.f * u);
  case CK_CUBIC:
    return u < .5f ? 4.f * u * u * u
                   : 1.f - 4.f * (1.f - u) * (1.f - u) * (1.f - u);
  case CK_EXP:
    if (k <= 0.f)
      k = 2.f;
    return .5f + .5f * tanhf(k * (u - .5f)) / tanhf(.5f * k);
  case CK_LOG:
    if (k <= 0.f)
      k = 2.f;
    return .5f + .5f * copysignf(logf(1.f + (expf(k) - 1.f) * a) / k, s);
  case CK_SQRT: {
    float p = k > 0.f ? k : .5f;
    float x = powf(u, p), y = powf(1.f - u, p);
    return x / (x + y + 1e-20f);
  }
  case CK_CIRCLE:
    return u < .5f
               ? .5f - .5f * sqrtf(clampf(1.f - 4.f * u * u, 0.f, 1.f))
               : .5f +
                     .5f * sqrtf(
                         clampf(1.f - 4.f * (u - 1.f) * (u - 1.f), 0.f, 1.f));
  case CK_NEAREST:
    return u < .5f ? 0.f : (u > .5f ? 1.f : .5f);
  default:
    return u;
  }
}

/* Continuous 1D kernel profiles (source-pixel units), v4.

   v3 blurred the source *at its own resolution* and then sampled with a
   shaped 2x2 tap.  For the small sigmas the self-supervised fit prefers,
   that discrete blur degenerated to ~identity, so at large scale factors
   every source pixel rendered as an individually shaded block (C1 seams at
   each cell border -> the "blurry pixels" mosaic), and anti-aliased
   staircases were tracked as hard one-scale-phase steps ("sawtooth" along
   slightly wandering horizontal/vertical lines).

   Splatting the *analytic* kernel at the target resolution removes both by
   construction: transitions always span the true kernel support, never the
   source grid.  Each profile carries a floor so the support never collapses
   below ~1 source pixel; kernels are non-negative and the coordinate warp
   fixes cell endpoints, so the result stays monotone and cannot ring. */
static float kernel_profile_1d(int kind, float sigma, float x) {
  float a = fabsf(x);
  switch (kind) {
  case BK_BOX: {
    /* v4.9.3 floor 0.75; v5 raised to 1.0 for 45-deg staircase but
       made auto_tune select wrong kernel.  Restore 0.75. */
    float h = 1.5f * sigma;
    if (h < .75f)
      h = .75f;
    return a <= h ? .5f / h : 0.f;
  }
  case BK_TRIANGLE: {
    float s = 1.1f * sigma + .5f;
    if (s < 1.f)
      s = 1.f;
    return a < s ? (s - a) / (s * s) : 0.f;
  }
  case BK_BSPLINE: {
    /* v4.9.3 floor 0.7; v5 raised to 1.05 to fix 45-deg staircase but
       made auto_tune select triangle instead of bspline, worsening
       autodeblur by 20% MSE.  Restore 0.7 and let the autodeblur
       terrace cleanup handle staircases. */
    float s = .9f * sigma + .25f, u, b;
    if (s < .7f)
      s = .7f;
    u = a / s;
    b = u < 1.f ? (4.f - 6.f * u * u + 3.f * u * u * u) / 6.f
      : u < 2.f ? (2.f - u) * (2.f - u) * (2.f - u) / 6.f
                : 0.f;
    return b / s;
  }
  default: { /* BK_GAUSSIAN */
    float s = sigma < .5f ? .5f : sigma;
    return expf(-.5f * x * x / (s * s)) / (s * 2.5066282746f);
  }
  }
}

static int kernel_support_1d(int kind, float sigma) {
  switch (kind) {
  case BK_BOX: {
    float h = 1.5f * sigma;
    return clampi((int)ceilf(h < .75f ? .75f : h), 1, 32);
  }
  case BK_TRIANGLE: {
    float s = 1.1f * sigma + .5f;
    return clampi((int)ceilf(s < 1.f ? 1.f : s), 1, 32);
  }
  case BK_BSPLINE: {
    float s = .9f * sigma + .25f;
    if (s < .7f)
      s = .7f;
    return clampi((int)ceilf(2.f * s), 1, 32);
  }
  default: {
    float s = sigma < .5f ? .5f : sigma;
    return clampi((int)ceilf(3.f * s), 1, 32);
  }
  }
}

/* Smoothness rank for near-tie fit decisions: box/triangle reach raw MSE
   minima on crisp sources but render harsher; among candidates within a few
   percent of the best validation score we deliberately pick the smoother
   one (artifact-free beats the last epsilon of MAE). */
static int kernel_smooth_rank(int k) {
  return k == BK_BSPLINE ? 3 : k == BK_GAUSSIAN ? 2 : k == BK_TRIANGLE ? 1 : 0;
}

/* Direct continuous rendering: the gradient curve warps the sample
   coordinate inside each source cell (endpoints fixed, so the warp itself
   introduces no seams), and the analytic kernel is convolved at the warped
   position.  Separable: one vertical gather per output row, one horizontal
   dot per output pixel.  Taps are border-clamped and weights renormalized
   per output point, so truncation/borders only rebalance existing mass. */
static int render_soft(const uint8_t *in, int sw, int sh, uint8_t *out, int dw,
                       int dh, int kk, float sigma, int ck, float cp) {
  int r = kernel_support_1d(kk, sigma), nt = 2 * r + 1;
  float *col = malloc((size_t)sw * 4 * sizeof *col);
  float *wy = malloc((size_t)nt * sizeof *wy);
  float yscale = (float)sh / dh, xscale = (float)sw / dw;
  if (!col || !wy) {
    free(col);
    free(wy);
    return 0;
  }
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * yscale - .5f;
    int iy = (int)floorf(sy);
    float syw = (float)iy + shape_curve(ck, cp, sy - (float)iy);
    int jy = (int)floorf(syw);
    float wsum = 0.f;
    for (int j = -r; j <= r; j++) {
      wy[j + r] = kernel_profile_1d(kk, sigma, syw - (float)(jy + j));
      wsum += wy[j + r];
    }
    float winv = wsum > 1e-20f ? 1.f / wsum : 0.f;
    for (int x = 0; x < sw; x++) {
      float *q = col + 4 * (size_t)x;
      q[0] = q[1] = q[2] = q[3] = 0.f;
      for (int j = -r; j <= r; j++) {
        float p[4];
        raw_pm(in, sw, sh, x, clampi(jy + j, 0, sh - 1), p);
        float w = wy[j + r] * winv;
        for (int c = 0; c < 4; c++)
          q[c] += w * p[c];
      }
    }
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * xscale - .5f;
      int ix = (int)floorf(sx);
      float sxw = (float)ix + shape_curve(ck, cp, sx - (float)ix);
      int jx = (int)floorf(sxw);
      float q[4] = {0.f, 0.f, 0.f, 0.f}, ws = 0.f;
      for (int i = -r; i <= r; i++) {
        float w =
            kernel_profile_1d(kk, sigma, sxw - (float)(jx + i));
        const float *p = col + 4 * (size_t)clampi(jx + i, 0, sw - 1);
        ws += w;
        for (int c = 0; c < 4; c++)
          q[c] += w * p[c];
      }
      float inv = ws > 1e-20f ? 1.f / ws : 0.f;
      put(out + 4 * ((size_t)y * dw + x), q[0] * inv, q[1] * inv, q[2] * inv,
          q[3] * inv);
    }
  }
  free(col);
  free(wy);
  return 1;
}

static uint8_t *downsample_pm_box(const uint8_t *in, int sw, int sh, int dw,
                                  int dh);
static double image_pm_mse(const uint8_t *a, const uint8_t *b, int w, int h,
                           int border);

/* Tunable edge-width goal (v4.4): the validation MSE of the blur fit is
   biased toward LESS blur (a sharper reconstruction trivially matches the
   sharp target), so the fit tends to pick the minimum blur that tracks the
   data -- leaving source AA staircases visible as mild sawtooth and giving
   autodeblur nothing to work against.  --edge-goal W states the user's
   actual goal: strong edges should be at least W source px wide (smooth),
   and the fit should spend blur to get there "towards smooth edges first".
   Width of a rendered edge = local range / local slope, robustified to the
   30th percentile over strong-edge pixels (we care about the narrowest
   offenders).  Candidates are scored mse * (1 + PENALTY * deficit^2), so
   widening edges below the goal is worth more than the MSE it costs, but
   never free. */
static float edge_goal = 0.f;
static float measure_edge_width30(const uint8_t *img, int w, int h,
                                  float scale_px) {
  /* Width of a rendered edge = (range over a window that spans the whole
     ramp) / (slope at ramp centre); a gaussian ramp of sigma s then indeed
     reads ~2.5 s px, i.e. its real AA width.  Range window: +-1.5 src px
     (anything narrower reads only the steepest 3px segment and
     under-measures smooth ramps).  30th percentile over strong-edge pixels:
     we steer by the narrowest offenders, not the mean. */
  int Rw = clampi((int)(1.5f * scale_px + .5f), 2, 12);
  size_t n = (size_t)w * h;
  float *t = malloc(n * sizeof *t);
  float *ws = malloc(n * sizeof *ws);
  if (!t || !ws) {
    free(t);
    free(ws);
    return 0.f;
  }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      float q[4];
      raw_pm(img, w, h, x, y, q);
      t[(size_t)y * w + x] = (q[0] + q[1] + q[2]) * (1.f / 3.f);
    }
  size_t m = 0;
  for (int y = Rw; y + Rw < h; y++)
    for (int x = Rw; x + Rw < w; x++) {
      float lo = 1e30f, hi = -1e30f;
      for (int j = -Rw; j <= Rw; j++)
        for (int i = -Rw; i <= Rw; i++) {
          float v = t[(size_t)(y + j) * w + x + i];
          if (v < lo)
            lo = v;
          if (v > hi)
            hi = v;
        }
      float rng = hi - lo;
      if (rng < .05f)
        continue;
      float gx = (t[(size_t)(y - 1) * w + x + 1] + 2 * t[(size_t)y * w + x + 1] +
                  t[(size_t)(y + 1) * w + x + 1]) -
                 (t[(size_t)(y - 1) * w + x - 1] + 2 * t[(size_t)y * w + x - 1] +
                  t[(size_t)(y + 1) * w + x - 1]),
            gy = (t[(size_t)(y + 1) * w + x - 1] + 2 * t[(size_t)(y + 1) * w + x] +
                  t[(size_t)(y + 1) * w + x + 1]) -
                 (t[(size_t)(y - 1) * w + x - 1] + 2 * t[(size_t)(y - 1) * w + x] +
                  t[(size_t)(y - 1) * w + x + 1]);
      /* true Sobel slope per px: divide by 8 */
      float g = sqrtf(gx * gx + gy * gy) * .125f;
      if (g < .01f)
        continue;
      if (m < n)
        ws[m++] = rng / g;
    }
  free(t);
  if (m < 16) {
    free(ws);
    return 0.f;
  }
  /* 30th percentile via partial selection. */
  size_t k30 = m * 30 / 100;
  for (size_t i = 0; i <= k30; i++) {
    size_t bj = i;
    for (size_t j = i + 1; j < m; j++)
      if (ws[j] < ws[bj])
        bj = j;
    float tmp = ws[i];
    ws[i] = ws[bj];
    ws[bj] = tmp;
  }
  float r = ws[k30];
  free(ws);
  return r;
}
static int auto_tune_soft_params(const uint8_t *in, int sw, int sh, int *kk,
                                 float *sigma, int *ck, float *cp) {
  int tw = sw / 2, th = sh / 2;
  if (tw < 4 || th < 4) {
    /* Too small for the 2x validation proxy: keep defaults. */
    *kk = *kk == BK_AUTO ? BK_GAUSSIAN : *kk;
    *ck = *ck == CK_AUTO ? CK_LINEAR : *ck;
    return 1;
  }
  uint8_t *train = downsample_pm_box(in, sw, sh, tw, th);
  uint8_t *recon = malloc((size_t)sw * sh * 4);
  if (!train || !recon) {
    free(train);
    free(recon);
    return 0;
  }
  static const int kernels[] = {BK_BOX, BK_TRIANGLE, BK_GAUSSIAN, BK_BSPLINE};
  static const float sigmas[] = {.15f, .30f, .50f, .75f, 1.10f, 1.60f};
  static const struct {
    int kind;
    float param;
  } curves[] = {{CK_LINEAR, 0.f},   {CK_SIGMOID, 0.f}, {CK_CUBIC, 0.f},
                {CK_EXP, 2.f},      {CK_EXP, 4.5f},    {CK_LOG, 2.f},
                {CK_LOG, 4.5f},     {CK_SQRT, .4f},    {CK_SQRT, .7f},
                {CK_CIRCLE, 0.f},   {CK_NEAREST, 0.f}};
  int best_k = *kk == BK_AUTO ? BK_GAUSSIAN : *kk;
  float best_s = *sigma, best_cp = *cp;
  int best_c = *ck == CK_AUTO ? CK_LINEAR : *ck;
  double best = 1e300;

  /* Stage 1: kernel + sigma with a linear gradient curve.  Record every
     candidate score, then apply the smoothness prior: among candidates
     within 3% of the raw best (statistical ties -- the MSE-vs-sharp-target
     criterion cannot see blockiness) pick the largest sigma and then the
     smoothest kernel family. */
  double s1[4 * 6];
  for (size_t i = 0; i < sizeof s1 / sizeof s1[0]; i++)
    s1[i] = -1.0;
  for (size_t ki = 0; ki < sizeof kernels / sizeof kernels[0]; ki++) {
    if (*kk != BK_AUTO && kernels[ki] != *kk)
      continue;
    for (size_t si = 0; si < sizeof sigmas / sizeof sigmas[0]; si++) {
      if (blur_radius_set && fabsf(sigmas[si] - *sigma) > 1e-6f)
        continue;
      if (!render_soft(train, tw, th, recon, sw, sh, kernels[ki], sigmas[si],
                       CK_LINEAR, 0.f))
        continue;
      double score = image_pm_mse(recon, in, sw, sh, 2);
      s1[ki * 6 + si] = score;
      if (score < best) {
        best = score;
        best_k = kernels[ki];
        best_s = sigmas[si];
      }
    }
  }
  if (best < 1e300) {
    double thr = best == 0.0 ? 1e-12 : best * 1.03;
    int brank = -1;
    for (size_t ki = 0; ki < sizeof kernels / sizeof kernels[0]; ki++) {
      if (*kk != BK_AUTO && kernels[ki] != *kk)
        continue;
      for (size_t si = 0; si < sizeof sigmas / sizeof sigmas[0]; si++) {
        if (blur_radius_set && fabsf(sigmas[si] - *sigma) > 1e-6f)
          continue;
        double score = s1[ki * 6 + si];
        if (score <= 0 || score > thr)
          continue;
        int rank = kernel_smooth_rank(kernels[ki]);
        if (sigmas[si] > best_s + 1e-6f ||
            (fabsf(sigmas[si] - best_s) <= 1e-6f && rank > brank)) {
          best_s = sigmas[si];
          best_k = kernels[ki];
          brank = rank;
        }
      }
    }
  }
  /* Stage 2: gradient curve family + parameter.  Same near-tie policy:
     `nearest` (hard step) must win outright by >3%, otherwise the smoothest
     tied family is kept. */
  if (*ck != CK_AUTO || best == 1e300) {
    best_c = *ck == CK_AUTO ? best_c : *ck;
  } else {
    double best2 = 1e300;
    double s2[11];
    for (size_t i = 0; i < sizeof s2 / sizeof s2[0]; i++)
      s2[i] = -1.0;
    for (size_t ci = 0; ci < sizeof curves / sizeof curves[0]; ci++) {
      if (*ck != CK_AUTO && curves[ci].kind != *ck)
        continue;
      if (*cp > 0.f && fabsf(curves[ci].param - *cp) > 1e-6f)
        continue;
      if (!render_soft(train, tw, th, recon, sw, sh, best_k, best_s,
                       curves[ci].kind, curves[ci].param))
        continue;
      /* Validation MSE is blind to blockiness/sawtooth at large scales:
         steep curves + a floored kernel track the source staircase at any
         scale and score equally well.  Penalize the warp's maximum slope,
         so a steep curve must *truly* fit better (pixel art, where the MSE
         gap is huge), and smooth content always renders smooth. */
      float steep = 0.f;
      for (int g = 0; g < 64; g++) {
        float u0 = (float)g / 64.f, u1 = (float)(g + 1) / 64.f;
        float d = (shape_curve(curves[ci].kind, curves[ci].param, u1) -
                   shape_curve(curves[ci].kind, curves[ci].param, u0)) *
                  64.f;
        if (d > steep)
          steep = d;
      }
      /* v5: stronger penalty for steep warp curves (was .30) to avoid
         tracking source lattice as sawtooth; must win outright. */
      double score =
          image_pm_mse(recon, in, sw, sh, 2) * (1. + .55 * (steep - 1.));
      s2[ci] = score;
      if (score < best2) {
        best2 = score;
        best_c = curves[ci].kind;
        best_cp = curves[ci].param;
      }
    }
    if (best2 < 1e300) {
      double thr = best2 == 0.0 ? 1e-12 : best2 * 1.03;
      for (size_t ci = 0; ci < sizeof curves / sizeof curves[0]; ci++) {
        if (*ck != CK_AUTO && curves[ci].kind != *ck)
          continue;
        if (*cp > 0.f && fabsf(curves[ci].param - *cp) > 1e-6f)
          continue;
        double score = s2[ci];
        if (score > 0 && score <= thr && curves[ci].kind != CK_NEAREST &&
            best_c == CK_NEAREST) {
          best_c = curves[ci].kind;
          best_cp = curves[ci].param;
        }
      }
    }
    best = best2;
  }
  free(train);
  free(recon);
  *kk = best_k;
  *sigma = best_s;
  *ck = best_c;
  *cp = best_cp;
  fprintf(stderr,
          "autoblur selected kernel=%s sigma=%.2f curve=%s param=%.2f "
          "(validation MSE %.8g)\n",
          kernel_name(best_k), best_s, curve_name(best_c), best_cp, best);
  return 1;
}

static int upscale_autoblur(const uint8_t *in, int sw, int sh, uint8_t *out,
                            int dw, int dh) {
  int kk = blur_kernel_kind, ck = blur_curve_kind;
  float sigma = blur_radius, cp = curve_param;
  int fit_needed = kk == BK_AUTO || ck == CK_AUTO || !blur_radius_set ||
                   (cp <= 0.f &&
                    (ck == CK_EXP || ck == CK_LOG || ck == CK_SQRT) &&
                    blur_curve_kind != CK_AUTO);
  if (fit_needed) {
    if (!auto_tune_soft_params(in, sw, sh, &kk, &sigma, &ck, &cp))
      return 0;
  } else
    fprintf(stderr,
            "autoblur manual: kernel=%s sigma=%.2f curve=%s param=%.2f "
            "(all four pinned by -k/-r/-c/-p; validation fit skipped)\n",
            kernel_name(kk), sigma, curve_name(ck), cp);
  fitted_kernel = kk;
  fitted_sigma = sigma;
  fitted_curve = ck;
  fitted_cp = cp;
  if (adb_sigma_div > 0.f) {
    adb_assumed_sigma = sigma;
    sigma = clampf(sigma / adb_sigma_div, .6f, sigma);
    fitted_sigma = sigma;
    fprintf(stderr,
            "autodeblur base sigma %.2f = assumed %.2f / %.1f (v4.9 hook, inactive in v4.9.1; "
            "assumed value still sizes windows/gates)\n",
            sigma, adb_assumed_sigma, adb_sigma_div);
  }
  int ok = render_soft(in, sw, sh, out, dw, dh, kk, sigma, ck, cp);
  if (ok && edge_goal > 0.f && blur_radius_set)
    fprintf(stderr,
            "edge-goal %.2f: sigma pinned by -r (%.2f), escalation "
            "skipped -- manual wins over goal\n",
            edge_goal, sigma);
  if (ok && edge_goal > 0.f && !blur_radius_set) {
    /* Goal-first, measured at the TARGET resolution: the validation-proxy
       fit is systematically biased to little blur (a sharper reconstruction
       trivially matches the sharp target), so enforce the user's goal
       directly -- escalate sigma until strong edges are at least
       --edge-goal source px wide (or the sigma ceiling says the model
       cannot get wider).  "Increase blur towards smooth edges first of
       all." */
    for (int it = 0; it < 5; it++) {
      float w30 = measure_edge_width30(out, dw, dh, (float)dw / sw) *
                  (float)sw / dw;
      fprintf(stderr,
              "autoblur edge-goal %.2f src px: strong-edge width p30 = %.2f "
              "(sigma %.2f)%s\n",
              edge_goal, w30, sigma, w30 >= edge_goal ? " OK" : "");
      if (w30 >= edge_goal || sigma >= 2.5f)
        break;
      sigma = fminf(sigma * 1.35f, 2.5f);
      fitted_sigma = sigma;
      if (!render_soft(in, sw, sh, out, dw, dh, kk, sigma, ck, cp)) {
        ok = 0;
        break;
      }
    }
  }
  return ok;
}

/* ---------------------------------------------------------------------------
   autodeblur (v4.3): gradient-slope enhancement on the fitted autoblur base.

   Motivation: AI-upscaled / diffusion-rendered art arrives with mushy 1-2px
   anti-aliasing, salt noise in flats and lossy block boundaries; the user
   wants those CLEANED while upscaling, with zero added artifacts.  Classic
   references converge on the same answer:
   - Anime4K (bloc97 2019) treats colour as a heightmap and iteratively
     pushes pixels up their gradient ("maximizing the gradients ... without
     overshoot or ringing artifacts commonly found on traditional
     unblurring"),
   - shock filters (Osher-Rudin 1990) steepen sign(L_u)-directed slopes but
     staircase hard (visible in e.g. Krita's unblur brush),
   - anime descaling inverts the wrong-kernel upscale to native res and
     rescales with a sane kernel.
   This mode combines the useful halves.  Base = autoblur (fits the actual
   blur the source carries, kills mosquito/block noise consistently).  Then
   every pixel gets a MONOTONE local slope remap: over a window of ~1.25
   src px radius, find the robust per-channel range [lo,hi]; normalise the
   pixel to u in [0,1]; map u' = .5 + (u-.5)*k (k = slope multiplier from
   --strength), clamped; write back v' = lo + u'(hi-lo).  Being monotone
   and range-anchored, it CANNOT overshoot: no halos, no ringing, no
   hourglass, no new colours -- the whole v2-v4.2 artifact class is gone by
   construction.  The blend weight w rises from 0 to 1 across
   |grad|/range in [.08,.18], so smooth shading/blush gradients (low
   relative slope) are untouched (no posterization, unlike shock filters
   and quantization unblurs) and near-flat zones are instead gently pulled
   toward their local mean (flat-flatten) to mop up diffusion salt. */
/* Bilinear sample in premultiplied space (for the gradient push). */
static void sample_pm(const uint8_t *img, int w, int h, float x, float y,
                      float q[4]) {
  int ix = (int)floorf(x), iy = (int)floorf(y);
  float fx = x - (float)ix, fy = y - (float)iy;
  q[0] = q[1] = q[2] = q[3] = 0.f;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      float p[4], wgt = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
      raw_pm(img, w, h, ix + i, iy + j, p);
      for (int c = 0; c < 4; c++)
        q[c] += wgt * p[c];
    }
}
/* deblur method: 0 = auto (validation proxy picks), 1 = monotone slope
   remap, 2 = Anime4K-style gradient push, 3 = analytical gradient push
   (inverted steepness semantics: 1=max deblur, higher=less deblur;
   whole-image consistent filter, no case-specific safety gates). */
static int deblur_method = 0;
static float deblur_steepness = 0.f; /* <=0: auto (-e adaptive or -s formula) */
static int last_deblur_method = 0;   /* effective method of the last run */
static float last_deblur_k = 0.f;    /* effective fixed steepness (0=adaptive) */
/* v4.9.3: effective (post-cap) steepness statistics */
static double adb_keff_sum = 0.0, adb_keff_w = 0.0;
static float adb_keff_max = 0.f;
static float adb_qconf = 0.f;
static int adb_noamp = 0, adb_noter = 0;
/* adb_nohull / adb_nospeckle declared above upscale_adaptive */
static float deblur_texgain = 0.f; /* -T texgain */
static float adb_srclo[3] = {0.f, 0.f, 0.f}, adb_srchi[3] = {1.f, 1.f, 1.f};
static int autodeblur_is_photo = 0; /* set in upscale_autodeblur from class map */

static void sample_f4(const float *img, int w, int h, float x, float y, float q[4]);
/* autodeblur_analytic_pass -- the "analytical" deblur (method 3).

   A second, deliberately different deblur that does NOT use the per-pixel
   local-window erf fit + trust gates of autodeblur_pass.  It follows the
   analytic prescription literally: PRODUCE a full-image gradient field,
   EDIT it, then SAMPLE from it.

   1. PRODUCE the full-image gradient field.  The autoblur base is loaded into
      a premultiplied-linear RGBA field A; a 4D structure-tensor pass gives
      every pixel its gradient normal (nx,ny) and magnitude MAG (the full-image
      gradient).  For each pixel the colour TRANSITION (the lobe) it sits in is
      then traced ALONG ITS NORMAL across its whole extent -- an adaptive walk
      that keeps going until the colour saturates to a plateau on each side and
      stops there (its length is set by the assumed blur, NOT a fixed 2x2/6x6
      tap, so a wide blurred ramp is traced end to end while a crisp one stops
      after a couple of samples).  The lobe is thus reconstructed as a gradient
      between TWO 4-channel colours P0, P1 (the farthest pair found along the
      trace), and the pixel records its blend coordinate u in [0,1] on the
      P0->P1 segment.

   2. EDIT the gradient field.  Push the start (u=0, P0) and end (u=1, P1) of
      every gradient towards each other: a retention alpha in (0,1] narrows
      each transition by
                      u' = clamp(0.5 + (u - 0.5) / alpha, 0, 1).
      Mid-ramp pixels are driven to the nearer plateau -> the ramp steepens;
      at alpha -> 0 the whole lobe collapses to one point and the colour
      quantizes to P0 or P1.  The remap is symmetric about u=0.5, so it is
      independent of which endpoint is P0 -- only the steepness changes.

   3. SAMPLE from the edited gradient: out = P0 + u' * (P1 - P0), blended in by
      a contrast weight so genuine flats (|P1-P0| ~ 0) are left untouched.
      The output is a convex combination of two REAL sampled colours, so it
      never leaves their hull -- no ringing vocabulary (invariant #1), no hue
      inversion, premultiplied-safe.

   Strength semantics differ from remap/push: here 1 is the MAXIMUM (collapse
   to one point = quantize) and larger values are LESS deblur, so
   alpha = (K-1)/K for K >= 1 (K=1 -> alpha 0 -> quantize; K->inf -> alpha 1 ->
   identity).  K=0 means auto.  (0 is auto because the same -g knob is shared
   with remap/push, where 0 also means auto.) */
static int autodeblur_analytic_pass(uint8_t *out, int dw, int dh, float scale) {
  size_t n = (size_t)dw * dh;
  float *A = malloc(n * 4 * sizeof *A);   /* premultiplied-linear RGBA field */
  float *NX = malloc(n * sizeof *NX);     /* gradient normal x              */
  float *NY = malloc(n * sizeof *NY);     /* gradient normal y              */
  float *MAG = malloc(n * sizeof *MAG);   /* |grad I| (the full-image grad) */
  float *SEG = malloc(n * 8 * sizeof *SEG); /* P0[4], P1[4] per pixel       */
  float *U = malloc(n * sizeof *U);       /* blend coordinate in [0,1]      */
  float *WGT = malloc(n * sizeof *WGT);   /* lobe validity (0=flat/no grad) */
  uint8_t *dst = malloc(n * 4);
  if (!A || !NX || !NY || !MAG || !SEG || !U || !WGT || !dst) {
    free(A);
    free(NX);
    free(NY);
    free(MAG);
    free(SEG);
    free(U);
    free(WGT);
    free(dst);
    return 0;
  }

  /* Load the autoblur base render into a linear premultiplied field. */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      float q[4];
      raw_pm(out, dw, dh, x, y, q);
      size_t k = 4 * ((size_t)y * dw + x);
      A[k] = q[0];
      A[k + 1] = q[1];
      A[k + 2] = q[2];
      A[k + 3] = q[3];
    }

  /* --- PHASE 1a: full-image gradient + structure-tensor normal (one pass) */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      int xl = x > 0 ? x - 1 : 0, xr = x + 1 < dw ? x + 1 : dw - 1,
          yu = y > 0 ? y - 1 : 0, yd = y + 1 < dh ? y + 1 : dh - 1;
      float Jxx = 0.f, Jxy = 0.f, Jyy = 0.f;
      for (int c = 0; c < 4; c++) {
        float gx = A[4 * ((size_t)yu * dw + xr) + c] +
                   2 * A[4 * ((size_t)y * dw + xr) + c] +
                   A[4 * ((size_t)yd * dw + xr) + c] -
                   A[4 * ((size_t)yu * dw + xl) + c] -
                   2 * A[4 * ((size_t)y * dw + xl) + c] -
                   A[4 * ((size_t)yd * dw + xl) + c],
              gy = A[4 * ((size_t)yd * dw + xl) + c] +
                   2 * A[4 * ((size_t)yd * dw + x) + c] +
                   A[4 * ((size_t)yd * dw + xr) + c] -
                   A[4 * ((size_t)yu * dw + xl) + c] -
                   2 * A[4 * ((size_t)yu * dw + x) + c] -
                   A[4 * ((size_t)yu * dw + xr) + c];
        gx *= 1.f / 16.f;
        gy *= 1.f / 16.f;
        Jxx += gx * gx;
        Jxy += gx * gy;
        Jyy += gy * gy;
      }
      float trh = .5f * (Jxx - Jyy), root = sqrtf(trh * trh + Jxy * Jxy),
            lam = .5f * (Jxx + Jyy) + root;
      MAG[idx] = lam; /* gradient magnitude ~= top eigenvalue of the tensor */
      float nx = 1.f, ny = 0.f;
      if (lam > 1e-12f) {
        float vx = Jxy, vy = lam - Jxx;
        if (vx * vx + vy * vy < 1e-18f) {
          vx = lam - Jyy;
          vy = Jxy;
        }
        float vl = vx * vx + vy * vy;
        if (vl > 1e-18f) {
          float inv = 1.f / sqrtf(vl);
          nx = vx * inv;
          ny = vy * inv;
        }
      }
      NX[idx] = nx;
      NY[idx] = ny;
    }

  /* The assumed blur sizes the trace length so it spans a whole lobe end to
     end (global), not a fixed 2x2/6x6 tap.  Capped; early termination keeps
     crisp ramps and flats cheap. */
  float sref0 = adb_assumed_sigma > 0.f ? adb_assumed_sigma : fitted_sigma;
  float sref = sref0 > 1.f ? sref0 : 1.f;
  int maxR = clampi((int)(3.5f * scale * sref + 6.f), 8, 64);
  const float moveThresh = 1.0e-3f; /* per-step plateau detection threshold */
  int dbg = 0, dbg_x = 96, dbg_y = 96;
  {
    const char *e = getenv("CELUP_DBG");
    if (e) {
      dbg = 1;
      sscanf(e, "%d,%d", &dbg_x, &dbg_y);
    }
  }

  /* --- PHASE 1b: trace each pixel's lobe along its normal (global) ---
     Walk +-normal until the colour saturates to a plateau on each side (an
     adaptive, potentially many-pixel walk -- NOT a fixed 2x2/6x6 tap), and
     read the two plateau colours straight off the saturated tails.  The walk
     is global: a wide blurred ramp is traced end to end so even its tail
     pixels see both plateaus and get narrowed, while a crisp ramp stops after
     a couple of samples (early termination).  Every pixel is traced -- the
     early-termination keeps flats cheap, and the lobe's OWN proven contrast
     decides validity (no per-pixel magnitude skip, no 2x2/6x6 confidence). */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      float nx = NX[idx], ny = NY[idx];
      float *P0 = SEG + 8 * idx, *P1 = P0 + 4;
      U[idx] = 0.f;
      WGT[idx] = 0.f;
      float c0[4];
      sample_f4(A, dw, dh, (float)x, (float)y, c0);
      /* Walk each side; the plateau colour is the mean of the saturated tail
         (>= STAB consecutive sub-threshold steps), or the farthest sample
         reached if the lobe is wider than maxR. */
      float plat[2][4];
      for (int s = 0; s < 2; s++) {
        float dir = s ? -1.f : 1.f;
        float prev[4], lastc[4];
        memcpy(prev, c0, sizeof prev);
        memcpy(lastc, c0, sizeof lastc);
        float sum[4] = {0, 0, 0, 0};
        int ns = 0, stab = 0;
        for (int t = 1; t <= maxR; t++) {
          float C[4];
          sample_f4(A, dw, dh, (float)x + dir * (float)t * nx,
                    (float)y + dir * (float)t * ny, C);
          memcpy(lastc, C, sizeof lastc);
          if (dist4_pm(C, prev) > moveThresh) { /* still transitioning */
            stab = 0;
            ns = 0;
            for (int c = 0; c < 4; c++)
              sum[c] = 0.f;
          } else { /* saturated: accumulate the plateau tail */
            stab++;
            for (int c = 0; c < 4; c++)
              sum[c] += C[c];
            ns++;
          }
          memcpy(prev, C, sizeof prev);
          if (stab >= 3)
            break; /* plateau confirmed on this side */
        }
        if (ns >= 2)
          for (int c = 0; c < 4; c++)
            plat[s][c] = sum[c] / (float)ns;
        else
          memcpy(plat[s], lastc, sizeof plat[s]);
      }
      for (int c = 0; c < 4; c++) {
        P0[c] = plat[0][c];
        P1[c] = plat[1][c];
      }
      float L2 = dist4_pm(P0, P1); /* |P1 - P0|^2 */
      if (L2 < 6.4e-5f) { /* no real transition -> inert (flat) */
        for (int c = 0; c < 4; c++)
          P0[c] = P1[c] = A[4 * idx + c];
        continue;
      }
      /* Blend coordinate of THIS pixel on the P0->P1 segment. */
      float dot = 0.f;
      for (int c = 0; c < 4; c++)
        dot += (c0[c] - P0[c]) * (P1[c] - P0[c]);
      U[idx] = clampf(dot / L2, 0.f, 1.f);
      /* Validity: the lobe's own proven contrast (no 2x2/6x6 confidence). */
      WGT[idx] = ramp01(L2, 4e-4f, 3e-2f);
      if (dbg && y == dbg_y && abs(x - dbg_x) <= 16 && (x & 3) == 0)
        fprintf(stderr,
                "DBGA %d,%d maxR=%d L2=%.5f u=%.3f wgt=%.3f P0=%.3f P1=%.3f "
                "mag=%.5f\n",
                x, y, maxR, L2, U[idx], WGT[idx], P0[0], P1[0], MAG[idx]);
    }

  /* --- PHASE 2 + 3: edit (narrow) + sample ---
     K semantics: 0 = auto, 1 = max (collapse to a point = quantize), larger =
     less deblur.  alpha = (K-1)/K is the transition retention. */
  float K;
  if (deblur_steepness > 0.f)
    K = deblur_steepness;
  else
    K = clampf(2.2f - 0.012f * (compress_strength - 1.f), 1.05f, 2.5f);
  last_deblur_k = K;
  float alpha = K <= 1.0001f ? 0.f : (K - 1.f) / K;
  fprintf(stderr,
          "autodeblur analytic K=%.3f (1=max/quantize, higher=less) "
          "alpha=%.3f maxR=%d\n",
          K, alpha, maxR);
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      const float *P0 = SEG + 8 * idx;
      const float *P1 = P0 + 4;
      float o[4];
      for (int c = 0; c < 4; c++)
        o[c] = A[4 * idx + c];
      float w = WGT[idx];
      if (w <= 1e-4f) { /* flat / no proven gradient -> keep base colour */
        put(dst + 4 * idx, o[0], o[1], o[2], o[3]);
        continue;
      }
      float u = U[idx], up;
      if (alpha <= 1e-4f) /* K = 1: collapse the gradient to one point */
        up = u < 0.5f ? 0.f : (u > 0.5f ? 1.f : 0.5f);
      else
        up = clampf(0.5f + (u - 0.5f) / alpha, 0.f, 1.f);
      float v[4];
      for (int c = 0; c < 4; c++)
        v[c] = o[c] + w * (P0[c] + up * (P1[c] - P0[c]) - o[c]);
      put(dst + 4 * idx, v[0], v[1], v[2], v[3]);
    }
  memcpy(out, dst, n * 4);
  free(dst);
  free(WGT);
  free(U);
  free(SEG);
  free(MAG);
  free(NY);
  free(NX);
  free(A);
  return 1;
}

static int upscale_autodeblur(const uint8_t *in, int sw, int sh, uint8_t *out,
                              int dw, int dh);
/* Standard-normal CDF (libm erff). */
static float phi1(float z) { return .5f * (1.f + erff(z * 0.70710678f)); }

/* v4.9.8: inverse normal CDF Phi^-1(p), p in (0,1) -- Acklam's
   rational approximation (max abs error ~3e-9 in z, far below the
   1/255 output quantum).  Used by the erf-gain post-map. */
static float probit01(float p) {
  static const float a[6] = {-3.969683028665376e+01f,
                             2.209460984245205e+02f,
                             -2.759285104469687e+02f,
                             1.383577518672690e+02f,
                             -3.066479806614716e+01f,
                             2.506628277459239e+00f};
  static const float b[5] = {-5.447609879822406e+01f,
                             1.615858368580409e+02f,
                             -1.556989798598866e+02f,
                             6.680131188771972e+01f,
                             -1.328068155288572e+01f};
  static const float c[6] = {-7.784894002430293e-03f,
                             -3.223964580411365e-01f,
                             -2.400758277161838e+00f,
                             -2.549732539343734e+00f,
                             4.374664141464968e+00f,
                             2.938163982698783e+00f};
  static const float d[4] = {7.784695709041462e-03f,
                             3.224671290700398e-01f,
                             2.445134137142996e+00f,
                             3.754408661907416e+00f};
  const float plow = .02425f, phigh = 1.f - .02425f;
  if (p <= 1e-6f)
    return -4.7534f; /* phi1(-4.7534) ~ 1e-6 */
  if (p >= 1.f - 1e-6f)
    return 4.7534f;
  if (p < plow) {
    float q = sqrtf(-2.f * logf(p));
    return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) *
                q +
            c[5]) /
           ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.f);
  }
  if (p <= phigh) {
    float q = p - .5f, r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) *
                r +
            a[5]) *
           q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) *
                r +
            1.f);
  }
  {
    float q = sqrtf(-2.f * logf(1.f - p));
    return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) *
                 q +
             c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.f);
  }
}
/* v6: trust gate further widened .04/.30 (was .03/.10 then .04/.22);
   narrow gate zeroed many wide-blur fits (r=6) -> parameter ignoring.
   Wide blur needs higher hi to keep fits. */
static float trust_lo = .03f, trust_hi = .10f; /* debug override: CDG=lo,hi */

/* v4.9.6 dip/line-profile helper: the box(h)-gauss(sig) dip shape,
   normalized to 1 at the centre (p = |position - line centre|). */
static float dipnorm(float p, float h, float sig) {
  if (p < 0.f)
    p = -p;
  if (h < 0.f)
    h = 0.f;
  if (sig < .05f)
    sig = .05f;
  float den = 2.f * phi1(h / sig) - 1.f;
  if (den < 1e-3f) { /* h << sig: gaussian-limit bump */
    float z = p / sig;
    return expf(-.5f * z * z);
  }
  return (phi1((p + h) / sig) - phi1((p - h) / sig)) / den;
}

#define ZSTAR_N 1024
/* Half-width calibration: the |du|-weighted flank centroids of a
   blurred line sit at the half-depth points of the box-gauss dip,
   z*(r) sigma from centre, with r = true half-width / sigma.  For
   r << 1 z* -> 1.177 (gaussian half-max); for r >> 1 z* -> r.  LUT
   maps the observed half-span q (= z*) back to r. */
#define ZSTAR_N 1024
static float zstar_r[ZSTAR_N], zstar_z[ZSTAR_N];
static int zstar_ready = 0;
static void zstar_init(void) {
  if (zstar_ready)
    return;
  for (int i = 0; i < ZSTAR_N; i++) {
    float r = .02f * powf(400.f, (float)i / (float)(ZSTAR_N - 1));
    float target = phi1(r) - .5f; /* dip(0)/2 */
    float lo = 0.f, hi = r + 5.f;
    if (target < 1e-4f) {
      zstar_z[i] = 1.17741f;
    } else {
      for (int it = 0; it < 60; it++) {
        float z = .5f * (lo + hi);
        float du = phi1(z + r) - phi1(z - r);
        if (du > target)
          lo = z;
        else
          hi = z;
      }
      zstar_z[i] = .5f * (lo + hi);
    }
    zstar_r[i] = r;
  }
  zstar_ready = 1;
}
static float r_from_zstar(float q) {
  zstar_init();
  if (q <= zstar_z[0])
    return zstar_r[0];
  int lo = 0, hi = ZSTAR_N - 1;
  while (hi - lo > 1) {
    int mid = (lo + hi) / 2;
    if (zstar_z[mid] <= q)
      lo = mid;
    else
      hi = mid;
  }
  float t = (q - zstar_z[lo]) / (zstar_z[hi] - zstar_z[lo] + 1e-9f);
  return zstar_r[lo] + clampf(t, 0.f, 1.f) * (zstar_r[hi] - zstar_r[lo]);
}
static float ss01(float z) {
  z = clampf(z, 0.f, 1.f);
  return z * z * (3.f - 2.f * z);
}
/* 3-parameter weighted profile fit on a projection line:
   y = *A + *C*z + *B*phi(z), z = (j - R - mu)/s, weights wj over
   [dl..dr].  Linear baseline = shading content; phi = the step to
   deblur.  Degenerate pivot falls back to the 2-param amplitude fit. */
static void lsq_profile(const float *raw, const float *wj, int dl, int dr,
                        double mu, float s, int R, float *A, float *C,
                        float *B) {
  double sw = 0, sz = 0, szz = 0, sp = 0, spp = 0, szp = 0, sy = 0,
         szy = 0, spy = 0;
  float a = 0.f, b = 1.f, c = 0.f;
  for (int j = dl; j <= dr; j++) {
    if (wj[j] <= 1e-9f)
      continue;
    double z = (j - R - mu) / s, p = phi1((float)z), yv = raw[j],
           w2 = wj[j];
    sw += w2;
    sz += w2 * z;
    szz += w2 * z * z;
    sp += w2 * p;
    spp += w2 * p * p;
    szp += w2 * z * p;
    sy += w2 * yv;
    szy += w2 * z * yv;
    spy += w2 * p * yv;
  }
  double m00 = sw, m01 = sz, m02 = sp, m11 = szz, m12 = szp, m22 = spp,
         c00 = m11 * m22 - m12 * m12, c01 = m02 * m12 - m01 * m22,
         c02 = m01 * m12 - m02 * m11, c11 = m00 * m22 - m02 * m02,
         c12 = m01 * m02 - m00 * m12, c22 = m00 * m11 - m01 * m01,
         det = m00 * c00 + m01 * c01 + m02 * c02;
  if (fabs(det) > 1e-9 * sw * sw * sw && sw > 1e-9) {
    a = (float)((sy * c00 + szy * c01 + spy * c02) / det);
    c = clampf((float)((sy * c01 + szy * c11 + spy * c12) / det), -.3f,
               .3f);
    b = clampf((float)((sy * c02 + szy * c12 + spy * c22) / det), .25f,
               4.f);
    a = clampf(a, -1.f, 2.f);
  } else if (sw * m22 - sp * sp > 1e-12 && sw > 1e-9) {
    double d2p = sw * m22 - sp * sp;
    a = clampf((float)((sy * m22 - sp * spy) / d2p), -1.f, 2.f);
    b = clampf((float)((sw * spy - sp * sy) / d2p), .25f, 4.f);
  }
  *A = a;
  *C = c;
  *B = b;
}

/* autodeblur core pass (v4.8: ANCHORED single-lobe profile steepening).

   The v4.3..v4.6 pass steepened each channel independently toward
   per-channel box-window extremes (hue inversion, boxy halos);
   v4.7 introduced the 4D single-gradient sampling and the analytic
   erf/pulse fit, but evaluated the steepened curve by inverting the
   pixel's COLOUR: nu = phi(k * phi^-1(u_px)).  Review forensics:
   - d(nu)/du = k at the ramp centre, so any pixel sitting epsilon off
     the fitted curve rendered k*epsilon off -> outstanding pixels at
     gradient centres;
   - near plateaus phi^-1 explodes, turning flat noise into a colour
     halo that starts at a line's centre and dies near its edge;
   - one window-wide fit spanning a thin line mixed BOTH flanks and
     BOTH backgrounds into one phantom step centred mid-line ("2
     gradients surrounding the edge combined instead of not going
     further than each other") -> snake-tongue forks at line caps;
   - every output was snapped onto the fit's 1D colour segment,
     discarding the orthogonal colour component -> "watered away
     colors".

   v4.8 keeps the v4.7 sampling (ONE gradient direction per pixel for
   the whole premultiplied RGBA vector; tangentially averaged line
   samples) but changes fit domain and evaluation:
   - LOBE MAP: |du| along the normal is segmented into transition
     lobes.  Each pixel is assigned its nearest lobe; plateau colours,
     centre mu and width s come from THAT lobe alone with one-sided
     margins clipped at neighbouring lobes -- two flanks and two
     backgrounds never enter one fit.
   - ANCHORED EVALUATION: the steepened fit is evaluated at the
     pixel's GEOMETRIC position on the normal (t = 0), and the pixel's
     own residual to the unsteepened fit is re-added with GAIN 1:
     out = F_k(0) + (o - F(0)).  On-curve pixels steepen exactly by k;
     off-curve deviations (texture, hue arcs, dither, alpha) pass
     through unamplified; line interiors and plateaus evaluate to
     plateau + residual = identity, so misassignment is never an
     artifact.  Consequence: float steepness is now safe far beyond 8
     (-g accepts 1..64), still capped so output ramps keep >= .6 px
     sigma (no re-aliased sawtooth).
   - MULTI-CROSSING trust replaces the beta2 gate: per-lobe fits are
     all locally good inside dense texture, so suppression counts
     hysteresis mid-level crossings over the whole window (step = 1,
     one line = 2: full trust; 4+: crosshatch/hair-clump/text fades to
     zero), beside the erf-RMSE-over-domain trust gate.
   -D method 1 (remap): evaluate the slope-steepened fit at t = 0.
   -D method 2 (push):  evaluate the ORIGINAL fit at a position
     displaced toward the nearer plateau by (ufit-.5)(k-1)*s*1.5/s --
     the Anime4K heightmap push in analytic form.

   v4.9 review forensics (smiley test: rounded corners + smooth "neon"
   glow around lines):
   - CORNER ROUNDING: both tangential mechanisms (line-sample averaging
     and pass-2 delta smoothing) assume the contour is translation-
     invariant along its tangent.  At corners/tips that premise fails:
     taps run AROUND the corner, the radial fit is diluted and the
     sharpened edge is dissolved into an arc.  Fix: junction measure
     rho = lambda2/lambda1 from the SAME structure tensor scales the
     tangent span and the pass-2 tap weights -- a corner keeps its own
     radial fit and stays sharp; straight contours are untouched.
   - NEON GLOW (v4.9.1 redo): v4.9 "fixed" the skirt by rendering the
     base at sigma r/min(K,8); the user rejected that instantly: the
     crisp base brought the lattice staircase (-r 6 exists exactly to
     blur it away), per-tread speckle and forked caps back.  The real
     mechanism of the dark pre-edge band: the clamped projection kills
     ramp tails whenever the window is narrower than the blur, and a
     pure erf cannot represent step-on-linear-shading, so the fit
     dragged whole shading gradients to a flat plateau colour.  v4.9.1
     keeps the base at sigma R and fixes the FIT: raw (unclamped)
     projection restores the tails, a linear+erf LS keeps shading in
     the gain-1 residual, the drag amplitude is the locally proven
     one-sided plateau span (robust against LS phi/linear degeneracy
     BOTH ways), and a step-evidence coverage gate mutes fits where
     the window does not contain the modelled transition.
   - CONTOUR CONSENSUS: raw per-pixel fits misplaced mu by 1-2 out px
     near wedge apexes (cornerstar48); anchored evaluation amplified
     that ~k into +-0.25 colour deltas with alternating sign, and
     delta transport pushed pixels outside the locally observed
     colours.  Fix: pass 1.5 integrates the wS-weighted fit parameters
     along the (junction-aware) tangent and re-derives z/k/nu from the
     consensus; plus a convex-hull output clamp (deblur has no ringing
     vocabulary), quantized in DISPLAY space. */
/* Bilinear sampler for float4 fields (used for the tangential
   anti-jitter smoothing of the model delta below). */
static void sample_f4(const float *img, int w, int h, float x, float y,
                      float q[4]) {
  int ix = (int)floorf(x), iy = (int)floorf(y);
  float fx = x - (float)ix, fy = y - (float)iy;
  q[0] = q[1] = q[2] = q[3] = 0.f;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      float ww = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
      const float *p =
          img + 4 * ((size_t)clampi(iy + j, 0, h - 1) * w +
                     clampi(ix + i, 0, w - 1));
      for (int c = 0; c < 4; c++)
        q[c] += ww * p[c];
    }
}

/* Bilinear sampler for interleaved nc-channel float fields (the v4.9
   fit-parameter field). */
static void sample_fn(const float *img, int w, int h, int nc, float x,
                      float y, float *q) {
  int ix = (int)floorf(x), iy = (int)floorf(y);
  float fx = x - (float)ix, fy = y - (float)iy;
  for (int c = 0; c < nc; c++)
    q[c] = 0.f;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      float ww = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
      const float *p =
          img + (size_t)nc * ((size_t)clampi(iy + j, 0, h - 1) * w +
                              clampi(ix + i, 0, w - 1));
      for (int c = 0; c < nc; c++)
        q[c] += ww * p[c];
    }
}
static int autodeblur_pass(uint8_t *out, int dw, int dh, float scale,
                           int method) {
  size_t n = (size_t)dw * dh;
  float *A = malloc(n * 4 * sizeof *A);
  float *DEL = malloc(n * 4 * sizeof *DEL);  /* flat + model colour delta */
  /* v4.9 fit-parameter field, wS-weighted: [wS, wS*mu, wS*s, wS*d2x4,
     coh, tanx, tany, wS*off0x4, wS*off1x4] (18 floats/px; off0/off1 are
     the v4.9.3 narrow-feature plateau-restoration deltas).  Pass 1.5
     integrates it ALONG THE CONTOUR so each pixel is rendered from a
     contour-consensus fit instead of its own jittering 1D fit
     (near-apex windows misplace mu by 1-2 out px and raw per-pixel
     deltas swung +-0.25 with alternating sign). */
  float *PF = malloc(n * 25 * sizeof *PF);
  uint8_t *LOH = malloc(n * 8);              /* local hull, u8/chan    */
  uint8_t *dst = malloc(n * 4);
  float *ZM = calloc(n, sizeof *ZM); /* v4.9.8 erf-gain map weight/px */
  if (!A || !DEL || !PF || !LOH || !dst || !ZM) {
    free(A);
    free(DEL);
    free(PF);
    free(LOH);
    free(dst);
    free(ZM);
    return 0;
  }
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      float q[4];
      raw_pm(out, dw, dh, x, y, q);
      for (int c = 0; c < 4; c++)
        A[4 * ((size_t)y * dw + x) + c] = q[c];
    }
  {
    const char *cdg = getenv("CDG");
    if (cdg)
      sscanf(cdg, "%f,%f", &trust_lo, &trust_hi);
    /* forensics isolation knobs: CELUP_NOAMP=1 disables narrow-
       feature amplitude restoration, CELUP_NOTER=1 disables the
       terrace cleanup, CELUP_NOHULL=1 disables the hull clamp,
       CELUP_NOSPECKLE=1 disables adaptive speckle suppression. */
    if (getenv("CELUP_NOAMP"))
      adb_noamp = 1;
    if (getenv("CELUP_NOTER"))
      adb_noter = 1;
    if (getenv("CELUP_NOHULL"))
      adb_nohull = 1;
    if (getenv("CELUP_NOSPECKLE"))
      adb_nospeckle = 1;
  }
  /* sref = ASSUMED source blur (window sizing, shading gate).  When the
     reconstruction sigma was decoupled (v4.9) the assumed value is kept
     separately: fitted_sigma is then the (smaller) base-render sigma. */
  float sref0 = adb_assumed_sigma > 0.f ? adb_assumed_sigma : fitted_sigma;
  float sref = sref0 > 1.f ? sref0 : 1.f;
  int wide = scale * sref > 4.001f;
  int R = wide ? clampi((int)(1.25f * scale * sref + .5f), 2, 64)
               : clampi((int)(1.25f * scale + .5f), 2, 12);
  int NS = 2 * R + 1; /* R <= 64 -> NS <= 129 */
  float kbase = deblur_steepness > 0.f
                    ? deblur_steepness
                    : clampf(1.f + .25f * (compress_strength - 1.f), 1.f, 3.f);
  last_deblur_k = deblur_steepness > 0.f   ? deblur_steepness
                  : edge_goal > 0.f        ? 0.f
                                           : kbase;
  adb_keff_sum = adb_keff_w = 0.0;
  adb_keff_max = 0.f;
  /* Shading close-gate on the fitted ramp sigma s (output px):
     transitions far wider than a fitted-blur ramp are content softness.
     Smooth LINEAR shading is inert regardless -- its fitted mu is ~0
     and u_px ~.5, so the fit reproduces it unchanged. */
  float sa = 1.3f * scale * sref, sb = 2.4f * scale * sref;
  float flatmix = .25f; /* diffusion-noise flattening in gate-zero zones */
  int T = clampi((int)(scale * .75f + .5f), 1, 3); /* tangent span (v4.7) */
  /* per-fit diagnostic dump: CELUP_DBG=x,y prints the fit internals for
     pixels near (x,y) (step 4 px) */
  int dbg = 0, dbg_x = 96, dbg_y = 96;
  {
    const char *e = getenv("CELUP_DBG");
    if (e) {
      dbg = 1;
      sscanf(e, "%d,%d", &dbg_x, &dbg_y);
    }
  }
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      float o[4];
      for (int c = 0; c < 4; c++)
        o[c] = A[4 * idx + c];
      /* 4D structure tensor from per-channel Sobel taps on A. */
      int xl = x > 0 ? x - 1 : 0, xr = x + 1 < dw ? x + 1 : dw - 1,
          yu = y > 0 ? y - 1 : 0, yd = y + 1 < dh ? y + 1 : dh - 1;
      float Jxx = 0.f, Jxy = 0.f, Jyy = 0.f;
      for (int c = 0; c < 4; c++) {
        float gx = A[4 * ((size_t)yu * dw + xr) + c] +
                   2 * A[4 * ((size_t)y * dw + xr) + c] +
                   A[4 * ((size_t)yd * dw + xr) + c] -
                   A[4 * ((size_t)yu * dw + xl) + c] -
                   2 * A[4 * ((size_t)y * dw + xl) + c] -
                   A[4 * ((size_t)yd * dw + xl) + c],
              gy = A[4 * ((size_t)yd * dw + xl) + c] +
                   2 * A[4 * ((size_t)yd * dw + x) + c] +
                   A[4 * ((size_t)yd * dw + xr) + c] -
                   A[4 * ((size_t)yu * dw + xl) + c] -
                   2 * A[4 * ((size_t)yu * dw + x) + c] -
                   A[4 * ((size_t)yu * dw + xr) + c];
        gx *= 1.f / 16.f;
        gy *= 1.f / 16.f;
        Jxx += gx * gx;
        Jxy += gx * gy;
        Jyy += gy * gy;
      }
      float trh = .5f * (Jxx - Jyy),
            root = sqrtf(trh * trh + Jxy * Jxy),
            lam = .5f * (Jxx + Jyy) + root;
      /* v4.9 junction measure rho = lambda2/lambda1: ~0 on a straight
         contour (translation-invariant ALONG it -- the premise of both
         tangential line-sample averaging and pass-2 delta smoothing),
         ->1 at corners/junctions where that premise fails.  coh scales
         the tangent span and the pass-2 tap weights, so a corner keeps
         its own radial fit (sharp tip) instead of being dissolved into
         an arc by taps running around it; straight edges keep the full
         anti-wobble averaging unchanged. */
      float lam2 = .5f * (Jxx + Jyy) - root;
      float rho = lam > 1e-12f ? lam2 / lam : 0.f;
      /* Gate band tuned so long blurred arcs (rings/corner torture,
         face contours) keep full tangential averaging and only genuine
         junctions/tips (rho >= ~.3 on a 3x3 tensor) lose it. */
      float coh = 1.f - ss01((rho - .10f) * (1.f / .20f));
      float dirx = 1.f, diry = 0.f;
      if (lam > 1e-12f) {
        float vx = Jxy, vy = lam - Jxx;
        if (vx * vx + vy * vy < 1e-18f) {
          vx = lam - Jyy;
          vy = Jxy;
        }
        float vl = vx * vx + vy * vy;
        if (vl > 1e-18f) {
          float inv = 1.f / sqrtf(vl);
          dirx = vx * inv;
          diry = vy * inv;
        }
      }
      /* Line samples along the normal, TANGENTIALLY AVERAGED over a
         small neighborhood perpendicular to it (v4.7 corner-scallop
         fix): steepening an edge whose source staircase survived the
         fit would otherwise amplify the source lattice into visible
         S-wobbles along the contour.  Genuine edges are translation-
         invariant along their contour, so averaging a few offset
         normals keeps the profile while erasing the grid-periodic
         jitter (period ~1 src px). */
      float tanx = -diry, tany = dirx;
      /* Coherence-scaled tangent span (v4.9): full T on straight
         contours, 0 at junctions/corners/tips. */
      int Teff = (int)((float)T * coh + .5f);
      float C[129][4], u[129], raw[129],
            lo[4] = {1e30f, 1e30f, 1e30f, 1e30f},
            hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f},
            mean[4] = {0, 0, 0, 0};
      for (int j = 0; j < NS; j++) {
        float t = (float)(j - R), acc[4] = {0, 0, 0, 0};
        for (int to = -Teff; to <= Teff; to++) {
          float q[4];
          sample_pm(out, dw, dh, (float)x + t * dirx + (float)to * tanx,
                    (float)y + t * diry + (float)to * tany, q);
          for (int c = 0; c < 4; c++)
            acc[c] += q[c];
        }
        float inv = 1.f / (2 * Teff + 1);
        for (int c = 0; c < 4; c++) {
          C[j][c] = acc[c] * inv;
          mean[c] += C[j][c];
          if (C[j][c] < lo[c])
            lo[c] = C[j][c];
          if (C[j][c] > hi[c])
            hi[c] = C[j][c];
        }
      }
      float rng = 0.f;
      for (int c = 0; c < 3; c++)
        if (hi[c] - lo[c] > rng)
          rng = hi[c] - lo[c];
      for (int c = 0; c < 4; c++)
        mean[c] *= 1.f / NS;
      /* Endpoint segment (both endpoints are REAL local colours). */
      int mE = R / 3;
      if (mE < 1)
        mE = 1;
      float CA[4] = {0, 0, 0, 0}, CB[4] = {0, 0, 0, 0}, d[4], L2;
      for (int j = 0; j < mE; j++)
        for (int c = 0; c < 4; c++) {
          CA[c] += C[j][c];
          CB[c] += C[NS - 1 - j][c];
        }
      L2 = 0.f;
      for (int c = 0; c < 4; c++) {
        CA[c] /= mE;
        CB[c] /= mE;
        d[c] = CB[c] - CA[c];
        L2 += d[c] * d[c];
      }
      /* v4.9: the pixel-level fit parameters (consensus-evaluated in
         pass 1.5); zero when no fit was made.  offv0/offv1: v4.9.3
         narrow-feature plateau restoration (channel space). */
      float wS = 0.f, wS2 = 0.f, frel = 0.f, fmu = 0.f, fs = 0.f,
            fd2[4] = {0, 0, 0, 0}, offv0[4] = {0, 0, 0, 0},
            offv1[4] = {0, 0, 0, 0}, Pc0[4] = {0, 0, 0, 0},
            Pc1[4] = {0, 0, 0, 0};
      /* v4.9.6 dip/line-class claim (pass 1.5 renders the feature
         from the shared dip model instead of the fat washed shoulder
         mid-contour): lW = fire flag, lVX/lVY = image-space vector
         from the pixel to the line centre (frame-free, so the
         consensus may average it), lDw = observed dip depth at the
         centre against this window's own fit, lH = calibrated true
         half-width in out px. */
      float lW = 0.f, lVX = 0.f, lVY = 0.f, lDw = 0.f, lH = 0.f;
      int uflip = 0;
      if (L2 >= 6.4e-5f) { /* |d| >= .008: not a flat */
        for (int j = 0; j < NS; j++) {
          float uu = 0.f;
          for (int c = 0; c < 4; c++)
            uu += (C[j][c] - CA[c]) * d[c];
          raw[j] = uu / L2; /* unclamped: ramp TAILS live here         */
          u[j] = clampf(raw[j], 0.f, 1.f);
        }
        float corr = 0.f;
        for (int j = 0; j < NS; j++)
          corr += (float)(j - R) * u[j];
        if (corr < 0.f) { /* orient u increasing along +t */
          uflip = 1;
          for (int j = 0; j < NS / 2; j++) {
            for (int c = 0; c < 4; c++) {
              float tmp = C[j][c];
              C[j][c] = C[NS - 1 - j][c];
              C[NS - 1 - j][c] = tmp;
            }
            float tu = u[j];
            u[j] = u[NS - 1 - j];
            u[NS - 1 - j] = tu;
          }
          float swp[4];
          memcpy(swp, CA, sizeof swp);
          memcpy(CA, CB, sizeof swp);
          memcpy(CB, swp, sizeof swp);
          for (int c = 0; c < 4; c++)
            d[c] = -d[c];
        }
        float ul = 0.f, ur = 0.f;
        for (int j = 0; j < mE; j++) {
          ul += u[j];
          ur += u[NS - 1 - j];
        }
        ul /= mE;
        ur /= mE;
        {
          /* -------- v4.8: unified single-lobe erf profile fit ------ */
          /* Lobe map: |du| along the normal is segmented into
             transition lobes (runs above a relative floor, single-
             sample dips bridged).  The pixel is assigned its NEAREST
             lobe; the erf profile is fit on that lobe only, with
             plateau colours taken one-sided from just outside it and
             the domain clipped at neighbouring lobes -- a thin line's
             two flanks and its two backgrounds ("2 gradients
             surrounding edge") never combine into one phantom step,
             and shading slopes on the plateaus never dilute the
             moments.  Misassignment degrades to identity: far from the
             lobe centre both nu and ufit saturate, so the output is
             plateau + residual = the original pixel. */
          float wj[129], wmax = 0.f, th;
          int jl[16], jr[16], NL = 0;
          wj[0] = wj[NS - 1] = 0.f;
          for (int j = 1; j < NS - 1; j++) {
            wj[j] = fabsf(raw[j + 1] - raw[j - 1]);
            if (wj[j] > wmax)
              wmax = wj[j];
          }
          th = wmax * .06f;
          if (th < .004f)
            th = .004f;
          for (int j = 1; j < NS - 2 && NL < 16;) {
            if (wj[j] < th) {
              j++;
              continue;
            }
            int a = j;
            /* Run, bridging single-sample dips -- but a SATURATED flat
               plateau (u pinned at 0/1 while |du| dies) is a hard
               break: it is the interior of a thin line/tip, and the
               raw (unclamped) wj keeps sloping there so without this
               the two flanks merge into one phantom step exactly like
               the pre-lobe-map days.  A saturated but still-sloping
               ramp (neck skirt: u pinned, raw still falling) does NOT
               break -- that tail belongs to the lobe.
               v4.9.3: an UNSATURATED extremum is a hard break too --
               where the raw slope reverses with |du| >= th on BOTH
               sides (a smooth dip/hill bottom, e.g. a line core
               widened past its plateau by a large -r): a single erf
               cannot fit rise-then-fall, the rmse trust gate read the
               victim windows as misfits (.2-.8) and zeroed wS there,
               which was the -r 6 washout of thin dark lines (the
               smiley's mouth/eye lines faded to half contrast and
               steepness knobs went dead on exactly those pixels).
               Noise reversals never satisfy both-sides-above-th. */
            while (j + 1 < NS - 1 &&
                   (wj[j + 1] >= th || (j + 2 < NS - 1 && wj[j + 2] >= th)) &&
                   !((u[j + 1] <= .002f || u[j + 1] >= .998f) &&
                     wj[j + 1] < th) &&
                   !(j + 2 < NS - 1 &&
                     (raw[j + 2] - raw[j + 1]) * (raw[j] - raw[j - 1]) < 0.f &&
                     fabsf(raw[j + 2] - raw[j + 1]) >= .10f * wmax &&
                     fabsf(raw[j] - raw[j - 1]) >= .10f * wmax))
              j++;
            jl[NL] = a;
            jr[NL] = j;
            NL++;
            j++;
          }
          if (dbg && y == dbg_y && abs(x - dbg_x) <= 8 && (x & 1) == 0) {
            fprintf(stderr, "DBGP %d,%d NL=%d th=%.4f wmax=%.4f prof:",
                    x, y, NL, th, wmax);
            for (int j = R - 6; j <= R + 6; j++)
              fprintf(stderr, " %d:r%.3f/w%.3f/u%.2f", j - R, raw[j],
                      wj[j], u[j]);
            fprintf(stderr, "\n");
          }
          if (NL > 0) {
            /* v4.9.3: per-lobe |du| centroids for ALL lobes (the dip/
               hill geometry measures need neighbouring flank
               positions). */
            float mus[16];
            for (int q = 0; q < NL; q++) {
              double Wq = 0., Mq = 0.;
              for (int j = jl[q]; j <= jr[q]; j++) {
                Wq += wj[j];
                Mq += wj[j] * (j - R);
              }
              mus[q] = Wq > 1e-9 ? (float)(Mq / Wq) : 0.f;
            }
            int li = 0, lbest = INT_MAX;
            for (int q = 0; q < NL; q++) {
              int dist = R < jl[q] ? jl[q] - R : R > jr[q] ? R - jr[q] : 0;
              if (dist < lbest) {
                lbest = dist;
                li = q;
              }
            }
            /* Fit domain: lobe + one-sided plateau margins, clipped at
               neighbouring lobes and window ends. */
            int dl = jl[li] - mE, dr = jr[li] + mE;
            if (li > 0 && dl <= jr[li - 1])
              dl = jr[li - 1] + 1;
            if (li + 1 < NL && dr >= jl[li + 1])
              dr = jl[li + 1] - 1;
            if (dl < 0)
              dl = 0;
            if (dr > NS - 1)
              dr = NS - 1;
            float P0[4], P1[4], d2[4], Ld2 = 0.f;
            int n0 = 0, n1 = 0;
            for (int c = 0; c < 4; c++) {
              P0[c] = 0.f;
              P1[c] = 0.f;
            }
            for (int j = dl; j <= dr; j++)
              if (u[j] < .25f) {
                n0++;
                for (int c = 0; c < 4; c++)
                  P0[c] += C[j][c];
              } else if (u[j] > .75f) {
                n1++;
                for (int c = 0; c < 4; c++)
                  P1[c] += C[j][c];
              }
            for (int c = 0; c < 4; c++) {
              P0[c] = n0 ? P0[c] / n0 : C[jl[li]][c];
              P1[c] = n1 ? P1[c] / n1 : C[jr[li]][c];
              d2[c] = P1[c] - P0[c];
              Ld2 += d2[c] * d2[c];
            }
            /* Full-lobe moments (v4.8): |du| centroid and second moment
               over the whole lobe.  Windowed-moment refinements were
               tried and rejected: a core-trimmed centroid halves the
               tail lever arm but starves the width (sigma 6 reads as
               2.5 -- the fit-trust gate then rejects every clean wide
               ramp), and the Gaussian truncation inversion is exact
               only for perfect Gaussians and flaps the gate per pixel.
               The residual jitter of plain moments shows up only as
               +/-1 LSB random dither on noisy synthetic ramps and is
               suppressed where it is coherent: pass-2 tangential
               smoothing. */
            double W = 0, mu = 0, sq = 0;
            for (int j = jl[li]; j <= jr[li]; j++) {
              W += wj[j];
              mu += wj[j] * (j - R);
            }
            if (Ld2 > 6.4e-5f && W > 1e-9) {
              mu /= W;
              for (int j = jl[li]; j <= jr[li]; j++) {
                double t = j - R - mu;
                sq += wj[j] * t * t;
              }
              float s = (float)sqrt(sq / W), lwm = .5f * (jr[li] - jl[li] + 1);
              if (s < .3f)
                s = .3f;
              if (s > lwm)
                s = lwm;
              fmu = (float)mu;
              fs = s;
              /* Profile fit (v4.9.1): linear baseline + erf step on the
                 RAW projection, y = a + c*z + b*phi(z).  The clamped u
                 destroys ramp TAILS whenever the window is narrower
                 than the blur (-r 6), and an erf alone cannot represent
                 step-on-linear-shading: the explicit linear term takes
                 the baked-in shading so phi only models the step.  The
                 baseline cancels in v = o + b*(phi(kz)-phi(z))*d, so
                 shading passes through the residual channel untouched.
                 (A two-round centroid refinement on the step component
                 was tried and REMOVED: re-centring mu on y-raw-c*z at
                 tips/corners moved the fit grid ~.3 px and rounded
                 them, and the plain single LS already produces a
                 healthy b once the raw projection restores the ramp
                 tails to the lobe map.) */
              float ab_a, ab_b, ab_c;
              lsq_profile(raw, wj, dl, dr, mu, s, R, &ab_a, &ab_c,
                          &ab_b);
              /* Drag amplitude = the one-sided PLATEAU SPAN (v4.8
                 estimator, robust, shading-free), not the LS b.  On a
                 soft wide ramp (decouple-reverted base) the 3-param LS
                 systematically shifts contrast between phi and the
                 near-parallel linear term, so ab_b reads low (~.7*sp)
                 at tips and corners -> the render under-drags and
                 rounds them; and on shaded skirts (-r 6) it can spike
                 ABOVE the span -> flat-top dark "neon" band.  sp is
                 the contrast the window itself PROVES between its two
                 plateaus -- exactly the v4.8/v4.9 fd2 = d2 semantics.
                 Bounds: nothing when b degenerates, 1.2x span when the
                 fit spikes. */
              /* Steepness, decided BEFORE the amplitude restoration
                 (v4.9.4): the mass-correct restore depth needs the k
                 the contour will actually be rendered with.  Same
                 rules as the former inline block: -g pins k exactly
                 (float, up to 64); -e adapts per edge; -s formula
                 otherwise; always capped so the OUTPUT ramp never
                 falls below .6 px sigma (~1.5 px 30% width): no
                 re-aliased sawtooth, and already-crisp content is
                 left alone (k -> 1).  A MANUAL -g buys a sharper
                 floor (.45 px instead of .6): the user explicitly
                 takes responsibility for the ramp width; auto
                 steepness stays at the proven alias-free .6.  Either
                 way the EFFECTIVE k is reported (it was the "ignoring
                 parameters" lie: summary echoed 64 while the cap
                 silently rendered ~7). */
              float k = kbase;
              if (deblur_steepness <= 0.f && edge_goal > 0.f) {
                float st = fmaxf(.6f, edge_goal * scale / 2.5f);
                k = clampf(s / st, 1.f, 16.f);
              }
              k = fminf(k, s / .32f);
              float sp_dbg = -1.f, off0r = 0.f, off1r = 0.f,
                    bstep = ab_b;
              for (int c = 0; c < 4; c++) {
                Pc0[c] = P0[c];
                Pc1[c] = P1[c];
              }
              if (n0 > 0 && n1 > 0) {
                float sp = 0.f;
                for (int c = 0; c < 4; c++)
                  sp += (P1[c] - P0[c]) * d[c];
                sp /= L2;
                sp_dbg = sp;
                /* v4.9.3 narrow-feature amplitude restoration.  The
                   base blur at sigma ~R attenuates any feature whose
                   width is not >> sigma: a 4-6 px line at -r 6 keeps
                   only ~1/erf(w/2.4R) of its contrast, and anchored
                   steepening (which preserves plateaus) renders the
                   washout faithfully -- the smiley's mouth faded to
                   half contrast exactly this way.  Here the window
                   itself PROVES the geometry: the assigned flank and
                   its neighbouring flank lobe sit mu_li..mu_nb apart,
                   so the box-feature attenuation of the base render is
                   att = erf((w/scale)/(2sqrt2*sigma_b)) and the inner
                   plateau is corrected toward the outer one by
                   1/att (capped 1.75x and to [0,1]; without a
                   neighbouring flank att = 1 = the old behaviour, so
                   plain edges and unattended texture are untouched).
                   The correction enters BOTH the saturation zones
                   (line interiors move to the restored plateau) and
                   the effective step amplitude below, and the hull is
                   extended by exactly the corrected plateau. */
                if (!adb_noamp && coh > .2f)
                  for (int side = 0; side < 2; side++) {
                    double iM = 0., oL = 0., oN = 0.;
                    int nb = side == 0 ? li - 1 : li + 1;
                    if (nb < 0 || nb >= NL)
                      continue;
                    if ((side == 0 ? n0 : n1) < 2)
                      continue; /* plateau mean from 1 sample: too
                                   noisy to extrapolate */
                    /* Flank-pair test: the two lobes must be the TWO
                       FLANKS OF ONE FEATURE -- the profile between
                       them then forms an extremum (line interior /
                       ridge) that is MORE EXTREME than both outer
                       levels.  Wedge arms and T-junction strokes are
                       unrelated edges: the gap between them stays
                       monotone between the outer plateaus and such
                       pairs are rejected (cornerstar hull regression:
                       the bright wedge channel was extrapolated to
                       pure black). */
                    {
                      /* Inner level = the plateau samples on the
                         INNER side (u extreme toward the neighbour);
                         the lobes often touch (empty gap) while the
                         interior plateau sits inside the lobe's own
                         domain (the saturated u=0/u=1 run). */
                      iM = 0.;
                      oL = oN = 0.;
                      int ni = 0, n = 0;
                      float ulo = side == 0 ? .25f : .75f;
                      for (int j = dl; j <= dr; j++)
                        if (side == 0 ? u[j] < ulo : u[j] > ulo) {
                          iM += raw[j];
                          ni++;
                        }
                      if (ni < 2)
                        continue;
                      iM /= ni;
                      int eA = jr[li] + 1,
                          eB = jr[li] + mE < NS ? jr[li] + mE : NS - 1,
                          eC, eD;
                      eA = jr[li] + 1;
                      eB = jr[li] + mE < NS ? jr[li] + mE : NS - 1;
                      if (side == 1) {
                        int t = jl[li] - mE > 0 ? jl[li] - mE : 0;
                        eA = t;
                        eB = jl[li] - 1;
                      }
                      for (int j = eA; j <= eB; j++) {
                        oL += raw[j];
                        n++;
                      }
                      if (n)
                        oL /= n;
                      if (side == 0) {
                        int t = jl[nb] - mE > 0 ? jl[nb] - mE : 0;
                        eC = t;
                        eD = jl[nb] - 1;
                      } else {
                        eC = jr[nb] + 1;
                        eD = jr[nb] + mE < NS ? jr[nb] + mE : NS - 1;
                      }
                      n = 0;
                      for (int j = eC; j <= eD; j++) {
                        oN += raw[j];
                        n++;
                      }
                      if (n)
                        oN /= n;
                      /* Saturated interiors: a broad flat at the
                         window's raw extreme IS the extremum (it is
                         by construction deeper/higher than everything
                         else) -- the margin test cannot grade it
                         because the saturation spills into the
                         margins themselves. */
                      float rwmin = 1e30f, rwmax = -1e30f;
                      for (int j = 0; j < NS; j++) {
                        if (raw[j] < rwmin)
                          rwmin = raw[j];
                        if (raw[j] > rwmax)
                          rwmax = raw[j];
                      }
                      if (!((iM < oL - .02 && iM < oN - .02) ||
                            (iM > oL + .02 && iM > oN + .02) ||
                            iM <= rwmin + .02f ||
                            iM >= rwmax - .02f))
                        continue;
                    }
                    /* v4.9.5 tip entry: the coherence gate (> .85)
                       disables the restoration at exactly the wedge
                       tips and tight line ends that need it -- the
                       bending-contour coherence is low there BY
                       CONSTRUCTION, so the tip apex was left at the
                       blurred base (domed/eaten tips, "rounded too
                       much").  Below the gate, take only flank pairs
                       whose OUTER levels agree (both arms exit to the
                       same surround = a genuine dip or wedge tip; a
                       T-junction arm pairs unequal outsides --
                       stroke vs paper -- and is rejected as before,
                       which is the cornerstar bright-channel
                       regression guard). */
                    if (coh <= .85f) {
                      float dd2 =
                          (float)(fabs(iM - oL) + fabs(iM - oN)) * .5f;
                      if (fabs(oL - oN) >
                              .35f * (dd2 > .1f ? dd2 : .1f) ||
                          coh <= .25f)
                        continue;
                    }
                    float wsrc = fabsf(mus[li] - mus[nb]) / scale;
                    double sig = fitted_sigma > .3f ? fitted_sigma : .3;
                    double att =
                        erf((double)wsrc / (2.8284271 * sig));
                    /* junction/tip windows misread the flank geometry,
                       so coherence gates the extrapolation; the cap
                       keeps the correction strictly inside the source
                       colour range (cornerstar hull gate). */
                    float f = att > .20 ? (float)(1.0 / att) : 5.f;
                    if (f < 1.f)
                      f = 1.f;
                    /* v4.9.5 accumulate-mass depth.  Recovering
                       CONTRAST by 1/att actually claims "the dip's
                       true depth saturates the source colour range":
                       fine for genuinely black line art (the clamp
                       lands at native black) but a washed mid-tone
                       structure (miya lip line, true depth ~130-170
                       on a 19 skin field) was blown to pure 255-black
                       rails -- depth was invented, not accumulated.
                       The honest deblur ledger: blur conserves the
                       deficit MASS, so under the box+gauss dip model
                       observed_mass = depth * (wsrc + 2.83*sig_src)
                       must equal rendered_mass =
                       depth' * (wsrc + 2.83*sig_src/k), which pins
                       the depth multiplier at
                          f = k (wsrc + t) / (k wsrc + t), t = 2.83 sig_src
                       -- self-limiting: a shallow wide wash gets
                       depth~1 (keeps its own level), a point line
                       gets ~k (full concentration), wide features
                       are untouched (f -> 1).  Colour is only ever
                       RECONCENTRATED from the observed wash back into
                       the stroke, never created. */
                    {
                      float t = 2.8284271f * ((float)sig / scale);
                      float fms = k * (wsrc + t) / (k * wsrc + t);
                      if (fms > f)
                        f = fms;
                    }
                    if (f > 4.f)
                      f = 4.f; /* the gates (coherence, extremum +
                                direction consistency, base-model
                                saturation membership uf0/uf1 and
                                value-membership gInn) proved this is
                                the feature's interior, so the full
                                1/att depth is safe to CLAIM; the
                                missing ink on blurred line art is
                                proportional to 1/att, and capping at
                                2.25 left -r 6 strokes ~13% ink short
                                (watercolor).  The source-range clamp
                                below is still the hard guard */
                    /* coherence discount: full confidence above the
                       .85 gate (v4.9.1); below it (v4.9.5 tip entry,
                       proven symmetric flank pairs only) fade to zero
                       toward .30 so a barely-admitted cusp gets only
                       a gentle correction. */
                    f = 1.f + (f - 1.f) *
                              (coh > .85f ? 1.f
                                          : ss01((coh - .30f) *
                                                 (1.f / .40f)));
                    if (f <= 1.001f)
                      continue;
                  float *Pin = side == 0 ? P0 : P1, *Pout = side == 0 ? P1 : P0,
                        *Pc = side == 0 ? Pc0 : Pc1, *offv = side == 0 ? offv0 : offv1;
                  /* Direction-consistency (v4.9.3): f overshoots Pin
                     along (Pin - Pout); that is only the truth when
                     the OBSERVED inner samples sit past the fit's own
                     plateau on exactly that ray.  Degenerate windows
                     can land an inverted step on the core (fit rises
                     +2.2 over a dark interior) and f then extrapolates
                     OUTWARD -- a bright offset inside a dark line
                     renders as inverted barcode teeth. */
                  {
                    float pin_p = 0.f, pout_p = 0.f;
                    for (int c = 0; c < 4; c++) {
                      pin_p += Pin[c] * d[c];
                      pout_p += Pout[c] * d[c];
                    }
                    pin_p /= L2;
                    pout_p /= L2;
                    if ((pin_p - pout_p) * ((float)iM - pin_p) <= 1e-4f)
                      continue;
                  }
                  for (int c = 0; c < 4; c++) {
                    Pc[c] = Pout[c] + f * (Pin[c] - Pout[c]);
                    /* never extrapolate past the colours the SOURCE
                       itself proves (premultiplied-linear range) */
                    if (c < 3)
                      Pc[c] = clampf(Pc[c], adb_srclo[c], adb_srchi[c]);
                    else
                      Pc[c] = clampf(Pc[c], 0.f, 1.f);
                    offv[c] = Pc[c] - Pin[c];
                  }
                  /* restoration confidence: the side fired, so the
                     flank-pair extremum + direction checks all
                     passed; record it for the pass-1.5 restoration
                     weight (independent of the erf-tail trust gate:
                     tent-shaped line profiles misfit the erf ramp
                     while their PLATEAU is still measured solid) */
                  wS2 = 1.f;
                  /* v4.9.6 dip/line-class claim.  Every gate the
                     plateau restoration passes (extremum sandwich,
                     direction consistency, coherence) proves these
                     two flank lobes are ONE feature -- but the erf
                     step model then tracks the mid-SHOULDER of the
                     washed tent, which for a narrow line sits
                     ~(1.2-1.4)s outside the true half-depth contour:
                     steepening that contour paints the background
                     ~2 px wide = the bright-field halo (and fat,
                     rounded line ends).  The dip model replaces the
                     contour with the line CENTRE and the calibrated
                     TRUE half-width so the rendered stroke keeps the
                     source's geometry; first (nearest) firing pair
                     wins. */
                  if (lW <= 0.f) {
                    float muc = .5f * (mus[li] + mus[nb]);
                    float span = fabsf(mus[li] - mus[nb]);
                    float sloc = fs > .3f ? fs : .3f;
                    float hh = r_from_zstar(span / (2.f * sloc)) * sloc;
                    int jc = (int)lrintf(muc) + R;
                    if (jc < 0)
                      jc = 0;
                    if (jc > NS - 1)
                      jc = NS - 1;
                    {
                      float zj = ((float)(jc - R) - (float)fmu) / sloc;
                      float Fc = ab_a + ab_c * zj + ab_b * phi1(zj);
                      float sgn = uflip ? -1.f : 1.f;
                      lVX = muc * sgn * dirx;
                      lVY = muc * sgn * diry;
                      lDw = Fc - raw[jc];
                      lH = hh;
                      lW = 1.f;
                    }
                  }
                  /* feature membership BY VALUE for the pass-1.5
                     gate: how far the pixel's own BASE colour sits
                     from the inner (this side) plateau relative to
                     the observed plateau span, all in this window's
                     own d-projection metric.  ufit saturation cannot
                     tell line interiors from bright pixels a skirt-
                     width away (both read plateau-side), but the
                     base value can: only pixels already near the
                     inner plateau receive the restoration. */
                  {
                    float op = 0.f, p0p = 0.f, p1p = 0.f;
                    for (int c = 0; c < 4; c++) {
                      op += (o[c] - CA[c]) * d[c];
                      p0p += (P0[c] - CA[c]) * d[c];
                      p1p += (P1[c] - CA[c]) * d[c];
                    }
                    op /= L2;
                    p0p /= L2;
                    p1p /= L2;
                    float rel =
                        p1p - p0p > 1e-3f
                            ? clampf((op - p0p) / (p1p - p0p), -.2f,
                                     1.2f)
                            : .5f;
                    /* frame-independent INNERNESS: side0's inner
                       plateau is P0 (rel->0 there), side1's is P1
                       (rel->1).  Mirrored flank windows of one line
                       have opposite window frames, so raw rel mixes
                       to a meaningless 0.5 mid-feature (gates closed
                       exactly at the core); innerness agrees across
                       frames: 1 = base colour at the proven inner
                       plateau, 0 = at the far plateau. */
                    frel = side == 0 ? 1.f - rel : rel;
                  }
                }
                float cap = fmaxf(sp, .03f) * 1.2f;
                if (ab_b > cap)
                  ab_b = cap;
                if (ab_b < sp)
                  ab_b = sp;
                ab_c = clampf(ab_c, -.5f * cap, .5f * cap);
                for (int c = 0; c < 4; c++) {
                  off0r += offv0[c] * d[c];
                  off1r += offv1[c] * d[c];
                }
                off0r /= L2;
                off1r /= L2;
                /* Effective restored step amplitude (raw units along
                   d): observed erf step plus the plateau shift
                   difference.  Clamps live ONLY in this block --
                   outside it (no two-sided plateaus) fd2 = ab_b*d,
                   exactly the v4.9.1 behaviour. */
                bstep = ab_b + off1r - off0r;
                if (bstep < .25f * ab_b) {
                  /* nonsense restored span (geometry misread): drop
                     the restoration rather than flip the step
                     direction */
                  bstep = ab_b;
                  off0r = off1r = 0.f;
                  wS2 = 0.f;
                  frel = 0.f;
                  for (int c = 0; c < 4; c++) {
                    offv0[c] = 0.f;
                    offv1[c] = 0.f;
                    Pc0[c] = P0[c];
                    Pc1[c] = P1[c];
                  }
                }
                {
                  /* Restored span: one-sided proof sp plus the plateau
                     shifts; bstep may use the full restored span but
                     not more than 1.2x it (same policy as the v4.9.1
                     ab_b cap, generalised). */
                  float spe = sp + off1r - off0r;
                  if (bstep < .03f)
                    bstep = .03f;
                  if (bstep > 1.2f * fmaxf(spe, .03f))
                    bstep = 1.2f * fmaxf(spe, .03f);
                }
              }
              for (int c = 0; c < 4; c++)
                fd2[c] = bstep * d[c];
              /* Step-evidence coverage (v4.9.1): a step fit is only
                 admissible when the WINDOW's observed profile actually
                 spans the modelled step amplitude [a, a+b].  A pixel
                 more than R from the true edge sees only a linear
                 shading slope; fitting a "step" there invents a phantom
                 knee centred on the window itself and drags the shading
                 (the dark pre-edge "neon" band).  Shading is shading:
                 coverage < ~.55 fades trust to zero (pixel keeps its
                 own colour), a fully captured rise keeps it. */
              {
                float rmin = 1e30f, rmax = -1e30f;
                for (int j = 0; j < NS; j++) {
                  if (raw[j] < rmin)
                    rmin = raw[j];
                  if (raw[j] > rmax)
                    rmax = raw[j];
                }
                float bb = fabsf(ab_b) > .03f ? fabsf(ab_b) : .03f;
                float cov =
                    ((ab_a + ab_b < rmax ? ab_a + ab_b : rmax) -
                     (ab_a > rmin ? ab_a : rmin)) /
                    bb;
                /* v4.9.3: KEEP this gate inert (multiply into wS = 0,
                   the close-gate below starts the chain by overwrite,
                   exactly as shipped v4.9.1).  Making it live was
                   measured to strip steepening from TRUE-step skirts
                   (window narrower than the step it genuinely belongs
                   to): the cornerstar outside-hypotenuse strip filled
                   in with the blur skirt (.98 -> .86, the user's neon
                   detector) and junction tips lost their rounding
                   counterweight.  The neck-skirt phantom-step problem
                   coverage was designed for is already handled by the
                   shading-linear LS term + trust gates. */
                wS *= ss01((cov - .55f) * (1.f / .25f));
                if (dbg && y == dbg_y && abs(x - dbg_x) <= 16 &&
                    (x & 3) == 0)
                  fprintf(stderr, "DBGC %d,%d cov=%.3f raw[%.3f..%.3f] "
                                  "step[%.3f..%.3f]\n",
                          x, y, cov, rmin, rmax, ab_a, ab_a + ab_b);
              }
              /* Steepness k is decided right after the profile fit
                 (v4.9.4 hoist: the amplitude restoration's mass-
                 correct depth needs it); comment block lives there. */
              /* Anchored evaluation (v4.8): the steepened fit is
                 evaluated at the pixel's GEOMETRIC position on the
                 normal (t = 0), and the pixel's own residual to the
                 unsteepened fit is re-added: out = F_k(0) + (o - F(0)).
                 On-curve pixels steepen exactly with k; off-curve
                 deviations (texture, hue arcs, dither) pass through
                 with GAIN 1 instead of the v4.7 colour-domain gain k
                 at the ramp centre -- no amplified speckle mid-
                 gradient, no phi^-1 flat-noise halo, original colours
                 kept.  push: displace the position toward the nearer
                 plateau, evaluate the ORIGINAL profile there. */
              float z0 = (0.f - (float)mu) / s;
              float ufit0 = phi1(z0), nu;
              if (method == 2 && k > 1.f)
                nu = phi1(z0 + (ufit0 - .5f) * (k - 1.f) * 1.5f);
              else
                nu = phi1(k * z0);
              nu = clampf(nu, 0.f, 1.f);
              wS = ss01((sb - s) / (sb - sa));
              /* Fit-trust: RMSE of the erf fit over the full lobe,
                 |du| weights (the weights concentrate the check on the
                 lobe core, which is what the steepening actually
                 moves; a curved/textured ramp misfits and trust
                 fades). */
              {
                double en = 0, ed = 0;
                for (int j = jl[li]; j <= jr[li]; j++) {
                  float zj = ((float)(j - R) - (float)mu) / s;
                  float fj = ab_a + ab_c * zj + ab_b * phi1(zj);
                  en += wj[j] * (raw[j] - fj) * (raw[j] - fj);
                  ed += wj[j];
                }
                if (ed > 1e-9) {
                  float rmse = (float)sqrt(en / ed);
                  /* v4.9.3: flank-pair windows (this lobe has a
                     neighbouring flank) are PROVEN line geometry;
                     their profile is the kernel's tent/cone ramp (the
                     bspline base blurs a hard line into a near-linear
                     tent), which the erf systematically misfits by
                     ~.12-.18 -- the gate zeroed EVERY line-core pixel
                     at the user's -r 6 recipe and the deblur pass had
                     nothing to steepen or restore there.  Relax the
                     gate for exactly this class (the multi-crossing
                     and coh gates still apply below). */
                  float tl = trust_lo, thh = trust_hi;
                  if (li > 0 || li + 1 < NL) {
                    tl *= 2.5f;
                    thh *= 2.5f;
                  }
                  wS *= ss01((thh - rmse) / (thh - tl + 1e-9f));
                }
              }
              /* Multi-crossing trust (whole-window property; replaces
                 the v4.7 beta2 gate): per-lobe fits are ALL locally
                 good inside dense texture, so crosshatch/hair-clump/
                 text suppression must look at the window: count mid-
                 level crossings with hysteresis.  A step crosses 1x,
                 one line 2x (full trust), 4x+ (dense structure) fades
                 to zero. */
              {
                float lmed = .5f * (ul + ur);
                int side = 0, cross = 0;
                for (int j = 0; j < NS; j++) {
                  if (side <= 0 && u[j] > lmed + .02f) {
                    cross += side < 0;
                    side = 1;
                  } else if (side >= 0 && u[j] < lmed - .02f) {
                    cross += side > 0;
                    side = -1;
                  }
                }
                wS *= 1.f - ss01(((float)cross - 2.25f) / 2.5f);
              }
              /* Validity gate (v4.9): at junctions/corners/line caps
                 the 1D ramp model is outside its domain -- the window
                 mixes both arms of a wedge and 'assigns' bright-side
                 pixels to the dark segment (cornerstar apex: bright
                 pixels darkened by |delta| ~ .9, transported around
                 the tip by pass-2).  Downweight to coh: junctions
                 keep the crisp base sample (source-faithful sharp
                 tip) instead of a bogus 1D fit; straight contours
                 (coh ~ 1) are untouched. */
              wS *= coh;
              if (dbg && y == dbg_y && abs(x - dbg_x) <= 16 &&
                  (x & 3) == 0) {
                double en = 0, ed = 0;
                for (int j = jl[li]; j <= jr[li]; j++) {
                  float zj = ((float)(j - R) - (float)mu) / s;
                  float fj = ab_a + ab_c * zj + ab_b * phi1(zj);
                  en += wj[j] * (raw[j] - fj) * (raw[j] - fj);
                  ed += wj[j];
                }
                fprintf(stderr,
                        "DBG %d,%d NL=%d lobe[%d..%d] dom[%d..%d] "
                        "W=%.3f mu=%.3f s=%.3f k=%.3f z0=%.3f ufit=%.4f "
                        "nu=%.4f wS=%.4f rmse=%.4f Ld2=%.5f coh=%.2f "
                        "Teff=%d a=%.3f c=%.3f b=%.3f sp=%.3f o0=%.3f o1=%.3f "
                        "InvM=%d\n",
                        x, y, NL, jl[li], jr[li], dl, dr, W, mu, s, k,
                        z0, ufit0, nu, wS, ed > 1e-9 ? sqrt(en / ed) : -1.,
                        Ld2, coh, Teff, ab_a, ab_c, ab_b, sp_dbg, off0r, off1r,
                        adb_noamp ? -9 : (int)0);
              }
            }
          }
        }
      }
      /* Store the flat-flatten part of the delta, the wS-weighted fit
         parameter field for the pass-1.5 contour-consensus evaluation,
         and the local colour hull (u8) for the hull clamp; the pixel's
         own fit residual is NOT stored: it is re-added unsmoothed at
         write time via o[c] (texture stays per-pixel). */
      {
        float fw = ss01((.025f - rng) * (1.f / .017f)) * (1.f - wS);
        float *dd = DEL + 4 * idx, *p = PF + 25 * idx;
        for (int c = 0; c < 4; c++)
          dd[c] = fw * flatmix * (mean[c] - o[c]);
        p[0] = wS;
        p[1] = wS * fmu;
        p[2] = wS * fs;
        for (int c = 0; c < 4; c++)
          p[3 + c] = wS * fd2[c];
        p[7] = coh;
        p[8] = -diry; /* tangent of the fitted contour */
        p[9] = dirx;
        for (int c = 0; c < 4; c++) {
          p[10 + c] = wS2 * offv0[c]; /* fire-weighted: the consensus */
          p[14 + c] = wS2 * offv1[c]; /* denominator is aW2 (v4.9.3)  */
        }
        p[18] = wS2;
        p[19] = wS2 * frel; /* base-value feature membership (v4.9.3)  */
        p[20] = lW;  /* v4.9.6 dip/line-class claim: image-space     */
        p[21] = lW * lVX; /* vector to the line centre, depth and    */
        p[22] = lW * lVY; /* true half-width (the blur below spreads */
        p[23] = lW * lDw; /* them to all windows that merely SEE the */
        p[24] = lW * lH;  /* feature, which is exactly the veil zone)*/
        /* v4.9.3: widen the hull by the CORRECTED plateaus only (they
           are extrapolated past the observed window range by design;
           nothing else may exceed the observed colours). */
        if (offv0[0] != 0.f || offv0[1] != 0.f || offv1[0] != 0.f ||
            offv1[1] != 0.f)
          for (int c = 0; c < 4; c++) {
            if (Pc0[c] < lo[c])
              lo[c] = Pc0[c];
            if (Pc0[c] > hi[c])
              hi[c] = Pc0[c];
            if (Pc1[c] < lo[c])
              lo[c] = Pc1[c];
            if (Pc1[c] > hi[c])
              hi[c] = Pc1[c];
          }
        /* Hull codes in DISPLAY space (sRGB u8 for colour, linear u8
           for alpha): linear-space u8 codes have no usable resolution
           at the dark end (0.12 sRGB = 0.012 linear = code 3), which
           made the first hull clamp toothless exactly where fringes
           live. */
        for (int c = 0; c < 4; c++) {
          int lv, hv;
          if (c < 3) {
            lv = to_srgb[clampi((int)(clampf(lo[c], 0.f, 1.f) * 4096.f),
                                0, 4096)] -
                 1;
            hv = to_srgb[clampi((int)(clampf(hi[c], 0.f, 1.f) * 4096.f),
                                0, 4096)] +
                 1;
          } else {
            lv = (int)(clampf(lo[c], 0.f, 1.f) * 255.f) - 1;
            hv = (int)(clampf(hi[c], 0.f, 1.f) * 255.f + .999f) + 1;
          }
          LOH[8 * idx + c] = (uint8_t)clampi(lv, 0, 255);
          LOH[8 * idx + 4 + c] = (uint8_t)clampi(hv, 0, 255);
        }
      }
    }
  /* v4.9.3: widen the EVIDENCE field (wS) before the consensus.  Per-
     pixel trust flips under sub-pixel window shifts (the lobe map and
     the erf trust gate are both sampled-quantity sensitive), so wS is
     patchy at 1-2 px scale even along a genuine contour; when the
     restored-plateau delta is scaled by such a weight it renders as
     barcode teeth along narrow features.  All fit QUANTITIES stay on
     the sharp slots (ratios aMu/aW etc. are unchanged in meaning);
     only the scalar confidence w is read from a spatially low-passed
     copy (separable 5-tap box: +-2 px). */
  {
    /* Blur the fifteen wS-WEIGHTED slots jointly ([0] evidence, [1,2]
       mu/s, [3..6] d2, [10..17] plateau offsets): denominator and
       numerators get the same kernel so every ratio (mu, s, d2, off)
       stays calibrated where evidence exists and interpolates smoothly
       across the holes.  Slots 7 (coh) and 8,9 (tangent) stay sharp --
       they steer the taps, they are not consensus quantities. */
    /* restoration slots reach +-3 px: the firing windows can sit a
       full flank width away from the feature core they describe (the
       +-2 kernel left un-fired cores washed and tripled MAE at the
       48px probe).  Fit-parameter slots stay +-2 (sharper). */
    static const int slots2[7] = {0, 1, 2, 3, 4, 5, 6};
    static const int slots3[10] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    /* dip/line-claim slots (20-24) reach +-5 px: firing windows sit a
       full flank width away from the veil zone they describe, and the
       veil is exactly where the replacement must land. */
    static const int slots5[5] = {20, 21, 22, 23, 24};
    static const int *slotsetA[3] = {slots2, slots3, slots5};
    static const int nslotsA[3] = {7, 10, 5};
    static int radiiA[3] = {2, 3, 5};
    {
      const char *e = getenv("CELUP_RADA");
      if (e) {
        int r2, r3, r5;
        if (sscanf(e, "%d,%d,%d", &r2, &r3, &r5) == 3) {
          radiiA[0] = r2;
          radiiA[1] = r3;
          radiiA[2] = r5;
        }
      }
    }
    /* v4.9.9e: TANGENT-ORIENTED evidence integration.  The separable
       axis-aligned box blur reaches R px across the contour
       everywhere, but at a contour's XY poles that axis coincides
       with the contour NORMAL: at the ring's top/bottom/left/right
       the +-3 box mixes the inner flank's evidence with the outer
       flank's (opposite deltas cancel, nu claims flip per pixel) --
       the measured "additional blur at the poles of circles since
       v4.8", left by both tangential mechanisms' shared premise that
       averaging must follow the contour.  Everywhere else the same
       arithmetic is the intended consensus.  Marching the taps along
       the per-pixel tangent (bilinear samples, tent kernel, sign-
       free by symmetry) keeps exactly the old effective support
       along-trace and removes any cross-contour reach: no special-
       case code, one rule for all angles. */
    float *tmp = malloc((size_t)dw * dh * sizeof *tmp);
    if (tmp)
      for (int sset = 0; sset < 3; sset++)
        for (int sl = 0; sl < nslotsA[sset]; sl++) {
          const int q = slotsetA[sset][sl], R = radiiA[sset];
          for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
              size_t idx = (size_t)y * dw + x;
              const float *pf = PF + 25 * idx;
              double acc = 0., ws = 0.;
              for (int to = -R; to <= R; to++) {
                float wt = (float)(R + 1 - (to < 0 ? -to : to)), qv[25];
                sample_fn(PF, dw, dh, 25, (float)x + (float)to * pf[8],
                          (float)y + (float)to * pf[9], qv);
                acc += (double)wt * qv[q];
                ws += wt;
              }
              tmp[idx] = ws > 1e-9 ? (float)(acc / ws) : 0.f;
            }
          for (size_t k = 0; k < (size_t)dw * dh; k++)
            PF[25 * k + q] = tmp[k];
        }
    free(tmp);
  }
  /* Pass 1.5: contour-consensus evaluation (v4.9).  Raw per-pixel fits
     jitter (mu by up to 1-2 out px near wedges/apexes) and the anchored
     output renders that jitter amplified ~k-fold -- pass-2 delta
     smoothing patched the symptom, this treats the cause: the fit
     parameters themselves are constant/move coherently ALONG a genuine
     contour, so integrate the wS-weighted parameter field along the
     tangent (junction-aware weights, wS as the weight of evidence --
     junction-gated and untrusted pixels contribute no mass) and render
     each pixel from the CONSENSUS fit: z, k, nu are recomputed from the
     smoothed mu/s so the anchored evaluation stays exact, and the
     pixel's own residual (o - F(smoothed fit)) is still re-added per
     pixel at gain 1.  The local convex-hull clamp (no ringing
     vocabulary: erf tail-shape misfit at the ramp foot once drove the
     step48 dark flank .149 -> .027) bounds the result to the colours
     observed in the pixel's own window. */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      const float *pf = PF + 25 * idx;
      float tanx = pf[8], tany = pf[9], cc = pf[7];
      float aW = 0.f, aW2 = 0.f, aR = 0.f, aMu = 0.f, aS = 0.f,
            aD[4] = {0, 0, 0, 0}, aO0[4] = {0, 0, 0, 0},
            aO1[4] = {0, 0, 0, 0}, wsum = 0.f, tw[9], tmu[9];
      float aWL = 0.f, aVX = 0.f, aVY = 0.f, aWD = 0.f, aHH = 0.f;
      for (int to = -T; to <= T + 2; to++) {
        float wt, q[25];
        if (to <= T) { /* tangential taps (the v4.8 consensus) */
          wt = (float)(T + 1 - (to < 0 ? -to : to));
          sample_fn(PF, dw, dh, 25, (float)x + (float)to * tanx,
                    (float)y + (float)to * tany, q);
        } else { /* v4.9.3: one NORMAL tap to each side, lower weight.
                    The per-pixel evidence field (wS) has holes at
                    1-2 px scale across the flank (window lobe maps
                    flip NL under sub-pixel center shifts), and purely
                    tangential averaging cannot bridge them: on narrow
                    features whose evidence stripe is only a few px
                    wide the consensus weight w then oscillates
                    pixel-to-pixel and the restored-plateau delta
                    renders as a barcode along the contour.  Symmetric
                    +-1 normal taps keep mu unbiased (linear mu sweep
                    cancels) while filling the holes. */
          float no = to == T + 1 ? -1.f : 1.f;
          wt = .55f * (T + 1) * .5f;
          sample_fn(PF, dw, dh, 25, (float)x + no * -tany,
                    (float)y + no * tanx, q);
        }
        wt *= sqrtf(cc * fmaxf(q[7], 0.f));
        if (to <= T) {
          tw[to + T] = wt;
          tmu[to + T] = q[0] > 1e-9f ? q[1] / q[0] : 0.f;
        }
        aW += wt * q[0];
        aW2 += wt * q[18];
        aR += wt * q[19];
        aMu += wt * q[1];
        aS += wt * q[2];
        aWL += wt * q[20];
        aVX += wt * q[21];
        aVY += wt * q[22];
        aWD += wt * q[23];
        aHH += wt * q[24];
        for (int c = 0; c < 4; c++) {
          aD[c] += wt * q[3 + c];
          aO0[c] += wt * q[10 + c];
          aO1[c] += wt * q[14 + c];
        }
        wsum += wt;
      }
      if (aW > 1e-6f && wsum > 1e-6f) {
        float o[4];
        float mu = aMu / aW, s = aS / aW, w = aW / wsum,
              w2v = clampf(aW2 / wsum, 0.f, 1.f), d2[4], off0e[4],
              off1e[4];
        /* membership-by-value: innerness~1 = the pixel's base colour
           already sits at the washed inner plateau (genuine feature
           interior: restore); innerness near 0 = the skirt or the
           background, where saturation-only claims used to repaint
           dark blobs around blurred lines (-r6 diagline probe). */
        float gInn = .5f, inn_ = .5f;
        if (aW2 > 1e-6f) {
          inn_ = clampf(aR / aW2, -.2f, 1.2f);
          gInn = ss01((inn_ - .5f) * (1.f / .3f));
        }
        /* offsets normalize by their OWN fire evidence aW2: windows
           that could not prove a flank pair keep wS2 = 0 and must not
           dilute the restoration amplitude of those that did --
           diluted by 10x at the 48px probe core, the restored core
           reverted to washed grey (the consensus-steal bug). */
        float aW2s = aW2 > 1e-6f ? aW2 : 1e-6f;
        for (int c = 0; c < 4; c++) {
          d2[c] = aD[c] / aW;
          off0e[c] = aO0[c] / aW2s;
          off1e[c] = aO1[c] / aW2s;
          o[c] = A[4 * idx + c];
        }
        /* Steepness from the CONSENSUS width: same rules as pass 1,
           then governed by the mu SPREAD along the tangent.  A k-
           steepened step renders a mu disagreement of d px as an edge
           displaced ~d*k -- the dark pre-edge "neon" bands are exactly
           that on shaded wide ramps (-r 6).  Straight stable contours
           read std << .5 px and keep full k; skirts/wedges/dense zones
           with std >= ~2.5 px get k -> 1 (left at their own slope). */
        float k = kbase, vmu = 0.f, wt2 = 0.f;
        for (int to = -T; to <= T; to++) {
          if (tw[to + T] <= 0.f)
            continue;
          float dd = tmu[to + T] - (float)mu;
          vmu += tw[to + T] * dd * dd;
          wt2 += tw[to + T];
        }
        float mu_std = wt2 > 1e-9f ? sqrtf(vmu / wt2) : 0.f;
        (void)mu_std; /* v4.9.1: the mu-spread governor is REMOVED.
                         Along a bending edge (tip/corner) mu varies
                         along the tangent by construction, so the
                         governor only ever fired at exactly the
                         geometry it destroyed (k -> 1 there = rounded
                         tips); on straight shaded ramps (its target)
                         mu_std reads < .35 and it never engaged. */
        if (deblur_steepness <= 0.f && edge_goal > 0.f) {
          float st = fmaxf(.6f, edge_goal * scale / 2.5f);
          k = clampf(s / st, 1.f, 16.f);
        }
        k = fminf(k, s / .32f);
        /* v4.9.3 effective-k bookkeeping (the report used to echo the
           REQUESTED steepness even when the sawtooth cap replaced it
           almost everywhere -- "ignoring parameters"). */
        if (w > 1e-3f) {
          adb_keff_sum += (double)k * w;
          adb_keff_w += (double)w;
          if (k > adb_keff_max)
            adb_keff_max = k;
        }
        /* Anchored evaluation (v4.8) on the consensus fit. */
        float z0 = (0.f - mu) / s;
        float ufit0 = phi1(z0), nu;
        /* saturation membership: the BASE (unremapped) model must
           still place the pixel on this side's plateau before the
           restored plateau may be claimed for it.  Under -r 6 a
           narrow line's blur shoulder reads exactly like its washed
           core (both sit at the attenuated plateau level, inn ~1),
           so only model-u separates the interior from the rim;
           without this the offset paints a neon band +-1.5 flanks
           wide around narrow lines. */
        if (method == 2 && k > 1.f)
          nu = phi1(z0 + (ufit0 - .5f) * (k - 1.f) * 1.5f);
        else
          nu = phi1(k * z0);
        nu = clampf(nu, 0.f, 1.f);
        /* v4.9.6 dip/line render: where the consensus carries a
           flank-pair line claim, REPLACE the erf-step evaluation of
           the feature by the calibrated dip: the washed profile loses
           the observed centre depth (u -= Dw*dipN(.;h,s)), the
           steepened profile carries the restoration-proven depth at
           the TRUE half-width (nu -= Dt*dipN(.;h,s/k)).  The step
           part keeps its own semantics (ul != ur backgrounds ride the
           step as before); far from the feature dipN ~ 0 and the
           claim is a no-op, and the vector-to-centre field is
           image-space so the consensus average is frame-free.  The
           mid-shoulder fat-contour pathology ("halo" = the steepened
           fat contour painting the background ~2 px wide) cannot
           arise: the dip flanks sit at the true half-depth width. */
        {
          static int nodip = -1;
          if (nodip < 0)
            nodip = getenv("CELUP_NODIP") != NULL;
#ifndef CELUP_DIP_DEF
#define CELUP_DIP_DEF 0
#endif
          {
            static int dipon = -1;
            if (dipon < 0)
              dipon = getenv("CELUP_DIP") != NULL ? 1 : CELUP_DIP_DEF;
            if (!dipon || nodip)
              aWL = 0.f;
          }
          /* consensus-diluted claim strength: even one firing tap
             against 8 non-firing neighbours reads wLc ~ .12, so the
             gate sits low; the flank-pair tests in pass 1 already
             proved the geometry, this is only a spatial reach
             control. */
          if (aWL > 1e-6f && wsum > 1e-6f) {
            float wLc = aWL / wsum;
            float lfac = ss01((wLc - .03f) * (1.f / .10f));
            if (lfac > 1e-3f) {
              float vx = aVX / aWL, vy = aVY / aWL;
              float pabs = sqrtf(vx * vx + vy * vy);
              float hh = clampf(aHH / aWL, 0.f, 40.f);
              float dwc = clampf(aWD / aWL, -1.2f, 1.2f);
              float sp = s / (k > .05f ? k : .05f);
              sp = clampf(sp, .32f, 40.f);
              float sw2 = clampf(s, .4f, 40.f);
              float dnw = dipnorm(pabs, hh, sw2);
              float dns = dipnorm(pabs, hh, sp);
              float num = 0.f, den = 0.f;
              for (int c = 0; c < 4; c++) {
                num += (off0e[c] + off1e[c]) * d2[c];
                den += d2[c] * d2[c];
              }
              float dt = den > 1e-9f ? -num / den : 0.f;
              dt = clampf(dt, -1.2f, 1.2f);
              ufit0 -= lfac * dwc * dnw;
              nu -= lfac * dt * dns;
              ufit0 = clampf(ufit0, -.2f, 1.2f);
              nu = clampf(nu, 0.f, 1.f);
            }
          }
        }
        /* saturation membership: the BASE (unsteepened) model must
           still place the pixel on this side's plateau before the
           restored plateau may be claimed for it -- nu cannot do this
           job: phi1(k*z0) saturates at |z0| > ~2.5/k, so it reads
           "plateau" BOTH 4 px inside a stroke and 4 px outside the
           edge (v4.9.4 experiment: nu-based membership painted the
           washed skirt black, diagline MAE 13.7 -> 17.6).  The
           unsteepened ufit0 keeps the SIDE truth.  Gate centre
           lowered .78 -> CELUP_UFM (default .70, scanned .55-.78 on
           the smiley and diagline fixtures; .70 was the best joint
           point): a narrow -r 6 line's interior pixels stop at
           ufit0 ~ .6-.9
           (the whole stroke is ~2.9 sigma wide), and the .78 centre
           paid only the dead-centre sliver -- the proven interior
           stayed at the attenuated plateau (watercolor).  The halo
           guard on the bright outer side is the value-membership
           gInn, measured on the pixel's own base colour. */
#ifndef CELUP_UFM
#define CELUP_UFM .70f
#endif
        float uf0 = 1.f - ss01((ufit0 - (1.f - CELUP_UFM)) *
                               (1.f / .20f));
        float uf1 = ss01((ufit0 - CELUP_UFM) * (1.f / .20f));
        /* v4.9.5 dip-core tent: when BOTH sides' flank-pair
           restoration fired on this window (extremum-tested from
           BOTH flanks -- the lobe is a sandwiched dip/line core; a
           plain one-sided flank fires only one offset), the restored
           core colour must also be paid at the dip CENTRE itself,
           ufit ~ .5, which neither uf-side gate ever claims: a 1-2
           px line otherwise renders as two dark rails with a bright
           mid seam (the user's "mouth vertically doubled" on miya,
           and the smiley r6 ring double edge).  The tent is the
           conservative (min-magnitude, same-sign) component of the
           two offsets, gated by shape to vanish at both plateaus so
           it cannot add anything to one-sided edges. */
        float uft = ss01((.5f - fabsf(ufit0 - .5f) - .2f) * (1.f / .15f));
        /* v4.9.6 skirt transport (bright side): at pixels the model
           itself places on the SATURATED bright plateau (nu ~ 1) the
           anchored evaluation still re-adds the gain-1 residual
           o-F(0), and the erf tail undercalls the composite blur
           skirt (lobe s is read off the |du|-weighted shoulder; a
           neighbouring stroke's far tail belongs to no window's
           model) -- that leftover is the bright veil around strokes
           (smiley r6 halo 17.4 vs NN 1.9) and the chroma bleed at
           colour borders.  No DC-exact smoother can remove that
           mass, so TRANSPORT it: look ~2.6s farther along the normal
           (both directions; the tangent->normal map flips sign with
           the pass-1 u-reversal), and where an evidence-backed tap
           is itself claimed bright-plateau-saturated with an
           agreeing step direction, pull the pixel toward the
           OBSERVED base colour there.  Targets are neighbourhood
           observations (no invented colours; hull-clamped after),
           seams between close strokes reject themselves (the tap
           lands on the next stroke's ramp: no same-side saturation),
           and dark interiors are untouched (core depth is the
           restoration terms' job). */
        float wpeel = 0.f, atap[4] = {0, 0, 0, 0}, lvlT = 0.f;
#ifndef CELUP_NOPEEL_ENV
        {
          static int nopeel = -1;
          if (nopeel < 0)
            nopeel = getenv("CELUP_NOPEEL") != NULL;
          /* v4.9.7: own-line plateau transport replaces the v4.9.6
             tap transport.  v4.9.6 pulled saturated-claim pixels
             toward the RAW colours sampled +-2.6s along the normal;
             tap averages are not the pixel's own asymptote, so
             bright-plateau pixels near strokes were dragged to a
             mid-gray tap mean -> a smooth black ring hugging every
             stroke's OUTER flank (miya mouth, smiley outline).  Same
             probes, new discipline: a probe must (a) REACH the
             pixel's own model plateau projection along d2
             (existence-of-plateau test: cancels between close
             strokes and inside real gradients where no plateau is
             within tail distance) and (b) be strictly BRIGHTER along
             d2.  The colour then moves strictly along the pixel's
             OWN step line: atap = o + pv*.d2/|d2|, so foreign hues
             can never bleed in and the target can never sit below
             the claimed asymptote -- no darkening, no halo; the
             level is capped near the observed plateau and the hull
             clamp bounds the rest.  Bright branch (nu > .88) keeps
             v4.9.6 semantics. */
          float satb = ss01((nu - .88f) * (1.f / .09f));
          /* trough branch (v4.9.7b): ownership by the UNSTEEPENED
             model position.  True ink-fills saturate the base
             profile (ufit0 ~ 0) while the wash trough reads
             .1-.4 -- without the floor the brightening fired deep
             inside fat ink fills.  The wider ufit0-window variant
             (any model-mid pixel treated as trough) bleached thin
             strokes and reopened the halo class; keep the measured
             v4.9.7c/d gates. */
          float satd = ss01((.12f - nu) * (1.f / .06f)) *
                       ss01((ufit0 - .06f) * (1.f / .12f)) *
                       ss01((-z0 - 1.2f) * (1.f / .6f)) *
                       ss01((.60f - inn_) * (1.f / .15f)) *
                       ss01((s - 2.f) * (1.f / 2.f));
          float sat = satb > satd ? satb : satd;
          if (!nopeel && w > .04f && sat > .02f) {
            float nax = pf[9], nay = -pf[8];
            float Ld2 = 1e-9f;
            for (int c = 0; c < 4; c++)
              Ld2 += d2[c] * d2[c];
            float Ld = sqrtf(Ld2);
            float pvmin = (1.f - ufit0) * Ld - .03f;
            float best = 0.f, okm = 0.f;
            float dist = clampf(2.6f * s, 5.f, 20.f);
            for (int sgn = -1; sgn <= 1; sgn += 2) {
              float qa[4], pv = 0.f;
              sample_f4(A, dw, dh, (float)x + (float)sgn * dist * nax,
                        (float)y + (float)sgn * dist * nay, qa);
              for (int c = 0; c < 4; c++)
                pv += (qa[c] - o[c]) * d2[c];
              pv /= Ld2;
              float ok = ss01((pv * Ld - pvmin) * (1.f / .06f));
              pv *= Ld;
              if (pv < .015f)
                ok = 0.f; /* strictly brighter plateau along d2 only */
              if (ok > 1e-3f && pv > best) {
                best = pv;
                okm = ok;
              }
            }
            if (okm > 1e-3f) {
              /* target = own step line lifted to the observed plateau
                 level pv* (bounded by the hull clamp afterwards);
                 payment scales with how far the plateau sits above
                 the model's own claim. */
              if (best > (1.f - ufit0) * Ld)
                best = (1.f - ufit0) * Ld + .35f * (best - (1.f - ufit0) * Ld);
              if (best > 1.05f * Ld)
                best = 1.05f * Ld;
              for (int c = 0; c < 4; c++)
                atap[c] = o[c] + best * d2[c] / Ld;
              lvlT = best;
              wpeel = w * sat * okm;
            }
          }
        }
#endif
        float *dd = DEL + 4 * idx;
        float c0blo_dbg = -1.f;
        /* v4.9.3: widen the local colour hull by the CONSENSUS
           restoration offsets.  Pass 1 could only extend the hull by
           its own window's Pc, so non-firing windows (NL=1 cores,
           catastrophic degenerate fits) kept the washed minimum as
           their lower bound -- the clamp then pushed consensus-restored
           dark pixels back up to the washed level on exactly those
           pixels: visible teeth.  The consensus offsets are the
           blur-smoothed, extremum- and direction-tested restoration,
           so this extension stays inside proven colours while being
           spatially smooth (no per-pixel bound flapping). */
        if (w2v > 1e-3f)
          for (int c = 0; c < 4; c++) {
            float dlo = fminf(0.f, fminf(off0e[c], off1e[c])) * w2v,
                  dhi = fmaxf(0.f, fmaxf(off0e[c], off1e[c])) * w2v;
            if (dlo == 0.f && dhi == 0.f)
              continue;
            if (c < 3) {
              float lo = clampf(to_linear[LOH[8 * idx + c]] + dlo,
                                adb_srclo[c], adb_srchi[c]),
                    hi = clampf(to_linear[LOH[8 * idx + 4 + c]] + dhi,
                                adb_srclo[c], adb_srchi[c]);
              int lv = to_srgb[clampi((int)(clampf(lo, 0.f, 1.f) *
                                            4096.f),
                                      0, 4096)] -
                       1,
                  hv = to_srgb[clampi((int)(clampf(hi, 0.f, 1.f) *
                                            4096.f),
                                      0, 4096)] +
                       1;
              LOH[8 * idx + c] = (uint8_t)clampi(lv, 0, 255);
              LOH[8 * idx + 4 + c] = (uint8_t)clampi(hv, 0, 255);
            } else {
              float lo = LOH[8 * idx + c] * (1.f / 255.f) + dlo,
                    hi = LOH[8 * idx + 4 + c] * (1.f / 255.f) + dhi;
              LOH[8 * idx + c] =
                  (uint8_t)clampi((int)(clampf(lo, 0.f, 1.f) * 255.f) -
                                      1,
                                  0, 255);
              LOH[8 * idx + 4 + c] =
                  (uint8_t)clampi(
                      (int)(clampf(hi, 0.f, 1.f) * 255.f + .999f) + 1,
                      0, 255);
            }
          }
        /* v4.9.8 erf-gain post-map (the grey test: mid-grey on the
           output of a step-class source is wash, by definition).
           Goal: modify the gradient to increase its slope with zero
           possibility of overshoot.  Derivation: a Gaussian-blurred
           edge is v(x) = P0 + A*Phi((x-x0)/s); de-blurring the edge
           s -> s/g is exactly (x-x0) -> g*(x-x0), so with the
           rendered colour written as the hull fraction
           m = ((v-P0).A)/(A.A)  (A = P1-P0 = the pixel's OWN local
           colour hull, i.e. observed/proven colours only), the map
             m' = Phi(g * Phi^-1(m)),   v' = P0 + m'*A
           IS that de-blur.  Safety by construction, not by tuning:
           Phi o (g*Phi^-1) is a composition of monotone maps with
           range (0,1), so v' is ALWAYS strictly inside the proven
           hull [P0,P1] -- overshoot (ringing) is impossible, and the
           colour stays on the hull diagonal between two observed
           colours (no foreign hues can enter).  The plateaus are
           attractors (m ~ 0/1 snap fully: wash shoulders and
           cross-wash are re-absorbed into their own plateau),
           m = .5 is a fixpoint (the soft ramp core survives,
           compressed by 1/g), and the wash gate |m - nu| (applied
           after the colour is finished, below) fires only where the
           k-steepened model's position claim disagrees with the
           actual colour -- the analytic signature of blur wash.
           Genuine gradients render m ~ nu and stay verbatim. */
#ifndef CELUP_ZG_DEF
#define CELUP_ZG_DEF 2.2f
#endif
#ifndef CELUP_ZAA_DEF
#define CELUP_ZAA_DEF .001f
#endif
        float vv[4] = {0.f, 0.f, 0.f, 0.f},
              vblo[4] = {0.f, 0.f, 0.f, 0.f},
              vbhi[4] = {0.f, 0.f, 0.f, 0.f};
        float zg = CELUP_ZG_DEF, zaa = CELUP_ZAA_DEF, zmw = 0.f,
              zrmp = 0.f;
        int zno = 0;
        {
          static int noz = -1;
          static float zg_env = -1.f;
          if (noz < 0)
            noz = getenv("CELUP_NOZ") != NULL;
          if (zg_env < 0.f) {
            const char *e = getenv("CELUP_ZG");
            zg_env = e && *e ? (float)atof(e) : 0.f;
          }
          if (zg_env > .5f)
            zg = zg_env;
          zno = noz;
        }
        for (int c = 0; c < 4; c++) {
          float blo = adb_nohull ? 0.f : (c < 3 ? to_linear[LOH[8 * idx + c]]
                            : LOH[8 * idx + c] * (1.f / 255.f)),
              bhi = adb_nohull ? 1.f : (c < 3 ? to_linear[LOH[8 * idx + 4 + c]]
                            : LOH[8 * idx + 4 + c] * (1.f / 255.f));
          /* v4.9.4 soft hull: expand by deblur confidence w and qconf gate. */
          {
            float hexpand = w * adb_qconf * .55f;
            float slo = c < 3 ? adb_srclo[c] : 0.f,
                  shi = c < 3 ? adb_srchi[c] : 1.f;
            blo -= hexpand * fmaxf(blo - slo, 0.f);
            bhi += hexpand * fmaxf(shi - bhi, 0.f);
          }
          if (c == 0)
            c0blo_dbg = blo;
          /* v4.9.3: delta = contour remap (nu-ufit)*d2 PLUS the
             plateau restoration lerp(off0,off1,nu) -- at saturation
             (nu->0/1) the remap vanishes and the restored plateau
             remains, so narrow-feature interiors recover their
             un-attenuated colour instead of the washed base value. */
          /* steepening keeps the rmse-trust weight w; the plateau
             restoration runs on its own smooth confidence w2v (the
             erf tail misfit of tent-shaped line cores used to gate
             the plateau away pixel-by-pixel = barcode teeth) */
          float vr = w2v * gInn;
          /* v4.9.4: at a pixel that every gate independently places
             DEEP inside a proven washed feature, replace the smeared
             consensus weight with full restoration: w2v is a blurred
             confidence (max ~0.5-0.7 in firing zones) and paid only
             ~60% of the proven depth back -- the -r 6 smiley strokes
             stopped at ~215/255 ink (watercolor).  Where the base
             model's own saturation puts the pixel squarely on this
             side's plateau (nu ~ 0/1, uf ~ 1) and the value gate
             agrees (gInn > .6), fire the proven offset at full
             strength instead (still no weight when any gate is
             partial, so rims/skirts keep the diluted path). */
        if (nu < .02f && uf0 > .98f && gInn > .6f)
            vr = gInn * uf0;
          if (nu > .98f && uf1 > .98f && gInn > .6f)
            vr = gInn * uf1;
        /* dip-core tent component (v4.9.5): conservative same-sign
             component of the two offsets, paid at the dip centre
             where the side gates are silent; uses the ordinary
             diluted weight (no fullfire -- shape, not depth). */
          float t0 = off0e[c], t1 = off1e[c], tc = 0.f;
          if (t0 * t1 > 0.f)
            tc = fabsf(t0) < fabsf(t1) ? t0 : t1;
          float v = clampf(o[c] + w * ((nu - ufit0) * d2[c]) +
                               vr * (uf0 * (1.f - nu) * off0e[c] +
                                     uf1 * nu * off1e[c]) +
                               w2v * gInn * uft * tc,
                           blo, bhi);
          if (wpeel > 0.f)
            v = clampf(v + wpeel * (atap[c] - v), blo, bhi);
          vv[c] = v;
          vblo[c] = blo;
          vbhi[c] = bhi;
        }
        /* v4.9.8 erf-gain post-map, applied to the FINISHED colour:
           m -> Phi(zg * Phi^-1(m)) along the pixel's own proven
           plateau line (see above), re-clamped to the local hull.
           Alpha is left alone (smoke-fringe class, parked). */
        float zm_dbg = -1.f, zmm_dbg = -1.f, zal_dbg = -1.f,
              wz_dbg = 0.f;
        {
          /* endpoints = the pixel's OWN local colour hull (LOH:
             separable extrema of observed colours, consensus-widened;
             measured earlier: hull colours == NN colours within ~1
             8-bit step inside NN zones), NOT the LSQ step amplitude,
             which is under-read on washed flanks (m measured against
             o+-.ufit*d2+offs was uncalibrated: |A| ~ 1.3 in a 0..1
             range, P0 ~ -1).  m = hull fraction of the finished
             colour; steepen in z-space: mm = Phi(zg * Phi^-1(m)). */
          float num = 0.f, den = 0.f, calh = 0.f;
          for (int c = 0; c < 3; c++) {
            float Aq = vbhi[c] - vblo[c], rq = adb_srchi[c] - adb_srclo[c];
            num += (vv[c] - vblo[c]) * Aq;
            den += Aq * Aq;
            calh += rq * rq;
          }
          calh = sqrtf(calh);
          if (den > 1e-8f) {
            float m = clampf(num / den, 0.f, 1.f);
            float alh = sqrtf(den);
            /* v4.9.8b corridor map: the base model reports this
               pixel's signed offset mu from the PROVEN step centre.
               Within the true-AA corridor |mu| <= a (= zaa*scale out
               px) the rendered value is LEFT ALONE: it encodes the
               feature's exact sub-pixel width -- information that
               lives only in the boundary pixels' values; any
               positional repaint (linear ramp etc.) destroys it
               (crisp-GT check: MAE 13.2 -> 19+).  BEYOND the
               corridor a mid value cannot be true anti-aliasing --
               the underlying crisp image has its plateau there -- so
               it is blur wash, mapped fully to the pixel's own
               proven plateau (hull endpoint on the side d2 points
               to, globally extended on global-extremes steps).
               Monotone side selection + proven endpoints + hull
               clamp: overshoot impossible by construction. */
            {
              static float zaa_env = -2.f;
              if (zaa_env < -1.f) {
                const char *e = getenv("CELUP_ZAA");
                zaa_env = e && *e ? (float)atof(e) : 0.f;
              }
              if (zaa_env > 0.f)
                zaa = zaa_env;
            }
            float a = zaa * scale;
            float mmu = mu < 0.f ? -mu : mu;
            /* v4.9.9 deblur completion.  The value map
               mm = Phi(zg*Phi^-1(m)) has an unbreakable fixpoint at
               m = .5: one washed mid column per contour always
               survives (the residual "sharper halo").  A POSITIONAL
               tie-break via the consensus step offset mu does not
               work either: between a thin stroke's two flanks the
               nearest-lobe step assignment flips per pixel, and
               rendering that with a sub-pixel transition width is
               salt-and-pepper (measured catastrophe v4.9.9a: the eye
               ring doubled and hashed).  The stable tie-break is the
               pixel's own COLOUR side: m is smooth (it is the
               finished colour projected on the hull diagonal), so a
               large-but-finite gain in m-space places the threshold
               crossing on a smooth curve -- on quantized (step-class)
               sources boost the value gain zg by ZBOOST (default 4x
               => effective 12): everything with m < .47 snaps to its
               own dark plateau, m > .53 to the bright one, and the
               gray band shrinks to the sub-pixel strip where the
               colour itself is truly mid.  Same construction, same
               proof: monotone map, range (0,1), proven endpoints --
               no overshoot is possible, only geometry decided by the
               colour itself. */
            /* step-class completion needs TWO premises: a quantized
               source AND real blur to undo.  At -r <= ~1.2 the base
               adds no composite wash -- the source staircase is then
               true quantizer structure the renderer must preserve
               verbatim (the -l 0.5 crisp probe: hide the treads and
               the staircase detector goes toothless), so the
               completion fades out there. */
            float qq = ss01((adb_qconf - .60f) * (1.f / .30f)) *
                       ss01((sref - 1.2f) * (1.f / .8f));
            /* huge local hull span = a true step lives here, not a
               smooth-gradient fragment (skin/hair shading spans far
               less): extend the endpoints toward the source-global
               range -- in a wide wash the local extrema never reach
               the true plateau (the washed floor ~49/255 reads as the
               "plateau"), so the snap targets the span the step
               ACTUALLY connects.  calh = the source-GLOBAL channel
               range span; rmp engages only when the local window
               demonstrably straddles a global-extremes step. */
            /* v4.9.9c full extension on quantized sources: the r6
               wash band's local hull spans 60-90% of the global
               range, so the proportional extension left (1-rcp) of
               the washed floor inside the snap target -- measured
               remnants .084 linear ~ lum 80.  On a step-class source
               every map-admissible step is taken to connect the
               source-global extremes (small local steps -- colour
                 pairs in multi-level content -- never pass the alh
                 admission floor), so extend to the extremes outright
                 whenever the step-span gate fires at all. */
            float rmp = ss01((alh - .22f * calh) * (1.f / .45f)) *
                        ss01((adb_qconf - .5f) * (1.f / .3f));
            /* v4.9.9 washed-core full extension ("remove the highest
               caps"): the model claims this pixel sits DEEP on a
               plateau (|z0| > 2.2) yet the finished colour still
               floats > .18 away from the claimed level -- the local
               window never saw the true level (a washed stroke core),
               so on quantized sources extend to the source-global
               extremes outright.  Spatially coherent (the |z0|
               saturation zone is contiguous along the feature), and
               photos never enter: qq = 0 there, the global-injection
               fringe class stays locked out. */
            float fullsat =
                qq * ss01((fabsf(z0) - 2.2f) * (1.f / .8f)) *
                ss01((fabsf(m - ufit0) - .18f) * (1.f / .15f));
            float rcp = rmp > fullsat ? rmp : fullsat;
            /* evidence: fit trust w, or flank-pair proof w2v
               DISCOUNTED by how far the colour itself sits from the
               steepened model's verdict (frame-flipped camps have nu
               saturated against m; those keep the bare w). */
#ifndef CELUP_EVW2V
#define CELUP_EVW2V 1
#endif
            /* admission: fit trust w, plus flank-pair proof w2v
               where the steepened model's verdict nu and the
               measured colour fraction m DISAGREE -- the analytic
               signature of blur wash (a genuine gradient has m ~ nu,
               wash does not).  Frame-flip safety comes from the
               target, not the gate: the map pays to the pixel's OWN
               proven hull along the colour diagonal reached from its
               own colour -- a frame-flipped fit changes WHICH side
               the fit claims, never which side the colour is on. */
            float mis = ss01((fabsf(nu - m) - .10f) * (1.f / .30f));
            float evz = fmaxf(w, w2v * mis);
            /* v4.9.9e straggler channel: occasionally a whole
               vertical/horizontal strip of pixels carries HEAVY
               model-vs-colour mismatch while both trust channels
               read ~0 (the nearest-lobe framing is unstable along
               the exact XY-pole rows of circles: wS and the flank-
               pair restoration both fall silent there, and the map's
               own gate then leaves the wash in place as a gray seam
               cutting the ring -- the "additional blur at the poles"
               the user sees).  The signature is unambiguous on a
               quantized source: mismatch near full scale plus a hull
               that still spans the global step.  Let the colour side
               settle those too: the target is the same own proven
               hull, so snap-away is still by-decision-only.  Photos:
               qq = 0, unreachable. */
            if (qq > 1e-3f) {
              float alt = ss01((fabsf(nu - m) - .45f) *
                               (1.f / .30f)) *
                          qq * ss01((alh - .80f) * (1.f / .50f));
              if (alt > evz)
                evz = alt;
            }
            /* weight: real step span x corridor exit x evidence.
               The true-AA corridor (|mu| <= a) is kept ONLY for
               continuous sources: there the boundary column's value
               encodes exact sub-pixel feature width (crisp-GT check:
               repainting it costs MAE 13.2 -> 19).  On quantized
               (step-class) sources that column is quantizer residue,
               and its linear hull fraction (footprint-majority
               coverage) assigns it to its own side's proven
               plateau. */
            float wz = ss01((evz - .10f) * (1.f / .4f)) *
                       ss01((alh - .30f) * (1.f / .25f)) *
                       fmaxf(ss01((mmu - a) * (1.f / .7f)), qq);
            float elo3[3], ehi3[3];
            for (int c = 0; c < 3; c++) {
              elo3[c] = vblo[c] + rcp * (adb_srclo[c] - vblo[c]);
              ehi3[c] = vbhi[c] + rcp * (adb_srchi[c] - vbhi[c]);
            }
            /* v4.9.9 evidence broadening: inside deep wash the erf
               trust w is ~0 BY CONSTRUCTION (the wash is what the erf
               cannot fit), so gating the deblur's own payment by w
               alone starves exactly the stroke cores it exists for.
               The flank-pair restoration evidence w2v, which is
               proven there instead, is equally binding -- take the
               max.  Geometry still comes from the proven hulls, so
               the payment remains bounded by proven colours. */
            /* outside the corridor: value-space erf-gain map of the
               rendered colour (orientation-free: m is measured along
               the hull diagonal, so the pass-1 u-reversal cannot
               flip it); on quantized sources the gain is boosted to
               an effectively hard (but smooth) threshold. */
#ifndef CELUP_ZBOOST_DEF
#define CELUP_ZBOOST_DEF 4.f
#endif
            float zge = zg;
            {
              static float zb_env = -1.f;
              if (zb_env < 0.f) {
                const char *e = getenv("CELUP_ZBOOST");
                zb_env = e && *e ? (float)atof(e) : 0.f;
              }
              zge *= 1.f + qq * ((zb_env > 1.f ? zb_env
                                                : CELUP_ZBOOST_DEF) -
                                 1.f);
            }
            float mm = phi1(zge * probit01(m));
            zm_dbg = m;
            zmm_dbg = mm;
            zal_dbg = alh;
            wz_dbg = wz;
            zmw = zno ? 0.f : wz;
            zrmp = rcp;
            if (!zno && wz > 1e-3f) {
              /* cap removal: a strongly-firing gate pays in full --
                 the partial lerp used to leave 30-50% of the proven
                 deblur behind on exactly the washed pixels it exists
                 for. */
              /* cap removal, second pass: .30/.12 fullness -- the
                 .55/.20 band left .1-linear floor remnants (lum 80)
                 on exactly the pixels it half-paid.  Gates below
                 keep their evidence role; the payment is binary. */
              float wz2 =
                  wz > .30f ? 1.f : ss01((wz - .12f) * (1.f / .18f));
              for (int c = 0; c < 3; c++) {
                /* target = mm-fraction across the channel's own HULL
                   span (hull amplitude is proven; the LSQ d2
                   amplitude is not -- a washed flank under-reads it
                   ~2x), oriented per channel by the d2 sign (the
                   pass-1 u-reversal flips the axis: never index
                   plateaus as dark/bright). */
                float tgt = elo3[c] + mm * (ehi3[c] - elo3[c]);
                vv[c] =
                    clampf(vv[c] + wz2 * (tgt - vv[c]), elo3[c],
                           ehi3[c]);
              }
            }
          }
        }
        for (int c = 0; c < 4; c++)
          dd[c] += vv[c] - o[c];
        /* The map owns this pixel's colour now: (a) pass 2's final
           hull clamp must not raise the washed floor back onto it --
           extend LOH toward the global range by the same rmp so the
           later clamp agrees with the map's span; (b) record the map
           weight so pass 2's staircase cleanup (which lerps the
           pixel back toward the WASHED base average) and the
           tangential delta smoothing stand down exactly where the
           map fired. */
        if (zmw > 1e-3f) {
          ZM[idx] = zmw;
          if (zrmp > 1e-3f)
            for (int c = 0; c < 3; c++) {
              float elo = vblo[c] + zrmp * (adb_srclo[c] - vblo[c]),
                    ehi = vbhi[c] + zrmp * (adb_srchi[c] - vbhi[c]);
              int lv = to_srgb[clampi((int)(clampf(elo, 0.f, 1.f) *
                                            4096.f),
                                      0, 4096)] -
                       1,
                  hv = to_srgb[clampi((int)(clampf(ehi, 0.f, 1.f) *
                                            4096.f),
                                      0, 4096)] +
                       1;
              if (lv < LOH[8 * idx + c])
                LOH[8 * idx + c] = (uint8_t)clampi(lv, 0, 255);
              if (hv > LOH[8 * idx + 4 + c])
                LOH[8 * idx + 4 + c] = (uint8_t)clampi(hv, 0, 255);
            }
        }
        {
          static FILE *zm = NULL;
          static int zm_on = -1;
          if (zm_on < 0) {
            const char *e = getenv("CELUP_ZDBG");
            zm_on = e && *e;
            if (zm_on)
              zm = fopen(e, "w");
          }
          if (zm)
            fprintf(zm,
                    "%d %d %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f "
                    "%.5f %.5f %.5f\n",
                    x, y, w, w2v, nu, ufit0, wpeel, zal_dbg, zm_dbg,
                    zmm_dbg, wz_dbg, o[0], vv[0]);
        }
        {
          static FILE *wm = NULL;
          static int wm_on = -1;
          if (wm_on < 0) {
            const char *e = getenv("CELUP_WMAP");
            wm_on = e && *e;
            if (wm_on)
              wm = fopen(e, "w");
          }
          if (wm) {
            float pabs_d =
                aWL > 1e-6f
                    ? sqrtf(aVX / aWL * (aVX / aWL) +
                            aVY / aWL * (aVY / aWL))
                    : 0.f;
            fprintf(wm, "%d %d %.5f %.5f %.5f %.5f %.5f %.4f %.4f\n",
                    x, y, w, w2v, nu, ufit0, wpeel, pabs_d);
          }
        }
        if (dbg && y == dbg_y && abs(x - dbg_x) <= 16 && (x & 1) == 0)
          fprintf(stderr,
                  "DBGS %d,%d mu=%.3f s=%.3f k=%.3f ufit=%.4f nu=%.4f "
                  "wSeff=%.4f w2=%.4f inn=%.3f gI=%.2f o0e=%.4f "
                  "o1e=%.4f d2=%.4f v0=%.4f blo=%.4f (cons)\n",
                  x, y, mu, s, k, ufit0, nu, w, w2v,
                  aW2 > 1e-6f ? aR / aW2 : -.5f, gInn, off0e[0],
                  off1e[0], d2[0],
                  o[0] + w * ((nu - ufit0) * d2[0]) +
                      w2v * gInn * (uf0 * (1.f - nu) * off0e[0] +
                                    uf1 * nu * off1e[0]) +
                      w2v * gInn * uft *
                          (off0e[0] * off1e[0] > 0.f
                               ? (fabsf(off0e[0]) < fabsf(off1e[0])
                                      ? off0e[0]
                                      : off1e[0])
                               : 0.f),
                  c0blo_dbg);
      }
    }
  /* Pass 2: tangential smoothing of the residual delta.  Fit jitter is
     already absorbed by the pass-1.5 consensus; this only polishes the
     small delta noise of the flat-flatten path and any rounding of the
     consensus evaluation.  Junction-aware taps (v4.9): a corner's delta
     is not exchangeable with its contour neighbours' -- tangents rotate
     there. */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      const float *pf = PF + 25 * idx;
      float cc = pf[7];
      float acc[4] = {0, 0, 0, 0}, o[4], res[4], wsum = 0.f;
      for (int to = -T; to <= T; to++) {
        float wt = (float)(T + 1 - (to < 0 ? -to : to)), q[4], qc[25];
        float sx = (float)x + (float)to * pf[8],
              sy = (float)y + (float)to * pf[9];
        sample_fn(PF, dw, dh, 25, sx, sy, qc);
        wt *= sqrtf(cc * fmaxf(qc[7], 0.f));
        sample_f4(DEL, dw, dh, sx, sy, q);
        for (int c = 0; c < 4; c++)
          acc[c] += wt * q[c];
        wsum += wt;
      }
      /* v4.9.3 quant-staircase cleanup: on trusted ramp zones of a
         hard-quantized source, smooth ALONG THE NORMAL (terraces run
         parallel to the contour, so tangential smoothing cannot reach
         them).  A [1,2,3,2,1]/9 average over +-2 normal offsets of the
         RESTORED image (A + DEL, not the washed base) kills 3-6 px
         terraces; it is DC-exact on linear shading, and the fitted
         contour itself was already applied as the DELTA.  Blend weight
         = qconf * wS so unfitted texture/flats stay verbatim, and the
         hull clamp below still bounds the result. */
      float navg[4] = {0, 0, 0, 0}, qn = 0.f;
      if (!adb_noter && adb_qconf > 1e-3f && pf[0] > 1e-3f && wsum > 1e-6f) {
        /* Skirt gate: terraces live on the ramp skirts (between the
           quant steps); near the contour core (|z0| < ~1) smoothing
           along the normal would bleed the corrected stroke colour
           across the contour (the neon band regression).  Only the
           skirt/plateau zones get the normal cleanup. */
        float zmu = pf[1] / pf[0], zs = pf[2] / pf[0];
        float z0 = fabsf((0.f - zmu) / (zs > .3f ? zs : .3f));
        float skirt = ss01((z0 - 1.0f) * (1.f / 1.2f));
        float wx = -pf[9], wy = pf[8], tw2 = 0.f;
        static const float nk[5] = {1.f, 2.f, 3.f, 2.f, 1.f};
        for (int no = -2; no <= 2; no++) {
          float q[4];
          sample_f4(A, dw, dh, (float)x + (float)no * wx,
                    (float)y + (float)no * wy, q);
          for (int c = 0; c < 4; c++)
            navg[c] += nk[no + 2] * q[c];
          tw2 += nk[no + 2];
        }
        float inv = tw2 > 1e-6f ? 1.f / tw2 : 0.f;
        qn = adb_qconf * pf[0] * skirt * .7f *
             (1.f - ss01((ZM[idx] - .05f) * (1.f / .25f)));
        for (int c = 0; c < 4; c++)
          navg[c] = qn * (navg[c] * inv - A[4 * idx + c]);
      }
      float zmq = ZM[idx];
      for (int c = 0; c < 4; c++) {
        o[c] = A[4 * idx + c];
        float v = wsum > 1e-6f ? o[c] + acc[c] / wsum : o[c];
        /* v4.9.8: where the erf-gain map owns this pixel, keep its
           own (mapped) delta instead of the tangentially averaged
           one -- the average mixes the snapped colour with its
           unmapped neighbours' wash and re-introduces the mid band
           the map just removed. */
        if (zmq > 1e-3f && wsum > 1e-6f)
          v += zmq * .85f * (DEL[4 * idx + c] - acc[c] / wsum);
        v += navg[c];
        /* Final hull clamp (v4.9): pass-1.5 clamps each pixel's own
           evaluation, but the smoothed delta can still leave the hull
           by transport.  No output may leave the locally observed
           colour range -- enforced here by construction. */
        float blo = adb_nohull ? 0.f : (c < 3 ? to_linear[LOH[8 * idx + c]]
                          : LOH[8 * idx + c] * (1.f / 255.f)),
              bhi = adb_nohull ? 1.f : (c < 3 ? to_linear[LOH[8 * idx + 4 + c]]
                          : LOH[8 * idx + 4 + c] * (1.f / 255.f));
        /* v4.9.4 soft hull: expand by deblur confidence pf[0] and qconf gate. */
        {
          float hexpand = fmaxf(pf[0], 0.f) * adb_qconf * .55f;
          float slo = c < 3 ? adb_srclo[c] : 0.f,
                shi = c < 3 ? adb_srchi[c] : 1.f;
          blo -= hexpand * fmaxf(blo - slo, 0.f);
          bhi += hexpand * fmaxf(shi - bhi, 0.f);
        }
        res[c] = clampf(v, blo, bhi);
      }
      put(dst + 4 * idx, res[0], res[1], res[2], res[3]);
    }
  memcpy(out, dst, n * 4);
  free(dst);
  free(LOH);
  free(PF);
  free(DEL);
  free(A);
  free(ZM);
  return 1;
}

/* ---------------------------------------------------------------------------
   autodeblur method 3 (ANALOG): a full-gradient analytical deblur.

   The model: reconstruct image as gradients of 2 4-channel colours; push
   each pair of starts and ends of gradients towards each other to increase
   steepness.  There is no per-pixel gate anywhere: the WHOLE-image gradient
   is produced, edited analytically, and the image is re-sampled from the
   edited gradient.

   Pipeline (all at target resolution on the premultiplied-linear base A):
   1. PRODUCE the full image gradient: compute the global colour axis as the
      dominant 4-D eigenvector (power iteration) of the colour covariance of
      A, and project every pixel onto it:  p = (A-mean) . axis.
   2. Reconstruct the scalar level field u = normalized p over the image.
      For a 2-colour source u is exactly the blend coordinate (0 on one
      plateau, 1 on the other) and its 0.5 isophote is the geometric edge
      centre.
   3. EDIT the gradient (the "push"): apply the monotone power-sigmoid
      remap  up = R(u) = u^g / (u^g + (1-u)^g).  R pushes the start (u=0)
      and end (u=1) of every transition toward each other: steepness grows
      with g, and at the limit g -> inf R is a hard threshold that
      QUANTISES the image to its two local colours.  The deblur value is
      INVERTED for this method: v = -g, q = 1/v, g = 1/(1-q).  So v=1
      (q=1) is the MAXIMUM push; v>1 (q<1) is WEAKER; 0 is auto.
   4. SAMPLE from the edited gradient:  out = A + (up - u) * span * axis.
      Because f = mean + p*axis along the axis, this equals
      mean + (pmin + up*span)*axis: the along-axis colour component is
      exactly remapped by R(u) (so plateaus land on the two source colours
      and quantise at v=1), and every perpendicular colour component passes
      through UNCHANGED (hue texture, alpha, off-axis detail kept).  No
      Poisson ring, no overshoot: (up-u) in [-1,1] and span is the measured
      axis range, so the output cannot leave the source's own colour range
      along the axis for a 2-colour source.
--------------------------------------------------------------------------- */

static float powersigmoid_float(float u, float g) {
  /* R(u) = 1 / (1 + exp(g * ln((1-u)/u))); stable for large g. */
  if (g <= 1.0001f)
    return u;
  if (u <= 1e-9f)
    return 0.f;
  if (u >= 1.f - 1e-9f)
    return 1.f;
  float e = g * logf((1.f - u) / u);
  if (e > 80.f)
    return 0.f;
  if (e < -80.f)
    return 1.f;
  return 1.f / (1.f + expf(e));
}

/* autodeblur_gradient_pass -- the "analytical" deblur (method 3).

   A second, deliberately different deblur that does NOT use the per-pixel
   local-window erf fit + trust gates of autodeblur_pass.  It follows the
   analytic prescription literally: PRODUCE a full-image gradient field,
   EDIT it, then SAMPLE from it.

   1. PRODUCE the full-image gradient field.  The autoblur base is loaded into
      a premultiplied-linear RGBA field A; a 4D structure-tensor pass gives
      every pixel its gradient normal (nx,ny) and magnitude MAG (the full-image
      gradient).  For each pixel the colour TRANSITION (the lobe) it sits in is
      then traced ALONG ITS NORMAL across its whole extent -- an adaptive walk
      that keeps going until the colour saturates to a plateau on each side and
      stops there (its length is set by the assumed blur, NOT a fixed 2x2/6x6
      tap, so a wide blurred ramp is traced end to end while a crisp one stops
      after a couple of samples).  The lobe is thus reconstructed as a gradient
      between TWO 4-channel colours P0, P1 (the farthest pair found along the
      trace), and the pixel records its blend coordinate u in [0,1] on the
      P0->P1 segment.

   2. EDIT the gradient field.  Push the start (u=0, P0) and end (u=1, P1) of
      every gradient towards each other: a retention alpha in (0,1] narrows
      each transition by
                      u' = clamp(0.5 + (u - 0.5) / alpha, 0, 1).
      Mid-ramp pixels are driven to the nearer plateau -> the ramp steepens;
      at alpha -> 0 the whole lobe collapses to one point and the colour
      quantizes to P0 or P1.  The remap is symmetric about u=0.5, so it is
      independent of which endpoint is P0 -- only the steepness changes.

   3. SAMPLE from the edited gradient: out = P0 + u' * (P1 - P0), blended in by
      a contrast weight so genuine flats (|P1-P0| ~ 0) are left untouched.
      The output is a convex combination of two REAL sampled colours, so it
      never leaves their hull -- no ringing vocabulary (invariant #1), no hue
      inversion, premultiplied-safe.

   Strength semantics differ from remap/push: here 1 is the MAXIMUM (collapse
   to one point = quantize) and larger values are LESS deblur, so
   alpha = (K-1)/K for K >= 1 (K=1 -> alpha 0 -> quantize; K->inf -> alpha 1 ->
   identity).  K=0 means auto.  (0 is auto because the same -g knob is shared
   with remap/push, where 0 also means auto.) */
static int autodeblur_gradient_pass(uint8_t *out, int dw, int dh, float scale) {
  size_t n = (size_t)dw * dh;
  float *A = malloc(n * 4 * sizeof *A);   /* premultiplied-linear RGBA field */
  float *NX = malloc(n * sizeof *NX);     /* gradient normal x              */
  float *NY = malloc(n * sizeof *NY);     /* gradient normal y              */
  float *MAG = malloc(n * sizeof *MAG);   /* |grad I| (the full-image grad) */
  float *SEG = malloc(n * 8 * sizeof *SEG); /* P0[4], P1[4] per pixel       */
  float *U = malloc(n * sizeof *U);       /* blend coordinate in [0,1]      */
  float *WGT = malloc(n * sizeof *WGT);   /* lobe validity (0=flat/no grad) */
  uint8_t *dst = malloc(n * 4);
  if (!A || !NX || !NY || !MAG || !SEG || !U || !WGT || !dst) {
    free(A);
    free(NX);
    free(NY);
    free(MAG);
    free(SEG);
    free(U);
    free(WGT);
    free(dst);
    return 0;
  }

  /* Load the autoblur base render into a linear premultiplied field. */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      float q[4];
      raw_pm(out, dw, dh, x, y, q);
      size_t k = 4 * ((size_t)y * dw + x);
      A[k] = q[0];
      A[k + 1] = q[1];
      A[k + 2] = q[2];
      A[k + 3] = q[3];
    }

  /* --- PHASE 1a: full-image gradient + structure-tensor normal (one pass) */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      int xl = x > 0 ? x - 1 : 0, xr = x + 1 < dw ? x + 1 : dw - 1,
          yu = y > 0 ? y - 1 : 0, yd = y + 1 < dh ? y + 1 : dh - 1;
      float Jxx = 0.f, Jxy = 0.f, Jyy = 0.f;
      for (int c = 0; c < 4; c++) {
        float gx = A[4 * ((size_t)yu * dw + xr) + c] +
                   2 * A[4 * ((size_t)y * dw + xr) + c] +
                   A[4 * ((size_t)yd * dw + xr) + c] -
                   A[4 * ((size_t)yu * dw + xl) + c] -
                   2 * A[4 * ((size_t)y * dw + xl) + c] -
                   A[4 * ((size_t)yd * dw + xl) + c],
              gy = A[4 * ((size_t)yd * dw + xl) + c] +
                   2 * A[4 * ((size_t)yd * dw + x) + c] +
                   A[4 * ((size_t)yd * dw + xr) + c] -
                   A[4 * ((size_t)yu * dw + xl) + c] -
                   2 * A[4 * ((size_t)yu * dw + x) + c] -
                   A[4 * ((size_t)yu * dw + xr) + c];
        gx *= 1.f / 16.f;
        gy *= 1.f / 16.f;
        Jxx += gx * gx;
        Jxy += gx * gy;
        Jyy += gy * gy;
      }
      float trh = .5f * (Jxx - Jyy), root = sqrtf(trh * trh + Jxy * Jxy),
            lam = .5f * (Jxx + Jyy) + root;
      MAG[idx] = lam; /* gradient magnitude ~= top eigenvalue of the tensor */
      float nx = 1.f, ny = 0.f;
      if (lam > 1e-12f) {
        float vx = Jxy, vy = lam - Jxx;
        if (vx * vx + vy * vy < 1e-18f) {
          vx = lam - Jyy;
          vy = Jxy;
        }
        float vl = vx * vx + vy * vy;
        if (vl > 1e-18f) {
          float inv = 1.f / sqrtf(vl);
          nx = vx * inv;
          ny = vy * inv;
        }
      }
      NX[idx] = nx;
      NY[idx] = ny;
    }

  /* The assumed blur sizes the trace length so it spans a whole lobe end to
     end (global), not a fixed 2x2/6x6 tap.  Capped; early termination keeps
     crisp ramps and flats cheap. */
  float sref0 = adb_assumed_sigma > 0.f ? adb_assumed_sigma : fitted_sigma;
  float sref = sref0 > 1.f ? sref0 : 1.f;
  int maxR = clampi((int)(3.5f * scale * sref + 6.f), 8, 64);
  const float moveThresh = 1.0e-3f; /* per-step plateau detection threshold */
  int dbg = 0, dbg_x = 96, dbg_y = 96;
  {
    const char *e = getenv("CELUP_DBG");
    if (e) {
      dbg = 1;
      sscanf(e, "%d,%d", &dbg_x, &dbg_y);
    }
  }

  /* --- PHASE 1b: trace each pixel's lobe along its normal (global) ---
     Walk +-normal until the colour saturates to a plateau on each side (an
     adaptive, potentially many-pixel walk -- NOT a fixed 2x2/6x6 tap), and
     read the two plateau colours straight off the saturated tails.  The walk
     is global: a wide blurred ramp is traced end to end so even its tail
     pixels see both plateaus and get narrowed, while a crisp ramp stops after
     a couple of samples (early termination).  Every pixel is traced -- the
     early-termination keeps flats cheap, and the lobe's OWN proven contrast
     decides validity (no per-pixel magnitude skip, no 2x2/6x6 confidence). */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      float nx = NX[idx], ny = NY[idx];
      float *P0 = SEG + 8 * idx, *P1 = P0 + 4;
      U[idx] = 0.f;
      WGT[idx] = 0.f;
      float c0[4];
      sample_f4(A, dw, dh, (float)x, (float)y, c0);
      /* Walk each side; the plateau colour is the mean of the saturated tail
         (>= STAB consecutive sub-threshold steps), or the farthest sample
         reached if the lobe is wider than maxR. */
      float plat[2][4];
      for (int s = 0; s < 2; s++) {
        float dir = s ? -1.f : 1.f;
        float prev[4], lastc[4];
        memcpy(prev, c0, sizeof prev);
        memcpy(lastc, c0, sizeof lastc);
        float sum[4] = {0, 0, 0, 0};
        int ns = 0, stab = 0;
        for (int t = 1; t <= maxR; t++) {
          float C[4];
          sample_f4(A, dw, dh, (float)x + dir * (float)t * nx,
                    (float)y + dir * (float)t * ny, C);
          memcpy(lastc, C, sizeof lastc);
          if (dist4_pm(C, prev) > moveThresh) { /* still transitioning */
            stab = 0;
            ns = 0;
            for (int c = 0; c < 4; c++)
              sum[c] = 0.f;
          } else { /* saturated: accumulate the plateau tail */
            stab++;
            for (int c = 0; c < 4; c++)
              sum[c] += C[c];
            ns++;
          }
          memcpy(prev, C, sizeof prev);
          if (stab >= 3)
            break; /* plateau confirmed on this side */
        }
        if (ns >= 2)
          for (int c = 0; c < 4; c++)
            plat[s][c] = sum[c] / (float)ns;
        else
          memcpy(plat[s], lastc, sizeof plat[s]);
      }
      for (int c = 0; c < 4; c++) {
        P0[c] = plat[0][c];
        P1[c] = plat[1][c];
      }
      float L2 = dist4_pm(P0, P1); /* |P1 - P0|^2 */
      if (L2 < 6.4e-5f) { /* no real transition -> inert (flat) */
        for (int c = 0; c < 4; c++)
          P0[c] = P1[c] = A[4 * idx + c];
        continue;
      }
      /* Blend coordinate of THIS pixel on the P0->P1 segment. */
      float dot = 0.f;
      for (int c = 0; c < 4; c++)
        dot += (c0[c] - P0[c]) * (P1[c] - P0[c]);
      U[idx] = clampf(dot / L2, 0.f, 1.f);
      /* Validity: the lobe's own proven contrast (no 2x2/6x6 confidence). */
      WGT[idx] = ramp01(L2, 4e-4f, 3e-2f);
      if (dbg && y == dbg_y && abs(x - dbg_x) <= 16 && (x & 3) == 0)
        fprintf(stderr,
                "DBGA %d,%d maxR=%d L2=%.5f u=%.3f wgt=%.3f P0=%.3f P1=%.3f "
                "mag=%.5f\n",
                x, y, maxR, L2, U[idx], WGT[idx], P0[0], P1[0], MAG[idx]);
    }

  /* --- PHASE 2 + 3: edit (narrow) + sample ---
     K semantics: 0 = auto, 1 = max (collapse to a point = quantize), larger =
     less deblur.  alpha = (K-1)/K is the transition retention. */
  float K;
  if (deblur_steepness > 0.f)
    K = deblur_steepness;
  else
    K = clampf(2.2f - 0.012f * (compress_strength - 1.f), 1.05f, 2.5f);
  last_deblur_k = K;
  float alpha = K <= 1.0001f ? 0.f : (K - 1.f) / K;
  fprintf(stderr,
          "autodeblur analytic K=%.3f (1=max/quantize, higher=less) "
          "alpha=%.3f maxR=%d\n",
          K, alpha, maxR);
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t idx = (size_t)y * dw + x;
      const float *P0 = SEG + 8 * idx;
      const float *P1 = P0 + 4;
      float o[4];
      for (int c = 0; c < 4; c++)
        o[c] = A[4 * idx + c];
      float w = WGT[idx];
      if (w <= 1e-4f) { /* flat / no proven gradient -> keep base colour */
        put(dst + 4 * idx, o[0], o[1], o[2], o[3]);
        continue;
      }
      float u = U[idx], up;
      if (alpha <= 1e-4f) /* K = 1: collapse the gradient to one point */
        up = u < 0.5f ? 0.f : (u > 0.5f ? 1.f : 0.5f);
      else
        up = clampf(0.5f + (u - 0.5f) / alpha, 0.f, 1.f);
      float v[4];
      for (int c = 0; c < 4; c++)
        v[c] = o[c] + w * (P0[c] + up * (P1[c] - P0[c]) - o[c]);
      put(dst + 4 * idx, v[0], v[1], v[2], v[3]);
    }
  memcpy(out, dst, n * 4);
  free(dst);
  free(WGT);
  free(U);
  free(SEG);
  free(MAG);
  free(NY);
  free(NX);
  free(A);
  return 1;
}

static int autodeblur_analog(uint8_t *out, int dw, int dh) {
  size_t n = (size_t)dw * dh;
  float *A = malloc(n * 4 * sizeof *A);
  uint8_t *dst = malloc(n * 4);
  if (!A || !dst) {
    free(A);
    free(dst);
    return 0;
  }
  /* 1. read the base render as premultiplied-linear floats. */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      float q[4];
      raw_pm(out, dw, dh, x, y, q);
      for (int c = 0; c < 4; c++)
        A[4 * ((size_t)y * dw + x) + c] = q[c];
    }
  /* 2. colour covariance -> dominant 4-D axis (power iteration). */
  double mean[4] = {0, 0, 0, 0};
  for (size_t k = 0; k < n; k++)
    for (int c = 0; c < 4; c++)
      mean[c] += A[4 * k + c];
  for (int c = 0; c < 4; c++)
    mean[c] /= (double)n;
  double cov[4][4];
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      cov[r][c] = 0.0;
  for (size_t k = 0; k < n; k++) {
    double d[4];
    for (int c = 0; c < 4; c++)
      d[c] = A[4 * k + c] - mean[c];
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        cov[r][c] += d[r] * d[c];
  }
  double axis[4] = {1, 0, 0, 0}, anorm;
  for (int it = 0; it < 16; it++) {
    double v[4] = {0, 0, 0, 0}, mx = 0;
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++)
        v[r] += cov[r][c] * axis[c];
      if (fabs(v[r]) > mx)
        mx = fabs(v[r]);
    }
    if (mx < 1e-300)
      break;
    for (int c = 0; c < 4; c++)
      axis[c] = v[c] / mx;
  }
  anorm = 0.0;
  for (int c = 0; c < 4; c++)
    anorm += axis[c] * axis[c];
  if (anorm < 1e-12) {
    axis[0] = 1.f;
    axis[1] = axis[2] = axis[3] = 0.f;
    anorm = 1.0;
  } else
    anorm = sqrt(anorm);
  for (int c = 0; c < 4; c++)
    axis[c] /= anorm;
  /* 3. projection p and its range. */
  double pmin = 1e300, pmax = -1e300;
  for (size_t k = 0; k < n; k++) {
    double p = 0.0;
    for (int c = 0; c < 4; c++)
      p += (A[4 * k + c] - mean[c]) * axis[c];
    if (p < pmin)
      pmin = p;
    if (p > pmax)
      pmax = p;
  }
  double span = (pmax > pmin) ? (pmax - pmin) : 1e-9;
  /* Global per-channel hull of the base (its plateaus reach the source's
     two colours): a safe, non-per-pixel clamp so the re-render can never
     leave the source's own per-channel range (no new colours/halo). */
  float glo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, ghi[4] = {-1e30f, -1e30f,
                                                          -1e30f, -1e30f};
  for (size_t k = 0; k < n; k++)
    for (int c = 0; c < 4; c++) {
      if (A[4 * k + c] < glo[c])
        glo[c] = A[4 * k + c];
      if (A[4 * k + c] > ghi[c])
        ghi[c] = A[4 * k + c];
    }
  /* 4. deblur value: v = -g; q = 1/v; g = 1/(1-q).  v=1 is the max push. */
  float qv;
  if (deblur_steepness > 0.f)
    qv = 1.f / deblur_steepness; /* -g 1 -> max, higher -> weaker */
  else
    qv = 0.85f; /* auto: a strong-but-not-quantising default */
  if (qv > 0.99999f)
    qv = 0.99999f;
  if (qv < 0.f)
    qv = 0.f;
  float g = 1.f / (1.f - qv);
  last_deblur_k = deblur_steepness > 0.f ? deblur_steepness : -1.f;
  /* 5. re-render from the edited gradient. */
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      size_t k = (size_t)y * dw + x;
      double p = 0.0;
      for (int c = 0; c < 4; c++)
        p += (A[4 * k + c] - mean[c]) * axis[c];
      float u = clampf((float)((p - pmin) / span), 0.f, 1.f);
      float up = powersigmoid_float(u, g);
      float res[4];
      for (int c = 0; c < 4; c++) {
        res[c] = A[4 * k + c] + (up - u) * (float)span * (float)axis[c];
        res[c] = clampf(res[c], glo[c], ghi[c]);
        res[c] = clampf(res[c], 0.f, 1.f);
      }
      put(dst + 4 * k, res[0], res[1], res[2], res[3]);
    }
  memcpy(out, dst, n * 4);
  free(dst);
  free(A);
  return 1;
}

/* Quantization staircase control (v4.9.3): hard pixelated sources
   (the smiley class) carry ~45-90 quantization levels; heavy base blur
   turns each level into a soft terrace, and ramp steepening re-crisp
   the terrace edges into visible bands ("staircase of not fully
   smoothed pixels").  qconf measures how quantized the source is
   (0 = natural continuous, 1 = hard few-level pixel art) and gates a
   normal-direction profile smoothing inside trusted ramp windows:
   terraces are monotone-ramp noise, texture zones have no ramp fit
   and stay untouched. */
static float estimate_qconf(const uint8_t *in, int sw, int sh) {
  uint8_t seen[3][256];
  memset(seen, 0, sizeof seen);
  long step = (long)sw * sh / 200000 + 1;
  for (long k = 0; k < (long)sw * sh; k += step) {
    const uint8_t *p = in + 4 * k;
    for (int c = 0; c < 3; c++)
      seen[c][p[c]] = 1;
  }
  int levels = 0;
  for (int c = 0; c < 3; c++) {
    int n = 0;
    for (int v = 0; v < 256; v++)
      n += seen[c][v];
    if (n > levels)
      levels = n;
  }
  return ramp01((160.f - (float)levels) * (1.f / 96.f), 0.f, 1.f);
}

static int upscale_autodeblur(const uint8_t *in, int sw, int sh, uint8_t *out,
                              int dw, int dh) {
  /* v4.9.1: the v4.9 base-sigma decouple is REVERTED. */
  adb_sigma_div = 0.f;
  adb_assumed_sigma = 0.f;
  adb_qconf = estimate_qconf(in, sw, sh);
  if (adb_qconf > 1e-3f)
    fprintf(stderr,
            "source quantization: qconf=%.2f (hard-quantized; terrace "
            "smoothing engaged on trusted ramps)\n",
            adb_qconf);
  for (int c = 0; c < 3; c++) {
    adb_srclo[c] = 1e30f;
    adb_srchi[c] = -1e30f;
  }
  for (long k = 0; k < (long)sw * sh; k++) {
    float q[4];
    raw_pm(in, sw, sh, (int)(k % sw), (int)(k / sw), q);
    for (int c = 0; c < 3; c++) {
      if (q[c] < adb_srclo[c])
        adb_srclo[c] = q[c];
      if (q[c] > adb_srchi[c])
        adb_srchi[c] = q[c];
    }
  }
  if (!upscale_autoblur(in, sw, sh, out, dw, dh))
    return 0;
  int method = deblur_method;
  if (method)
    fprintf(stderr, "autodeblur method %s (manual)\n",
            method == 1 ? "remap" : "push");
  if (!method) {
    /* Auto-choice over the implemented deblur methods with the same
       self-supervised 2x-downscale proxy the blur fit uses: whichever
       method reconstructs the source best at 2x wins for the full run. */
    int tw = sw / 2, th = sh / 2;
    method = 1;
    if (tw >= 8 && th >= 8) {
      uint8_t *train = downsample_pm_box(in, sw, sh, tw, th);
      uint8_t *recon = malloc((size_t)sw * sh * 4);
      double bs = 1e300;
      if (train && recon)
        for (int m = 1; m <= 2; m++) {
          if (!render_soft(train, tw, th, recon, sw, sh, fitted_kernel,
                           fitted_sigma, fitted_curve, fitted_cp))
            continue;
          if (!autodeblur_pass(recon, sw, sh, (float)sw / tw, m))
            continue;
          double s = image_pm_mse(recon, in, sw, sh, 2);
          fprintf(stderr, "autodeblur method %s proxy MSE %.8g\n",
                  m == 1 ? "remap" : "push", s);
          if (s < bs) {
            bs = s;
            method = m;
          }
        }
      free(train);
      free(recon);
      fprintf(stderr, "autodeblur auto-selected %s\n",
              method == 1 ? "remap" : "push");
    }
  }
  last_deblur_method = method;
  if (method == 3)
    return autodeblur_analog(out, dw, dh);
  if (method == 4)
    return autodeblur_gradient_pass(out, dw, dh, (float)dw / sw);
  return autodeblur_pass(out, dw, dh, (float)dw / sw, method);
}

/* ---------------------------------------------------------------------------
   sdf (v4): signed-distance-field edge reconstruction.

   Every confident edge pixel of the class map fits a two-colour patch with a
   t-plane (blend coordinate as a linear function of space).  From each such
   fit we extract the sub-pixel position where the plane crosses t=0.5 -- a
   point on the edge's mid-contour -- the local ramp width 1/|grad t|, and
   the two premultiplied endpoint colours.  A two-pass vectorial distance
   transform then gives every source pixel a *signed* distance to the nearest
   mid-contour (sign = which side its seed pixel was on).

   The signed distance, width, endpoints and seed confidence are upsampled at
   the target resolution (bounded Mitchell).  The output colour is the
   bounded-Mitchell base plus a bounded delta that re-thresholds the edge:

     t'      = smoothstep(clamp(.5 + d' / w'_eff, 0, 1))     (SDF iso-crossing)
     t_base  = projection of the base sample onto A'->B'
     out     = base + conf' * (t' - t_base) * (B' - A')

   Because the delta is a *difference of threshold coordinates*, it
   self-annihilates everywhere the SDF is unreliable: flat regions (both t
   saturated on the same side), junctions/lines/checker pixels (no seeds,
   conf'=0), and far from any contour (confidence falls off as a Gaussian of
   the distance).  The reconstruction can only sharpen WHERE an edge was
   actually measured, along a smooth contour that cannot staircase; the only
   band it alters is the edge's own ramp, so it cannot ring either.
--------------------------------------------------------------------------- */

/* (v4.1: fitted plane-weighted fields are smooth by construction, so
   bilinear upsampling suffices; v4.0's Mitchell field sampler is gone.) */

/* Bilinear sampler for the signed-distance/width/confidence channels:
   Mitchell's negative lobes ring a signed distance field (the zero-crossing
   wobbles), and the wobble renders as edge weave. */
static void field_bilinear_sample(const float *f, int sw, int sh, int nch,
                                  float sx, float sy, float *q) {
  int ix = (int)floorf(sx), iy = (int)floorf(sy);
  float fx = sx - (float)ix, fy = sy - (float)iy;
  for (int c = 0; c < nch; c++)
    q[c] = 0.f;
  for (int j = 0; j < 2; j++)
    for (int i = 0; i < 2; i++) {
      const float *p =
          f + (size_t)nch * ((size_t)clampi(iy + j, 0, sh - 1) * sw +
                             clampi(ix + i, 0, sw - 1));
      float w = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
      for (int c = 0; c < nch; c++)
        q[c] += w * p[c];
    }
}

static int upscale_sdf(const uint8_t *in, int sw, int sh, uint8_t *out, int dw,
                       int dh) {
  class_map_t cm;
  if (!build_class_map(in, sw, sh, &cm))
    return 0;
  size_t n = (size_t)sw * sh;
  float *fld = calloc(n * 11, sizeof *fld); /* d, width, A4, B4, conf */
  float *accw = calloc(n, sizeof *accw);    /* total splat weight         */
  if (!fld || !accw) {
    free(fld);
    free(accw);
    free_class_map(&cm);
    return 0;
  }
  /* SDF v2: the signed-distance field is *fitted*, not chamfered.  Each
     confident coherent-edge pixel's t-plane defines a signed-distance plane
     d_k(p) = (t0_k - .5 + g_k . (p - k)) / |g_k| oriented by its gradient.
     v1 seeded the plane's own zero crossing into a distance transform and
     gathered fields from the single nearest seed: Voronoi wins alternated
     per source row, which rippled d (washboard banding in saturation
     zones), and near-ridge sign logic put phantom contours down thin
     features.  Instead, splat every plane into its 2-sigma neighbourhood
     and accumulate a weighted mean of ALL candidate planes: d, the
     premultiplied endpoint colours, ramp width and confidence are all
     kernel-weighted averages, so the field is C1-smooth by construction,
     endpoint quilts cannot form, and the contour is the consensus of every
     stair segment's fit. */
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      size_t k = (size_t)y * sw + x;
      /* Checker/Nyquist ambiguity suppresses seeding outright: re-fitting
         geometry there invents structure the class policy says we cannot
         know. */
      float ck = cm.w_edge[k] * (1.f - cm.w_checker[k]);
      float gx = cm.edge_gx[k], gy = cm.edge_gy[k];
      float g2 = gx * gx + gy * gy;
      if (ck <= .35f || g2 < .12f * .12f || g2 > 1.4f * 1.4f)
        continue;
      float g = sqrtf(g2);
      /* De-dilute the ramp width.  The classifier's t-plane gradient comes
         from a least-squares fit over a fixed 5x5 window, which saturates
         for true AA ramps narrower than ~1.6 src px (any such ramp measures
         slope ~0.3 -> width 3.3) and dilutes middle spans.  Rendering with
         the LS width spreads the re-threshold band several px into what
         should be saturated flat colour, where the huge fitted colour axis
         turns small t errors into visible stains.  Invert the dilution
         empirically (bias narrow: over-width stains, under-width merely
         sharpens). */
      float w;
      if (g >= .295f)
        w = 1.2f;
      else {
        float wl = 1.f / g;
        w = wl < 4.3f ? wl - 2.1f : wl;
      }
      w = clampf(w, .6f, 5.f);
      float invg = 1.f / g;
      float t0 = cm.edge_t0[k];
      const float *A = cm.edge_side + 8 * k;
      for (int j = -4; j <= 4; j++)
        for (int i = -4; i <= 4; i++) {
          int px = x + i, py = y + j;
          if (px < 0 || py < 0 || px >= sw || py >= sh)
            continue;
          float r2 = (float)(i * i + j * j);
          if (r2 > 16.f)
            continue;
          float t = t0 + gx * (float)i + gy * (float)j;
          float d = (t - .5f) * invg;
          /* Trust the plane only near its own ramp: a short segment's fit
             must not ghost-extend its contour across empty space. */
          if (fabsf(d) > 2.6f)
            continue;
          float Kw = ck * expf(-r2 / (2.f * 1.5f * 1.5f));
          size_t kp = (size_t)py * sw + px;
          float *f = fld + 11 * kp;
          f[0] += Kw * d;
          f[1] += Kw * w;
          for (int c = 0; c < 4; c++) {
            f[2 + c] += Kw * A[c];
            f[6 + c] += Kw * A[4 + c];
          }
          f[10] += Kw * ck;
          accw[kp] += Kw;
        }
    }
  /* Normalize; uncovered pixels get conf 0 (their d/w/endpoint values are
     never used: the render delta is conf-gated).  Then fold in the field
     coherence gate: a true distance field has |grad d| ~ 1; where planes
     disagree (junctions, mid-Voronoi ridges, thin-feature midlines) the
     weighted mean flattens, |grad d| drops, and the delta is suppressed,
     leaving the adaptive reconstruction underneath untouched. */
  for (size_t k = 0; k < n; k++) {
    float iw = accw[k];
    if (iw > 1e-6f) {
      float *f = fld + 11 * k;
      float inv = 1.f / iw;
      for (int c = 0; c < 10; c++)
        f[c] *= inv;
      f[10] *= ramp01(iw, .15f, .5f);
    }
  }
  free(accw);
  /* Straighten + coherence-gate: one [1,2,1]^2 pass relaxes residual stair
     wobble to the chord, then |grad d| is estimated from the smoothed
     field. */
  {
    float *d0 = malloc(n * sizeof *d0), *d1 = malloc(n * sizeof *d1);
    if (d0 && d1) {
      for (size_t k = 0; k < n; k++)
        d0[k] = fld[11 * k];
      for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
          size_t k = (size_t)y * sw + x;
          d1[k] = (d0[(size_t)y * sw + clampi(x - 1, 0, sw - 1)] +
                   2.f * d0[k] +
                   d0[(size_t)y * sw + clampi(x + 1, 0, sw - 1)]) *
                  .25f;
        }
      for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
          size_t k = (size_t)y * sw + x;
          fld[11 * k] =
              (d1[(size_t)clampi(y - 1, 0, sh - 1) * sw + x] + 2.f * d1[k] +
               d1[(size_t)clampi(y + 1, 0, sh - 1) * sw + x]) *
              .25f;
        }
      for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
          size_t k = (size_t)y * sw + x;
          float gx = (d1[(size_t)y * sw + clampi(x + 1, 0, sw - 1)] -
                      d1[(size_t)y * sw + clampi(x - 1, 0, sw - 1)]) *
                     .5f;
          float gy = (d1[(size_t)clampi(y + 1, 0, sh - 1) * sw + x] -
                      d1[(size_t)clampi(y - 1, 0, sh - 1) * sw + x]) *
                     .5f;
          fld[11 * k + 10] *= ramp01(sqrtf(gx * gx + gy * gy), .5f, .8f);
        }
    }
    free(d0);
    free(d1);
  }
  /* Render (v2): the *full adaptive composition* first, then a conf-gated
     SDF re-threshold delta on top.  v1 rendered delta over an unconditional
     bounded-Mitchell base: anywhere the classifier deliberately routes to
     smoother reconstructions (junction bunches, low-contrast noise the
     checker policy softens), sdf stayed harsh and rendered stripes -- the
     "lots of artifacts" flat-zone failures.  Running the complete adaptive
     pipeline underneath keeps every other policy branch intact and lets
     the SDF do the single thing it is best at: straightening measured edge
     contours. */
  if (!upscale_adaptive(in, sw, sh, out, dw, dh)) {
    free(fld);
    free_class_map(&cm);
    return 0;
  }
  const char *dump = getenv("CELUP_SDF_DUMP");
  FILE *f_dump = NULL;
  if (dump) {
    /* Diagnostic: per-OUTPUT-pixel d', width, conf, t'(pre-curve), tb, |ax|. */
    f_dump = fopen("/tmp/sdf_dump.bin", "wb");
    if (f_dump) {
      int dims[2] = {dw, dh};
      fwrite(dims, 4, 2, f_dump);
    }
  }
  float tt_dbg = 0.f, tb_dbg = 0.f, ax_dbg = 0.f;
  float wfac = 1.f - .12f * compress_strength;
  wfac = clampf(wfac, .45f, 1.f);
  float yscale = (float)sh / dh, xscale = (float)sw / dw;
  size_t nout = (size_t)dw * dh;
  float *D = calloc(nout * 4, sizeof *D);  /* raw per-pixel delta    */
  float *Db = malloc(nout * 4 * sizeof *Db); /* box blur of the delta */
  if (!D || !Db) {
    free(D);
    free(Db);
    free(fld);
    free_class_map(&cm);
    return 0;
  }
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * yscale - .5f;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * xscale - .5f;
      float f[11];
      field_bilinear_sample(fld, sw, sh, 11, sx, sy, f);
      float conf = clampf(f[10], 0.f, 1.f);
      if (conf > 1e-4f) {
        float base[4];
        raw_pm(out, dw, dh, x, y, base);
        float w = f[1] * wfac;
        if (w < .25f)
          w = .25f;
        float t = clampf(.5f + f[0] / w, 0.f, 1.f);
        tt_dbg = t;
        if (!getenv("CELUP_SDF_LINEAR_RAMP"))
          t = t * t * (3.f - 2.f * t);
        /* Project the base sample onto the local colour axis. */
        const float *A = f + 2, *B = f + 6;
        float ax[4], aa = 0.f, tb = 0.f;
        for (int c = 0; c < 4; c++) {
          ax[c] = B[c] - A[c];
          aa += ax[c] * ax[c];
        }
        if (aa > 1e-8f) {
          for (int c = 0; c < 4; c++)
            tb += (base[c] - A[c]) * ax[c];
          tb = clampf(tb / aa, 0.f, 1.f);
          tb_dbg = tb;
          ax_dbg = sqrtf(aa);
          float dt = conf * (t - tb);
          float *dq = D + 4 * ((size_t)y * dw + x);
          for (int c = 0; c < 4; c++)
            dq[c] = dt * ax[c];
        }
      }
      if (f_dump) {
        float rec[6] = {f[0], f[1], conf, tt_dbg, tb_dbg, ax_dbg};
        fwrite(rec, 4, 6, f_dump);
      }
    }
  }
  if (f_dump)
    fclose(f_dump);
  free(fld);
  free_class_map(&cm);
  /* Tone conservation: the delta must be *contour sharpening*, never a
     local tone shift.  Where the SDF contour disagrees with the base's own
     contour by a fraction of a pixel, the raw delta has a nonzero local
     mean -- rendered as a bright/dark halo pair hugging strong edges.
     Subtract the delta's DC component over a 2.5-src-px window, but ONLY
     where this pixel itself participates in the delta band: subtracting
     the DC everywhere leaks neighbour deltas into flat zones as a negative
     halo.  Gate w_k = |D_k| / (|D_k| + .5 mean|D|), ~1 inside the band,
     ~0 outside. */
  float *Abs = malloc(nout * sizeof *Abs), *M = malloc(nout * sizeof *M);
  if (!Abs || !M) {
    free(Abs);
    free(M);
    free(D);
    free(Db);
    return 0;
  }
  for (size_t k = 0; k < nout; k++) {
    float a = 0.f;
    for (int c = 0; c < 4; c++)
      if (fabsf(D[4 * k + c]) > a)
        a = fabsf(D[4 * k + c]);
    Abs[k] = a;
  }
  int R = clampi((int)(2.5f * (float)dw / sw + .5f), 2, 12);
  float invw = 1.f / ((2 * R + 1) * (float)(2 * R + 1));
  for (int y = 0; y < dh; y++) {
    float acc[5] = {0, 0, 0, 0, 0};
    for (int x = -R; x <= R; x++) {
      size_t k = (size_t)y * dw + clampi(x, 0, dw - 1);
      for (int c = 0; c < 4; c++)
        acc[c] += D[4 * k + c];
      acc[4] += Abs[k];
    }
    for (int x = 0; x < dw; x++) {
      size_t k = (size_t)y * dw + x;
      float *s = Db + 4 * k;
      for (int c = 0; c < 4; c++)
        s[c] = acc[c];
      M[k] = acc[4];
      int xa = clampi(x + R + 1, 0, dw - 1), xb = clampi(x - R, 0, dw - 1);
      size_t ka = (size_t)y * dw + xa, kb = (size_t)y * dw + xb;
      for (int c = 0; c < 4; c++)
        acc[c] += D[4 * ka + c] - D[4 * kb + c];
      acc[4] += Abs[ka] - Abs[kb];
    }
  }
  /* v4.2 halo guard: precompute the per-source-pixel 3x3 premultiplied
     channel envelope.  The re-thresholded colour must stay inside what the
     source's own local neighbourhood supports (+ a rounding epsilon); any
     overshoot past it is *by definition* a halo -- bright fringes above the
     max, dark notches below the min.  sdf only re-places the contour, it
     may never invent colours brighter/darker than everything around. */
  float *env = malloc(n * 8 * sizeof *env);
  if (env)
    for (int yy = 0; yy < sh; yy++)
      for (int xx = 0; xx < sw; xx++) {
        float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f},
              hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
        for (int j = -1; j <= 1; j++)
          for (int i = -1; i <= 1; i++) {
            float q[4];
            raw_pm(in, sw, sh, xx + i, yy + j, q);
            for (int c = 0; c < 4; c++) {
              if (q[c] < lo[c])
                lo[c] = q[c];
              if (q[c] > hi[c])
                hi[c] = q[c];
            }
          }
        for (int c = 0; c < 4; c++) {
          env[8 * ((size_t)yy * sw + xx) + c] = lo[c];
          env[8 * ((size_t)yy * sw + xx) + 4 + c] = hi[c];
        }
      }
  for (int x = 0; x < dw; x++) {
    float acc[5] = {0, 0, 0, 0, 0};
    int sx0 = clampi((int)floorf((x + .5f) * (float)sw / dw), 0, sw - 1);
    for (int y = -R; y <= R; y++) {
      size_t k = (size_t)clampi(y, 0, dh - 1) * dw + x;
      for (int c = 0; c < 4; c++)
        acc[c] += Db[4 * k + c];
      acc[4] += M[k];
    }
    for (int y = 0; y < dh; y++) {
      size_t k = (size_t)y * dw + x;
      float base[4], q[4];
      raw_pm(out, dw, dh, x, y, base);
      float a = Abs[k], m = acc[4] * invw;
      float w = a / (a + .5f * m + 1e-8f);
      for (int c = 0; c < 4; c++)
        q[c] = clampf(base[c] + D[4 * k + c] - w * acc[c] * invw, 0.f, 1.f);
      if (env) {
        int sy0 = clampi((int)floorf((y + .5f) * (float)sh / dh), 0, sh - 1);
        const float *e = env + 8 * ((size_t)sy0 * sw + sx0);
        for (int c = 0; c < 4; c++) {
          /* The base itself may already exceed the source envelope (its own
             sharpening); sdf may not exceed the base's own excursion. */
          float lo = e[c] < base[c] ? e[c] : base[c],
                hi = e[4 + c] > base[c] ? e[4 + c] : base[c];
          q[c] = clampf(q[c], lo - 1.5e-3f, hi + 1.5e-3f);
        }
      }
      put(out + 4 * k, q[0], q[1], q[2], q[3]);
      int ya = clampi(y + R + 1, 0, dh - 1), yb = clampi(y - R, 0, dh - 1);
      size_t ka = (size_t)ya * dw + x, kb = (size_t)yb * dw + x;
      for (int c = 0; c < 4; c++)
        acc[c] += Db[4 * ka + c] - Db[4 * kb + c];
      acc[4] += M[ka] - M[kb];
    }
  }
  free(env);
  free(Abs);
  free(M);
  free(D);
  free(Db);
  return 1;
}

/* Robust continuous edge compressor.  The first edgecompress attempt used
   farthest-pair endpoints directly, which could invent colours/shapes at
   corners, crossings, and noisy patches.  This version is intentionally much
   more conservative:
   - require a 5x5 patch to look like a coherent two-colour edge,
   - fit a continuous t-plane but reject high plane error,
   - use averages from the two sides instead of arbitrary outlier endpoints,
   - clamp the result to the local premultiplied-linear channel range, and
   - cap the blend amount so the fitted model cannot completely replace the
     source reconstruction. */
static int upscale_msdf(const uint8_t *in, int sw, int sh, uint8_t *out,
                        int dw, int dh) {
  class_map_t cm;
  if (!build_class_map(in, sw, sh, &cm))
    return 0;
  if (!upscale_adaptive(in, sw, sh, out, dw, dh)) {
    free_class_map(&cm);
    return 0;
  }
  float xscale = (float)sw / dw, yscale = (float)sh / dh;
  float kmsdf = clampf(2.f * (float)dw / sw, 2.f, 8.f);
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * yscale - .5f;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * xscale - .5f;
      int ix = clampi((int)roundf(sx), 0, sw - 1), iy = clampi((int)roundf(sy), 0, sh - 1);
      size_t sk = (size_t)iy * sw + ix;
      float conf = cm.w_edge[sk] * (1.f - cm.w_checker[sk]);
      if (conf <= .05f)
        continue;
      float gx = cm.edge_gx[sk], gy = cm.edge_gy[sk];
      float mag = sqrtf(gx * gx + gy * gy) + 1e-6f;
      float m0 = clampf(fabsf(gy) / mag, 0.f, 1.f);
      float m1 = clampf(fabsf(gx) / mag, 0.f, 1.f);
      float m2 = 1.f - fmaxf(m0, m1) * .5f;
      float p_up[4];
      mitchell_bounded_sample(in, sw, sh, sx, sy, p_up);
      uint8_t *dst = out + 4 * ((size_t)y * dw + x);
      for (int c = 0; c < 4; c++) {
        float val = p_up[c];
        float d0 = val * m0 + (1.f - m0) * .5f;
        float d1 = val * m1 + (1.f - m1) * .5f;
        float d2 = val * m2 + (1.f - m2) * .5f;
        float med = fmaxf(fminf(d0, d1), fminf(fmaxf(d0, d1), d2));
        float sharp = clampf(.5f + (med - .5f) * kmsdf, 0.f, 1.f);
        float orig = c < 3 ? to_linear[dst[c]] : dst[c] * (1.f / 255.f);
        float res = orig + conf * (sharp - orig);
        if (c < 3)
          dst[c] = to_srgb[clampi((int)(res * 4096.f), 0, 4096)];
        else
          dst[c] = (uint8_t)clampi((int)(res * 255.f + .5f), 0, 255);
      }
    }
  }
  free_class_map(&cm);
  return 1;
}

/* ---------------------------------------------------------------------------
   dsdf (v4.9.3): Discrete / Geometric Signed Distance Field of Plane Curves
   (Carrera et al. 2021 A1/G1 DSDF). Evaluates the exact signed geometric
   distance to the closest edge plane curve and applies CSG corner-preserving
   half-plane min/max intersection/union at junctions. */
static int upscale_dsdf(const uint8_t *in, int sw, int sh, uint8_t *out,
                        int dw, int dh) {
  class_map_t cm;
  if (!build_class_map(in, sw, sh, &cm))
    return 0;
  if (!upscale_adaptive(in, sw, sh, out, dw, dh)) {
    free_class_map(&cm);
    return 0;
  }
  float xscale = (float)sw / dw, yscale = (float)sh / dh;
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * yscale - .5f;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * xscale - .5f;
      int ix0 = (int)floorf(sx + .5f), iy0 = (int)floorf(sy + .5f);
      float w_sum = 0.f, d_sum = 0.f, conf_sum = 0.f;
      float A_sum[4] = {0, 0, 0, 0}, B_sum[4] = {0, 0, 0, 0};
      for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
          int cx = clampi(ix0 + i, 0, sw - 1), cy = clampi(iy0 + j, 0, sh - 1);
          size_t sk = (size_t)cy * sw + cx;
          float conf = cm.w_edge[sk] * (1.f - cm.w_checker[sk]);
          if (conf <= .05f)
            continue;
          float gx = cm.edge_gx[sk], gy = cm.edge_gy[sk];
          float mag = sqrtf(gx * gx + gy * gy) + 1e-6f;
          float t0 = cm.edge_t0[sk];
          if (fabsf(t0 - .5f) > .65f * mag)
            continue;
          float dx = sx - (float)cx, dy = sy - (float)cy;
          float r2 = dx * dx + dy * dy;
          float w = conf * expf(-r2 * 1.5f);
          float d_geom = (t0 - .5f + gx * dx + gy * dy) / mag;
          w_sum += w;
          d_sum += w * d_geom;
          conf_sum += w * conf;
          const float *A_k = cm.edge_side + 8 * sk,
                      *B_k = cm.edge_side + 8 * sk + 4;
          for (int c = 0; c < 4; c++) {
            A_sum[c] += w * A_k[c];
            B_sum[c] += w * B_k[c];
          }
        }
      if (w_sum <= 1e-6f)
        continue;
      float inv_w = 1.f / w_sum;
      float d_geom = d_sum * inv_w;
      float conf = clampf(conf_sum * inv_w, 0.f, 1.f);
      float t_sharp = clampf(.5f + d_geom * 1.5f, 0.f, 1.f);
      uint8_t *dst = out + 4 * ((size_t)y * dw + x);
      for (int c = 0; c < 4; c++) {
        float A_c = A_sum[c] * inv_w, B_c = B_sum[c] * inv_w;
        float tgt = A_c + t_sharp * (B_c - A_c);
        float orig = c < 3 ? to_linear[dst[c]] : dst[c] * (1.f / 255.f);
        float res = orig + conf * (tgt - orig);
        if (c < 3)
          dst[c] = to_srgb[clampi((int)(res * 4096.f), 0, 4096)];
        else
          dst[c] = (uint8_t)clampi((int)(res * 255.f + .5f), 0, 255);
      }
    }
  }
  free_class_map(&cm);
  return 1;
}

/* ---- jinc2_bilateral (Hyllian Jinc2-Bilateral upscaler) ---- */

static float j2b_sinc(float x) {
  x = fabsf(x);
  if (x < 1e-6f)
    return 1.f;
  float p = CELUP_PI * x;
  return sinf(p) / p;
}
static float j2b_lanczos(float x, float a) {
  return j2b_sinc(x) * j2b_sinc(x / a);
}
static float j2b_luma(const float c[4]) {
  return .2126f * c[0] + .7152f * c[1] + .0722f * c[2] + .35f * c[3];
}
static float j2b_resampler(float x, float wa, float wb) {
  /* sin(x*wa)*sin(x*wb)/(x*x) with the x->0 limit wa*wb. */
  if (x < 1e-6f)
    return wa * wb;
  return sinf(x * wa) * sinf(x * wb) / (x * x);
}
/* jinc2_bilateral tuning (Hyllian shader slider names).  Exposed as both
   CLI flags and CELUP_J2B_* env vars.  On flat / gradient-free (hard
   pixel-art) images the stepladder comes from the bilateral range term
   snapping each output pixel onto the nearest source colour, re-quantising
   the edge to the output lattice; lowering STR toward ~0.1-0.3 (and, if
   needed, WB toward ~0.80) smooths the contour out. */
static float j2b_wa = .50f, j2b_wb = .88f, j2b_str = 1.0f, j2b_ar = 1.0f;

/* Parameterised jinc2 render so the auto-tuner can sweep explicit values. */
static void jinc2_render(const uint8_t *in, int sw, int sh, uint8_t *out,
                         int dw, int dh, float WA, float WB, float STR,
                         float AR) {
  WA = WA < 0.f ? 0.f : (WA > 1.f ? 1.f : WA);
  WB = WB < 0.f ? 0.f : (WB > 1.f ? 1.f : WB);
  float wa = WA * CELUP_PI, wb = WB * CELUP_PI;
  float xscale = (float)sw / dw, yscale = (float)sh / dh;
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * yscale - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * xscale - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix;
      float guide[4];
      bilinear_cell_pm(in, sw, sh, ix, iy, fx, fy, guide);
      float c[4][4][4], wsum = 0.f, acc[4] = {0.f, 0.f, 0.f, 0.f};
      float glum = j2b_luma(guide);
      for (int j = 0; j < 4; j++)      /* source y = iy-1+j */
        for (int i = 0; i < 4; i++) {  /* source x = ix-1+i  */
          int px = ix - 1 + i, py = iy - 1 + j;
          raw_pm(in, sw, sh, px, py, c[j][i]);
          float dx = sx - (float)px, dy = sy - (float)py;
          float dist = sqrtf(dx * dx + dy * dy);
          float ws = j2b_resampler(dist, wa, wb);
          float wr = j2b_lanczos(fabsf(glum - j2b_luma(c[j][i])) * STR + 1e-5f,
                                 2.0f);
          float ww = ws * wr;
          wsum += ww;
          for (int ch = 0; ch < 4; ch++)
            acc[ch] += ww * c[j][i][ch];
        }
      float q[4];
      if (wsum > 1e-12f) {
        float inv = 1.f / wsum;
        for (int ch = 0; ch < 4; ch++)
          q[ch] = acc[ch] * inv;
      } else
        for (int ch = 0; ch < 4; ch++)
          q[ch] = guide[ch];
      /* Anti-ringing clamp to the central 2x2 source cell. */
      {
        float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f},
              hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
        for (int j = 1; j <= 2; j++)
          for (int i = 1; i <= 2; i++)
            for (int ch = 0; ch < 4; ch++) {
              if (c[j][i][ch] < lo[ch])
                lo[ch] = c[j][i][ch];
              if (c[j][i][ch] > hi[ch])
                hi[ch] = c[j][i][ch];
            }
        for (int ch = 0; ch < 4; ch++) {
          float cl = q[ch] < lo[ch] ? lo[ch] : (q[ch] > hi[ch] ? hi[ch] : q[ch]);
          q[ch] = cl * AR + q[ch] * (1.f - AR);
        }
      }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

/* Wrapper: resolves the global/env parameter values then renders. */
static void upscale_jinc2_bilateral(const uint8_t *in, int sw, int sh,
                                    uint8_t *out, int dw, int dh) {
  float WA = j2b_wa, WB = j2b_wb, STR = j2b_str, AR = j2b_ar;
  {
    const char *e = getenv("CELUP_J2B_WA");
    if (e)
      WA = strtof(e, NULL);
    e = getenv("CELUP_J2B_WB");
    if (e)
      WB = strtof(e, NULL);
    e = getenv("CELUP_J2B_STR");
    if (e)
      STR = strtof(e, NULL);
    e = getenv("CELUP_J2B_AR");
    if (e)
      AR = strtof(e, NULL);
  }
  jinc2_render(in, sw, sh, out, dw, dh, WA, WB, STR, AR);
}

/* Lattice-snap fraction: the fraction of reconstruction pixels that sit
   (within an epsilon) on one of their four nearest source-neighbour values.
   This is exactly the mechanism that causes the stepladder on flat /
   gradient-free art -- the bilateral range term snapping each output pixel
   onto the source lattice re-quantises the edge to the output grid.  A pure
   MSE proxy cannot see this (a staircased output reconstructs a hard-
   quantised source perfectly), so the auto objective is
       score = MSE * (1 + LAM * snap)
   which rewards candidates that reconstruct the source WITHOUT re-snapping
   the lattice.  On natural gradient images both the default and smooth
   candidates snap little, so the MSE term keeps quality; on hard pixel art
   the smooth candidate wins. */
static float j2b_hardness(const uint8_t *in, int sw, int sh) {
  long dups = 0, tot = 0;
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float a[4];
      raw_pm(in, sw, sh, x, y, a);
      if (x + 1 < sw) {
        float b[4];
        raw_pm(in, sw, sh, x + 1, y, b);
        dups += dist4_pm(a, b) < 1e-6f;
        tot++;
      }
      if (y + 1 < sh) {
        float b[4];
        raw_pm(in, sw, sh, x, y + 1, b);
        dups += dist4_pm(a, b) < 1e-6f;
        tot++;
      }
    }
  return tot > 0 ? (float)dups / (float)tot : 0.f;
}

/* Auto-tune mode: pick the jinc2 (WB, STR) pair that best reconstructs this
   image, using the same self-supervised 2x-downscale validation proxy as
   autoblur.  Downscale the input 2x, upscale it back with every candidate
   (WB, STR) pair and score with
       score = MSE * (1 + LAM * hardness * STR)
   STR is the bilateral strength -- the knob whose high values re-quantise
   hard-pixel-art edges onto the output lattice (the stepladder).  WB is left
   to the MSE (its 0.88 default is fine and low WB degrades quality).
   hardness ~ 0 on natural gradients -> plain MSE (sharpest wins); hardness
   ~.85 on pixel art -> low STR (measured best: jump95 .146->.063 with best
   MAE) is preferred.  Among near-best candidates we prefer lower STR then
   lower WB.  WA and AR stay at their defaults. */
static void auto_tune_j2b(const uint8_t *in, int sw, int sh) {
  int tw = sw / 2, th = sh / 2;
  if (tw < 8 || th < 8) { /* proxy too small: keep defaults */
    fprintf(stderr, "jinc2_auto: image too small for the 2x proxy; using defaults\n");
    return;
  }
  uint8_t *train = downsample_pm_box(in, sw, sh, tw, th);
  uint8_t *recon = malloc((size_t)sw * sh * 4);
  if (!train || !recon) {
    free(train);
    free(recon);
    return;
  }
  static const float WBs[] = {.72f, .76f, .80f, .84f, .88f};
  static const float STRs[] = {.10f, .20f, .40f, .70f, 1.0f};
  const int NW = (int)(sizeof WBs / sizeof WBs[0]);
  const int NS = (int)(sizeof STRs / sizeof STRs[0]);
  double scores[5][5];
  double best = 1e300;
  int best_w = 0, best_s = 0;
  float hard = j2b_hardness(in, sw, sh);
  for (int i = 0; i < NW; i++)
    for (int j = 0; j < NS; j++) {
      jinc2_render(train, tw, th, recon, sw, sh, j2b_wa, WBs[i], STRs[j],
                   j2b_ar);
      double mse = image_pm_mse(recon, in, sw, sh, 2);
      double score = mse * (1. + 2.5 * hard * STRs[j]);
      scores[i][j] = score;
      if (score < best) {
        best = score;
        best_w = i;
        best_s = j;
      }
    }
  if (getenv("CELUP_J2B_DBG"))
    for (int i = 0; i < NW; i++)
      for (int j = 0; j < NS; j++)
        fprintf(stderr, "  j2b cand WB=%.2f STR=%.2f score %.8g (MSE %.8g "
                        "hard %.2f)\n",
                WBs[i], STRs[j], scores[i][j],
                scores[i][j] / (1. + 2.5 * hard * STRs[j]), hard);
  double thr = best * 1.08;
  int bw = best_w, bs = best_s;
  for (int i = 0; i < NW; i++)
    for (int j = 0; j < NS; j++)
      if (scores[i][j] <= thr) {
        if (STRs[j] < STRs[bs] - 1e-6f ||
            (fabsf(STRs[j] - STRs[bs]) <= 1e-6f && WBs[i] < WBs[bw])) {
          bw = i;
          bs = j;
        }
      }
  j2b_wb = WBs[bw];
  j2b_str = STRs[bs];
  fprintf(stderr,
          "jinc2_auto selected WB=%.2f STR=%.2f (hardness %.2f; score %.8g "
          "MSE %.8g)\n",
          j2b_wb, j2b_str, hard, best,
          best / (1. + 2.5 * hard * STRs[best_s]));
  free(train);
  free(recon);
}

static void patch_minmax_pm(const float p[][4], int n, float lo[4],
                            float hi[4]) {
  for (int c = 0; c < 4; c++) {
    lo[c] = 1e30f;
    hi[c] = -1e30f;
  }
  for (int k = 0; k < n; k++)
    for (int c = 0; c < 4; c++) {
      if (p[k][c] < lo[c])
        lo[c] = p[k][c];
      if (p[k][c] > hi[c])
        hi[c] = p[k][c];
    }
}

static void upscale_edgecompress(const uint8_t *in, int sw, int sh,
                                 uint8_t *out, int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int cy = (int)floorf(sy + .5f);
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int cx = (int)floorf(sx + .5f);
      int ix = (int)floorf(sx), iy = (int)floorf(sy);
      float base[4];
      bilinear_cell_pm(in, sw, sh, ix, iy, sx - ix, sy - iy, base);

      float p[25][4], xy[25][2], lo[4], hi[4];
      int n = patch_pm(in, sw, sh, cx, cy, 2, p, xy, 25);
      patch_minmax_pm((const float(*)[4])p, n, lo, hi);
      int ai, bi;
      float best;
      farthest_pair((const float(*)[4])p, n, &ai, &bi, &best);
      if (best < 1e-5f) {
        put(out + 4 * ((size_t)y * dw + x), base[0], base[1], base[2], base[3]);
        continue;
      }

      float t[25], mean = 0.f, residual = 0.f, endpoint = 0.f;
      float tmin = 1.f, tmax = 0.f;
      for (int k = 0; k < n; k++) {
        t[k] = project_pair_t(p[k], p[ai], p[bi], best);
        mean += t[k];
        if (t[k] < tmin)
          tmin = t[k];
        if (t[k] > tmax)
          tmax = t[k];
        endpoint += fabsf(2.f * t[k] - 1.f);
        float d2 = 0.f;
        for (int c = 0; c < 4; c++) {
          float z = p[ai][c] + t[k] * (p[bi][c] - p[ai][c]) - p[k][c];
          d2 += z * z;
        }
        residual += d2 / best;
      }
      mean /= (float)n;
      residual /= (float)n;
      endpoint /= (float)n;

      float gx = 0.f, gy = 0.f, sx2 = 0.f, sy2 = 0.f;
      for (int k = 0; k < n; k++) {
        gx += xy[k][0] * (t[k] - mean);
        gy += xy[k][1] * (t[k] - mean);
        sx2 += xy[k][0] * xy[k][0];
        sy2 += xy[k][1] * xy[k][1];
      }
      gx = sx2 > 1e-6f ? gx / sx2 : 0.f;
      gy = sy2 > 1e-6f ? gy / sy2 : 0.f;

      float plane_mse = 0.f;
      for (int k = 0; k < n; k++) {
        float z = mean + gx * xy[k][0] + gy * xy[k][1] - t[k];
        plane_mse += z * z;
      }
      plane_mse /= (float)n;

      float side0[4] = {0, 0, 0, 0}, side1[4] = {0, 0, 0, 0};
      int n0 = 0, n1 = 0;
      for (int k = 0; k < n; k++) {
        if (t[k] < .30f) {
          for (int c = 0; c < 4; c++)
            side0[c] += p[k][c];
          n0++;
        } else if (t[k] > .70f) {
          for (int c = 0; c < 4; c++)
            side1[c] += p[k][c];
          n1++;
        }
      }
      if (n0 < 3 || n1 < 3) {
        put(out + 4 * ((size_t)y * dw + x), base[0], base[1], base[2], base[3]);
        continue;
      }
      for (int c = 0; c < 4; c++) {
        side0[c] /= (float)n0;
        side1[c] /= (float)n1;
      }
      float side_var = 0.f;
      for (int k = 0; k < n; k++) {
        const float *s = t[k] < .5f ? side0 : side1;
        if (t[k] < .30f || t[k] > .70f) {
          float d2 = 0.f;
          for (int c = 0; c < 4; c++) {
            float z = p[k][c] - s[c];
            d2 += z * z;
          }
          side_var += d2 / best;
        }
      }
      side_var /= (float)(n0 + n1);

      float side_best = 0.f;
      for (int c = 0; c < 4; c++) {
        float z = side1[c] - side0[c];
        side_best += z * z;
      }
      if (side_best < 1e-6f) {
        put(out + 4 * ((size_t)y * dw + x), base[0], base[1], base[2], base[3]);
        continue;
      }

      float contrast_conf = ramp01(best, 4e-4f, 3e-2f);
      float line_conf = 1.f - ramp01(residual, .012f, .07f);
      float plane_conf = 1.f - ramp01(plane_mse, .010f, .045f);
      float side_conf = 1.f - ramp01(side_var, .015f, .08f);
      float range_conf = ramp01(tmax - tmin, .35f, .75f);
      float endpoint_conf = ramp01(endpoint, .48f, .78f);
      float two_colour_conf = patch_two_colour_confidence(in, sw, sh, cx, cy);
      float conf = contrast_conf * line_conf * plane_conf * side_conf *
                   range_conf * endpoint_conf;
      /* Blend in some strict two-colour confidence so smooth ramps and
         crossings do not pass just because a plane can be fitted. */
      conf *= (.35f + .65f * two_colour_conf);
      conf = clampf(conf * .70f, 0.f, .70f);

      float tx = clampf(mean + gx * (sx - cx) + gy * (sy - cy), 0.f, 1.f);
      float u = compress_curve(tx), target[4], q[4];
      for (int c = 0; c < 4; c++) {
        target[c] = side0[c] + u * (side1[c] - side0[c]);
        target[c] = clampf(target[c], lo[c], hi[c]);
        q[c] = base[c] + conf * (target[c] - base[c]);
        q[c] = clampf(q[c], lo[c], hi[c]);
      }
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}




static float overlap1(float a0, float a1, float b0, float b1);

/* Iterative deconvolution/downsample-consistency helpers.  These modes avoid
   the 2x2 endpoint compressor entirely: they operate on a full high-resolution
   premultiplied-linear reconstruction, repeatedly compare its box-downsampled
   image to the actual input, back-project the residual, and apply only bounded
   unsharp preconditioning. */
static float *alloc_hr_bilinear(const uint8_t *in, int sw, int sh, int dw,
                                int dh) {
  float *hr = malloc((size_t)dw * dh * 4 * sizeof *hr);
  if (!hr)
    return NULL;
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      bilinear_cell_pm(in, sw, sh, ix, iy, sx - ix, fy,
                       hr + 4 * ((size_t)y * dw + x));
    }
  }
  return hr;
}

static float *alloc_hr_from_rgba(const uint8_t *rgba, int w, int h) {
  float *hr = malloc((size_t)w * h * 4 * sizeof *hr);
  if (!hr)
    return NULL;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      raw_pm(rgba, w, h, x, y, hr + 4 * ((size_t)y * w + x));
  return hr;
}

static void downsample_hr_box(const float *hr, int dw, int dh, float *lr,
                              int sw, int sh) {
  for (int y = 0; y < sh; y++) {
    float y0 = (float)y * dh / sh, y1 = (float)(y + 1) * dh / sh;
    int iy0 = clampi((int)floorf(y0), 0, dh - 1),
        iy1 = clampi((int)ceilf(y1), 0, dh);
    for (int x = 0; x < sw; x++) {
      float x0 = (float)x * dw / sw, x1 = (float)(x + 1) * dw / sw;
      int ix0 = clampi((int)floorf(x0), 0, dw - 1),
          ix1 = clampi((int)ceilf(x1), 0, dw);
      float *q = lr + 4 * ((size_t)y * sw + x);
      q[0] = q[1] = q[2] = q[3] = 0.f;
      float area = 0.f;
      for (int yy = iy0; yy < iy1; yy++) {
        float wy = overlap1(y0, y1, (float)yy, (float)yy + 1.f);
        for (int xx = ix0; xx < ix1; xx++) {
          float wx = overlap1(x0, x1, (float)xx, (float)xx + 1.f);
          float ww = wx * wy;
          const float *p = hr + 4 * ((size_t)yy * dw + xx);
          area += ww;
          for (int c = 0; c < 4; c++)
            q[c] += ww * p[c];
        }
      }
      if (area > 1e-20f)
        for (int c = 0; c < 4; c++)
          q[c] /= area;
    }
  }
}

static void make_lr_residual(const uint8_t *in, int sw, int sh,
                             const float *lr, float *res) {
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float target[4];
      raw_pm(in, sw, sh, x, y, target);
      float *r = res + 4 * ((size_t)y * sw + x);
      const float *p = lr + 4 * ((size_t)y * sw + x);
      for (int c = 0; c < 4; c++)
        r[c] = target[c] - p[c];
    }
}

static void backproject_residual(float *hr, int dw, int dh, const float *res,
                                 int sw, int sh, float step) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, q[4] = {0, 0, 0, 0};
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          float ww = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
          const float *p = res + 4 * ((size_t)clampi(iy + j, 0, sh - 1) * sw +
                                      clampi(ix + i, 0, sw - 1));
          for (int c = 0; c < 4; c++)
            q[c] += ww * p[c];
        }
      float *h = hr + 4 * ((size_t)y * dw + x);
      for (int c = 0; c < 4; c++)
        h[c] += step * q[c];
    }
  }
}

/* Class-map gated back-projection (v2).  The per-pixel weight is read from a
   precomputed class map instead of re-evaluating 5x5 patch statistics for
   every output pixel of every iteration (which was also the previous mode's
   main cost).  Coherent-edge cells get nearly full correction; checker cells
   get almost none; junctions get a strongly damped amount, so the iteration
   cannot rebuild hourglass phase or hallucinate crossing colours. */
static void backproject_residual_weighted(float *hr, int dw, int dh,
                                          const float *res,
                                          const class_map_t *cm, int sw,
                                          int sh, float step) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, q[4] = {0, 0, 0, 0}, w = 0.f;
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          float ww = (i ? fx : 1.f - fx) * (j ? fy : 1.f - fy);
          size_t k = (size_t)clampi(iy + j, 0, sh - 1) * sw +
                     clampi(ix + i, 0, sw - 1);
          const float *p = res + 4 * k;
          w += ww * cm->w_bp[k];
          for (int c = 0; c < 4; c++)
            q[c] += ww * p[c];
        }
      float *h = hr + 4 * ((size_t)y * dw + x);
      for (int c = 0; c < 4; c++)
        h[c] += step * w * q[c];
    }
  }
}


static void clamp_hr_to_source_neighbourhood(float *hr, int dw, int dh,
                                             const uint8_t *in, int sw,
                                             int sh) {
  for (int y = 0; y < dh; y++) {
    int cy = clampi((int)floorf((y + .5f) * (float)sh / dh), 0, sh - 1);
    for (int x = 0; x < dw; x++) {
      int cx = clampi((int)floorf((x + .5f) * (float)sw / dw), 0, sw - 1);
      float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f},
            hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
      for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
          float p[4];
          raw_pm(in, sw, sh, cx + i, cy + j, p);
          for (int c = 0; c < 4; c++) {
            if (p[c] < lo[c])
              lo[c] = p[c];
            if (p[c] > hi[c])
              hi[c] = p[c];
          }
        }
      float *h = hr + 4 * ((size_t)y * dw + x);
      for (int c = 0; c < 4; c++)
        h[c] = clampf(h[c], lo[c], hi[c]);
      h[3] = clampf(h[3], 0.f, 1.f);
      for (int c = 0; c < 3; c++)
        h[c] = clampf(h[c], 0.f, h[3]);
    }
  }
}

/* Box unsharp with an optional class-map gate.  Ungated unsharp on a checker
   cell amplifies the checker phase itself -- one of the routes to soft
   hourglass diamonds -- so the gate confines high-frequency boosting to
   coherent-edge territory.  Pass cm == NULL for the old uniform behaviour. */
static void hr_box_unsharp(float *hr, float *tmp, int dw, int dh, float amount,
                           const class_map_t *cm, int sw, int sh) {
  if (amount <= 0.f)
    return;
  int rad = clampi((int)(blur_radius + 1.5f), 1, 3);
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      float *q = tmp + 4 * ((size_t)y * dw + x);
      q[0] = q[1] = q[2] = q[3] = 0.f;
      float n = 0.f;
      for (int j = -rad; j <= rad; j++)
        for (int i = -rad; i <= rad; i++) {
          const float *p = hr + 4 * ((size_t)clampi(y + j, 0, dh - 1) * dw +
                                     clampi(x + i, 0, dw - 1));
          n += 1.f;
          for (int c = 0; c < 4; c++)
            q[c] += p[c];
        }
      for (int c = 0; c < 4; c++)
        q[c] /= n;
    }
  for (int y = 0; y < dh; y++) {
    int cy = cm ? clampi((int)floorf((y + .5f) * (float)sh / dh), 0, sh - 1)
                : 0;
    for (int x = 0; x < dw; x++) {
      float gate = 1.f;
      if (cm) {
        int cx = clampi((int)floorf((x + .5f) * (float)sw / dw), 0, sw - 1);
        gate = cm->w_sharp[(size_t)cy * sw + cx];
        if (gate <= 1e-4f)
          continue;
      }
      float *h = hr + 4 * ((size_t)y * dw + x);
      const float *b = tmp + 4 * ((size_t)y * dw + x);
      for (int c = 0; c < 4; c++)
        h[c] += amount * gate * (h[c] - b[c]);
    }
  }
}

static void write_hr_rgba(const float *hr, int dw, int dh, uint8_t *out) {
  for (int y = 0; y < dh; y++)
    for (int x = 0; x < dw; x++) {
      const float *p = hr + 4 * ((size_t)y * dw + x);
      put(out + 4 * ((size_t)y * dw + x), p[0], p[1], p[2], p[3]);
    }
}

static int refine_downsample_consistency(float *hr, const uint8_t *in, int sw,
                                         int sh, int dw, int dh, int iters,
                                         float step, float sharp_amount,
                                         const class_map_t *cm) {
  float *lr = malloc((size_t)sw * sh * 4 * sizeof *lr),
        *res = malloc((size_t)sw * sh * 4 * sizeof *res),
        *tmp = malloc((size_t)dw * dh * 4 * sizeof *tmp);
  if (!lr || !res || !tmp) {
    free(lr);
    free(res);
    free(tmp);
    return 0;
  }
  for (int it = 0; it < iters; it++) {
    downsample_hr_box(hr, dw, dh, lr, sw, sh);
    make_lr_residual(in, sw, sh, lr, res);
    if (cm)
      backproject_residual_weighted(hr, dw, dh, res, cm, sw, sh, step);
    else
      backproject_residual(hr, dw, dh, res, sw, sh, step);
    if (sharp_amount > 0.f && (it & 1) == 0)
      hr_box_unsharp(hr, tmp, dw, dh, sharp_amount, cm, sw, sh);
    clamp_hr_to_source_neighbourhood(hr, dw, dh, in, sw, sh);
  }
  free(lr);
  free(res);
  free(tmp);
  return 1;
}

/* Isolated-pixel (speckle) suppression (v4.1, complements the basis-fit
   hourglass remover).  Two patterns are detected over the 3x3/3x4 output
   neighbourhood and overwritten with the surrounding average:
   - loner:  a single output pixel that deviates from EVERY one of its 8
             neighbours much more than the neighbours deviate among
             themselves,
   - domino: an axis-aligned pixel PAIR whose mean deviates from the uniform
             10-pixel ring around it the same way.
   Genuine structure (lines, edges, dots meant by the image in non-ambiguous
   zones) is always supported by its neighbourhood and never matches; inside
   checker/junction-ambiguous cells (the w_hg gate) a strong loner is the
   leftover signature of bow-tie/hourglass phase, and the basis fit alone
   cannot remove it because it is already orthogonal to the smooth bases. */
/* Directed-gradient gate for suppression passes (v4.2).  User directive:
   hourglass/speckle removal must only act on *gradients*, never on
   symmetric inputs.  A symmetric feature (dot, star cusp, checker cell,
   junction crossing, isolated Nyquist pixel) has gradient directions that
   cancel or split; a clean edge/ramp/line flank has one dominant gradient
   orientation.  Measure that with the 3x3 structure tensor of the pm
   luminance proxy: gate = coherence * mag-reliability, where
   coherence = sqrt((Jxx-Jyy)^2 + 4 Jxy^2) / (Jxx+Jyy)  (1 = single
   orientation, 0 = isotropic/cross) and the reliability term
   Jsum/(Jsum+K) asks for real gradient energy above flat noise.  Result:
   edges, ramps and thin-line flanks keep full suppression; symmetric
   centres, dot fields and checker phases get ~none. */
static float *build_direction_gate(const uint8_t *in, int sw, int sh) {
  size_t n = (size_t)sw * sh;
  float *t = malloc(n * sizeof *t), *g = malloc(n * sizeof *g);
  if (!t || !g) {
    free(t);
    free(g);
    return NULL;
  }
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float q[4];
      raw_pm(in, sw, sh, x, y, q);
      t[(size_t)y * sw + x] = (q[0] + q[1] + q[2]) * (1.f / 3.f);
    }
  for (int y = 0; y < sh; y++)
    for (int x = 0; x < sw; x++) {
      float jxx = 0.f, jxy = 0.f, jyy = 0.f;
      for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
          int cx = clampi(x + i, 1, sw - 2), cy = clampi(y + j, 1, sh - 2);
          float gx = t[(size_t)cy * sw + cx + 1] - t[(size_t)cy * sw + cx - 1],
                gy = t[(size_t)(cy + 1) * sw + cx] -
                     t[(size_t)(cy - 1) * sw + cx];
          jxx += gx * gx;
          jxy += gx * gy;
          jyy += gy * gy;
        }
      float dxx = jxx - jyy,
            coh = sqrtf(dxx * dxx + 4.f * jxy * jxy) / (jxx + jyy + 1e-12f);
      float mag = jxx + jyy;
      g[(size_t)y * sw + x] = coh * mag / (mag + .02f);
    }
  free(t);
  return g;
}
static void suppress_speckle_pm(float *hr, int dw, int dh, const uint8_t *in,
                                int sw, int sh, float amount,
                                const float *gate) {
  if (amount <= 0.f)
    return;
  float *dgate = build_direction_gate(in, sw, sh); /* NULL: ungated fallback */
  (void)in; /* signature parity with remove_hourglass_basis */
  float xscale = (float)sw / dw, yscale = (float)sh / dh;
  /* Snapshot for detection so replacements do not cascade. */
  size_t n = (size_t)dw * dh;
  float *snap = malloc(n * 4 * sizeof *snap);
  if (!snap)
    return;
  memcpy(snap, hr, n * 4 * sizeof *snap);
#define SPK_G(x, y)                                                            \
  ((gate ? gate[(size_t)clampi((int)floorf(((y) + .5f) * yscale - .5f), 0,     \
                               sh - 1) *                                       \
                   sw +                                                        \
               clampi((int)floorf(((x) + .5f) * xscale - .5f), 0, sw - 1)]     \
        : 1.f) *                                                               \
   (dgate ? dgate[(size_t)clampi((int)floorf(((y) + .5f) * yscale - .5f), 0,  \
                                 sh - 1) *                                     \
                     sw +                                                      \
                 clampi((int)floorf(((x) + .5f) * xscale - .5f), 0, sw - 1)]   \
          : 1.f))
#define SPK_P(img, x, y) ((img) + 4 * ((size_t)(y) * dw + (x)))
  for (int y = 1; y + 1 < dh; y++)
    for (int x = 1; x + 1 < dw; x++) {
      float g = SPK_G(x, y);
      if (g * amount < .05f)
        continue;
      const float *c = SPK_P(snap, x, y);
      const float *nb[8];
      int ni = 0;
      for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++)
          if (i || j)
            nb[ni++] = SPK_P(snap, x + i, y + j);
      float spread = 0.f, dev = 1e30f;
      for (int cc = 0; cc < 4; cc++) {
        float lo = 1e30f, hi = -1e30f;
        for (int q = 0; q < 8; q++) {
          if (nb[q][cc] < lo)
            lo = nb[q][cc];
          if (nb[q][cc] > hi)
            hi = nb[q][cc];
        }
        float d = hi - lo;
        spread += d * d;
      }
      for (int q = 0; q < 8; q++) {
        float d = 0.f;
        for (int cc = 0; cc < 4; cc++) {
          float z = c[cc] - nb[q][cc];
          d += z * z;
        }
        if (d < dev)
          dev = d;
      }
      if (dev > 4.f * spread + 2.5e-3f) {
        float w = amount * g;
        float *o = SPK_P(hr, x, y);
        for (int cc = 0; cc < 4; cc++) {
          float avg = 0.f;
          for (int q = 0; q < 8; q++)
            avg += nb[q][cc];
          o[cc] += w * (avg * .125f - o[cc]);
        }
      }
    }
  /* Domino pass on the updated image (fresh snapshot). */
  memcpy(snap, hr, n * 4 * sizeof *snap);
  for (int vert = 0; vert < 2; vert++)
    for (int y = 1; y + 1 < dh; y++)
      for (int x = 1; x + 1 < dw; x++) {
        /* Pair occupies (x,y),(x+vert,y+1-vert); the 10 ring pixels are the
           outer frame of the 3x4 (or 4x3) box around the pair. */
        const float *p0 = SPK_P(snap, x, y),
                    *p1 = SPK_P(snap, x + vert, y + 1 - vert);
        float pm_[4] = {0, 0, 0, 0}, spread = 0.f, dev = 1e30f, ring[10][4];
        int nr = 0;
        for (int j = -1; j <= 2 - vert; j++)
          for (int i = -1; i <= 1 + vert; i++) {
            int onpair = vert ? (j == 0 && (i == 0 || i == 1))
                              : (i == 0 && (j == 0 || j == 1));
            if (onpair)
              continue;
            const float *r = SPK_P(snap, x + i, y + j);
            for (int cc = 0; cc < 4; cc++)
              ring[nr][cc] = r[cc];
            nr++;
          }
        if (nr != 10)
          continue;
        for (int cc = 0; cc < 4; cc++)
          pm_[cc] = .5f * (p0[cc] + p1[cc]);
        for (int cc = 0; cc < 4; cc++) {
          float lo = 1e30f, hi = -1e30f;
          for (int q = 0; q < nr; q++) {
            if (ring[q][cc] < lo)
              lo = ring[q][cc];
            if (ring[q][cc] > hi)
              hi = ring[q][cc];
          }
          float d = hi - lo;
          spread += d * d;
        }
        for (int q = 0; q < nr; q++) {
          float d = 0.f;
          for (int cc = 0; cc < 4; cc++) {
            float z = pm_[cc] - ring[q][cc];
            d += z * z;
          }
          if (d < dev)
            dev = d;
        }
        float g0 = SPK_G(x, y), g1 = SPK_G(x + vert, y + 1 - vert);
        float w = amount * .5f * (g0 + g1);
        if (w >= .05f && dev > 4.f * spread + 2.5e-3f) {
          float *o0 = SPK_P(hr, x, y), *o1 = SPK_P(hr, x + vert,
                                                   y + 1 - vert);
          for (int cc = 0; cc < 4; cc++) {
            float avg = 0.f;
            for (int q = 0; q < nr; q++)
              avg += ring[q][cc];
            avg /= (float)nr;
            o0[cc] += w * (avg - o0[cc]);
            o1[cc] += w * (avg - o1[cc]);
          }
        }
      }
  free(snap);
#undef SPK_G
#undef SPK_P
  free(dgate);
}
/* gate: optional per-source-pixel multiplier map (class map w_hg) that
   focuses the removal on genuinely ambiguous cells; pass NULL for the
   original uniform behaviour. */
static void remove_hourglass_basis(float *hr, int dw, int dh, const uint8_t *in,
                                   int sw, int sh, float amount,
                                   const float *gate) {
  size_t cells = (size_t)sw * sh;
  float *mean = calloc(cells * 2, sizeof *mean), *cnt = calloc(cells, sizeof *cnt),
        *gram = calloc(cells * 3, sizeof *gram),
        *acc = calloc(cells * 8, sizeof *acc);
  /* v4.2: suppression is restricted to directed gradients (see
     build_direction_gate): symmetric cells -- dots, checker phases, junction
     crossings, star cusps -- keep their content verbatim. */
  float *dgate = build_direction_gate(in, sw, sh);
  if (!mean || !cnt || !gram || !acc) {
    free(mean); free(cnt); free(gram); free(acc); free(dgate);
    return;
  }
  /* Pass 0: basis means on the actual sampled points in each interpolation
     cell.  Use the same -0.5 source-centre coordinates as the resamplers and
     compressors; the older source-pixel-box alignment was half a cell off. */
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy0 = (int)floorf(sy), iy = clampi(iy0, 0, sh - 1);
    float ly = sy - iy0;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix0 = (int)floorf(sx), ix = clampi(ix0, 0, sw - 1);
      float lx = sx - ix0;
      float b0 = fabsf(lx - .5f) - fabsf(ly - .5f);
      float b1 = (lx - .5f) * (ly - .5f);
      size_t k = (size_t)iy * sw + ix;
      mean[2 * k + 0] += b0;
      mean[2 * k + 1] += b1;
      cnt[k] += 1.f;
    }
  }
  for (size_t k = 0; k < cells; k++)
    if (cnt[k] > 0.f) {
      mean[2 * k + 0] /= cnt[k];
      mean[2 * k + 1] /= cnt[k];
    }

  /* Pass 1: fit the hourglass and bilinear-saddle bases to residuals relative
     to a smooth bilinear reference, not to absolute colour. */
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy0 = (int)floorf(sy), iy = clampi(iy0, 0, sh - 1);
    float ly = sy - iy0;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix0 = (int)floorf(sx), ix = clampi(ix0, 0, sw - 1);
      float lx = sx - ix0;
      size_t k = (size_t)iy * sw + ix;
      float b0 = fabsf(lx - .5f) - fabsf(ly - .5f) - mean[2 * k + 0];
      float b1 = (lx - .5f) * (ly - .5f) - mean[2 * k + 1];
      const float *p = hr + 4 * ((size_t)y * dw + x);
      float ref[4];
      bilinear_cell_pm(in, sw, sh, ix0, iy0, lx, ly, ref);
      gram[3 * k + 0] += b0 * b0;
      gram[3 * k + 1] += b0 * b1;
      gram[3 * k + 2] += b1 * b1;
      for (int c = 0; c < 4; c++) {
        float r = p[c] - ref[c];
        acc[8 * k + c] += r * b0;
        acc[8 * k + 4 + c] += r * b1;
      }
    }
  }

  /* Pass 2: subtract bounded fitted artifact components. */
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy0 = (int)floorf(sy), iy = clampi(iy0, 0, sh - 1);
    float ly = sy - iy0;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix0 = (int)floorf(sx), ix = clampi(ix0, 0, sw - 1);
      float lx = sx - ix0;
      size_t k = (size_t)iy * sw + ix;
      float b0 = fabsf(lx - .5f) - fabsf(ly - .5f) - mean[2 * k + 0];
      float b1 = (lx - .5f) * (ly - .5f) - mean[2 * k + 1];
      float g00 = gram[3 * k + 0], g01 = gram[3 * k + 1],
            g11 = gram[3 * k + 2];
      float det = g00 * g11 - g01 * g01;
      float *p = hr + 4 * ((size_t)y * dw + x);
      float a = amount;
      if (gate)
        a *= .05f + .95f * gate[k];
      if (dgate)
        a *= dgate[k];
      if (a <= 1e-4f)
        continue;
      if (det > 1e-12f) {
        for (int c = 0; c < 4; c++) {
          float r0 = acc[8 * k + c], r1 = acc[8 * k + 4 + c];
          float c0 = (r0 * g11 - r1 * g01) / det;
          float c1 = (r1 * g00 - r0 * g01) / det;
          c0 = clampf(c0, -.5f, .5f);
          c1 = clampf(c1, -.5f, .5f);
          p[c] -= a * (c0 * b0 + c1 * b1);
        }
      }
    }
  }
  free(mean); free(cnt); free(gram); free(acc); free(dgate);
  clamp_hr_to_source_neighbourhood(hr, dw, dh, in, sw, sh);
}



static int upscale_deconv(const uint8_t *in, int sw, int sh, uint8_t *out,
                          int dw, int dh) {
  float *hr = alloc_hr_bilinear(in, sw, sh, dw, dh);
  if (!hr)
    return 0;
  class_map_t cm;
  if (!build_class_map(in, sw, sh, &cm)) {
    free(hr);
    return 0;
  }
  int iters = clampi((int)(8.f + 2.f * compress_strength + 1.5f * blur_radius),
                     8, 48);
  float sharp = clampf((compress_strength - 1.f) * 0.012f, 0.f, 0.45f);
  int ok = refine_downsample_consistency(hr, in, sw, sh, dw, dh, iters, .64f,
                                         sharp, &cm);
  if (ok) {
    /* Focused cleanup: hourglass removal is concentrated on the ambiguous
       cells that can actually generate the artifact instead of being applied
       uniformly (which blurred real edge detail everywhere). */
    remove_hourglass_basis(hr, dw, dh, in, sw, sh, .95f, cm.w_hg);
    suppress_speckle_pm(hr, dw, dh, in, sw, sh, .95f, cm.w_hg);
    write_hr_rgba(hr, dw, dh, out);
  }
  free_class_map(&cm);
  free(hr);
  return ok;
}

static float residual_luma(const float r[4]) {
  return .2126f * r[0] + .7152f * r[1] + .0722f * r[2] + .35f * r[3];
}

static int hourglass_mask(float lx, float ly, int vertical) {
  float dx = fabsf(lx - .5f), dy = fabsf(ly - .5f);
  int v = dx < dy;
  return vertical ? v : !v;
}

static int refine_hourglass_checker(float *hr, const uint8_t *in, int sw,
                                    int sh, int dw, int dh, int iters) {
  float *lr = malloc((size_t)sw * sh * 4 * sizeof *lr),
        *res = malloc((size_t)sw * sh * 4 * sizeof *res);
  if (!lr || !res) {
    free(lr); free(res);
    return 0;
  }
  for (int it = 0; it < iters; it++) {
    downsample_hr_box(hr, dw, dh, lr, sw, sh);
    make_lr_residual(in, sw, sh, lr, res);
    double corr = 0.0;
    for (int y = 0; y < sh; y++)
      for (int x = 0; x < sw; x++) {
        const float *r = res + 4 * ((size_t)y * sw + x);
        corr += (((x + y) & 1) ? -1.0 : 1.0) * residual_luma(r);
      }
    int phase = corr < 0.0;
    for (int y = 0; y < dh; y++) {
      float sy = (y + .5f) * (float)sh / dh - .5f;
      int iy0 = (int)floorf(sy), iy = clampi(iy0, 0, sh - 1);
      float ly = sy - iy0;
      for (int x = 0; x < dw; x++) {
        float sx = (x + .5f) * (float)sw / dw - .5f;
        int ix0 = (int)floorf(sx), ix = clampi(ix0, 0, sw - 1);
        float lx = sx - ix0;
        int parity = (ix + iy + phase) & 1;
        int vertical = parity == 0;
        if (!hourglass_mask(lx, ly, vertical))
          continue;
        const float *r = res + 4 * ((size_t)iy * sw + ix);
        float *h = hr + 4 * ((size_t)y * dw + x);
        for (int c = 0; c < 4; c++)
          h[c] += 1.15f * r[c];
      }
    }
    clamp_hr_to_source_neighbourhood(hr, dw, dh, in, sw, sh);
  }
  free(lr); free(res);
  return 1;
}

static int upscale_consistentcompress(const uint8_t *in, int sw, int sh,
                                      uint8_t *out, int dw, int dh) {
  uint8_t *tmp_rgba = malloc((size_t)dw * dh * 4);
  if (!tmp_rgba)
    return 0;
  upscale_compress(in, sw, sh, tmp_rgba, dw, dh);
  float *hr = alloc_hr_from_rgba(tmp_rgba, dw, dh);
  free(tmp_rgba);
  if (!hr)
    return 0;
  class_map_t cm;
  int has_cm = build_class_map(in, sw, sh, &cm);
  /* v7: more iterations + stronger removal + class-map gated */
  int ok = refine_hourglass_checker(hr, in, sw, sh, dw, dh, 8);
  if (ok)
    ok = refine_downsample_consistency(hr, in, sw, sh, dw, dh, 2, .45f, 0.f,
                                       has_cm ? &cm : NULL);
  if (ok) {
    remove_hourglass_basis(hr, dw, dh, in, sw, sh, 1.00f, has_cm ? cm.w_hg : NULL);
    suppress_speckle_pm(hr, dw, dh, in, sw, sh, 1.00f, has_cm ? cm.w_hg : NULL);
    /* extra bilinear blend for high checker confidence */
    for (int y=0; y<dh; y++) {
      float sy = (y + 0.5f) * (float)sh / dh - 0.5f;
      int iy = (int)floorf(sy); float fy = sy - iy;
      for (int x=0; x<dw; x++) {
        float sx = (x + 0.5f) * (float)sw / dw - 0.5f;
        int ix = (int)floorf(sx); float fx = sx - ix;
        float chk = 0.f;
        if (has_cm) {
          int cx = clampi((int)floorf(sx + 0.5f), 0, sw-1);
          int cy = clampi((int)floorf(sy + 0.5f), 0, sh-1);
          chk = cm.w_checker[(size_t)cy * sw + cx];
        } else {
          float cell[4][4];
          for (int j=0;j<2;j++) for (int i=0;i<2;i++) raw_pm(in, sw, sh, ix+i, iy+j, cell[j*2+i]);
          chk = checker2x2_confidence_pm(cell);
        }
        if (chk > 0.30f) {
          float t[4];
          bilinear_cell_pm(in, sw, sh, ix, iy, fx, fy, t);
          float *h = hr + 4 * ((size_t)y * dw + x);
          float blend = (chk - 0.30f)/0.70f;
          if (blend>1.f) blend=1.f;
          for (int c=0;c<4;c++) h[c]=h[c]*(1.f-blend)+t[c]*blend;
        }
      }
    }
    write_hr_rgba(hr, dw, dh, out);
  }
  if (has_cm) free_class_map(&cm);
  free(hr);
  return ok;
}

static int upscale_dehourglass(const uint8_t *in, int sw, int sh, uint8_t *out,
                               int dw, int dh) {
  uint8_t *tmp_rgba = malloc((size_t)dw * dh * 4);
  if (!tmp_rgba)
    return 0;
  int ok = upscale_kernel(in, sw, sh, tmp_rgba, dw, dh, 3, kernel_lanczos3);
  if (!ok) {
    free(tmp_rgba);
    return 0;
  }
  float *hr = alloc_hr_from_rgba(tmp_rgba, dw, dh);
  free(tmp_rgba);
  if (!hr)
    return 0;
  class_map_t cm;
  if (!build_class_map(in, sw, sh, &cm)) {
    free(hr);
    return 0;
  }
  /* v7: gated removal + lowpass blend for checker cells + 2-pass hourglass removal.
     First pass 0.95 removes most hourglass, second 0.85 removes residual. Lowpass blend for high checker confidence suppresses bow-tie pattern that appears after upscale. */
  remove_hourglass_basis(hr, dw, dh, in, sw, sh, .95f, cm.w_hg);
  suppress_speckle_pm(hr, dw, dh, in, sw, sh, .95f, cm.w_hg);
  ok = refine_downsample_consistency(hr, in, sw, sh, dw, dh, 1, .35f, 0.f, &cm);
  if (ok) {
    remove_hourglass_basis(hr, dw, dh, in, sw, sh, .85f, cm.w_hg);
    suppress_speckle_pm(hr, dw, dh, in, sw, sh, .85f, cm.w_hg);
    /* v7: extra checker suppression - blend high-checker HR pixels towards bilinear (lowest HG 0.00005) */
    {
      for (int y=0; y<dh; y++) {
        int cy = (int)((y + 0.5f) * (float)sh / dh);
        if (cy<0) cy=0; if (cy>=sh) cy=sh-1;
        for (int x=0; x<dw; x++) {
          int cx = (int)((x + 0.5f) * (float)sw / dw);
          if (cx<0) cx=0; if (cx>=sw) cx=sw-1;
          size_t k = (size_t)cy * sw + cx;
          float wc = cm.w_checker[k];
          if (wc > 0.35f) {
            float t[4];
            float sx = (x + 0.5f) * (float)sw / dw - 0.5f;
            float sy = (y + 0.5f) * (float)sh / dh - 0.5f;
            int ix = (int)floorf(sx), iy = (int)floorf(sy);
            float fx = sx - ix, fy = sy - iy;
            bilinear_cell_pm(in, sw, sh, ix, iy, fx, fy, t);
            float *h = hr + 4 * ((size_t)y * dw + x);
            float blend = (wc - 0.35f) / 0.65f;
            if (blend>1.f) blend=1.f;
            for (int c=0;c<4;c++) h[c] = h[c]*(1.f-blend) + t[c]*blend;
          }
        }
      }
    }
    write_hr_rgba(hr, dw, dh, out);
  }
  free_class_map(&cm);
  free(hr);
  return ok;
}

/* deblurcompress is now the iterative inverse-filter/back-projection mode.  It
   intentionally no longer calls safecompress/compress, avoiding the local 2x2
   triangle geometry entirely. */
static int upscale_deblurcompress(const uint8_t *in, int sw, int sh,
                                  uint8_t *out, int dw, int dh) {
  return upscale_deconv(in, sw, sh, out, dw, dh);
}


/* Self-supervised parameter search for blurcompress.  With no high-resolution
   truth image available, "optimal" means best under an internal validation
   proxy: downsample the input image by 2x in premultiplied-linear RGBA, upscale
   that validation input back to the original input size with candidate
   blur/strength values, then pick the pair with the lowest premultiplied-linear
   RGBA MSE against the original input.  This tunes the filter to the image's
   own edge/gradient statistics instead of using one fixed hand-picked pair. */
static float overlap1(float a0, float a1, float b0, float b1) {
  float lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
  return hi > lo ? hi - lo : 0.f;
}

static uint8_t *downsample_pm_box(const uint8_t *in, int sw, int sh, int dw,
                                  int dh) {
  uint8_t *out = malloc((size_t)dw * dh * 4);
  if (!out)
    return NULL;
  for (int y = 0; y < dh; y++) {
    float sy0 = (float)y * sh / dh, sy1 = (float)(y + 1) * sh / dh;
    int iy0 = clampi((int)floorf(sy0), 0, sh - 1),
        iy1 = clampi((int)ceilf(sy1), 0, sh);
    for (int x = 0; x < dw; x++) {
      float sx0 = (float)x * sw / dw, sx1 = (float)(x + 1) * sw / dw;
      int ix0 = clampi((int)floorf(sx0), 0, sw - 1),
          ix1 = clampi((int)ceilf(sx1), 0, sw);
      float q[4] = {0, 0, 0, 0}, area = 0.f;
      for (int yy = iy0; yy < iy1; yy++) {
        float wy = overlap1(sy0, sy1, (float)yy, (float)yy + 1.f);
        for (int xx = ix0; xx < ix1; xx++) {
          float wx = overlap1(sx0, sx1, (float)xx, (float)xx + 1.f);
          float ww = wx * wy, p[4];
          raw_pm(in, sw, sh, xx, yy, p);
          area += ww;
          for (int c = 0; c < 4; c++)
            q[c] += ww * p[c];
        }
      }
      if (area > 1e-20f)
        for (int c = 0; c < 4; c++)
          q[c] /= area;
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
  return out;
}

static double image_pm_mse(const uint8_t *a, const uint8_t *b, int w, int h,
                           int border) {
  if (border * 2 >= w || border * 2 >= h)
    border = 0;
  double e = 0.0, n = 0.0;
  for (int y = border; y < h - border; y++)
    for (int x = border; x < w - border; x++) {
      float pa[4], pb[4];
      raw_pm(a, w, h, x, y, pa);
      raw_pm(b, w, h, x, y, pb);
      for (int c = 0; c < 4; c++) {
        double d = (double)pa[c] - pb[c];
        e += d * d;
        n += 1.0;
      }
    }
  return n > 0.0 ? e / n : 1e300;
}

static int auto_tune_blurcompress_params(const uint8_t *in, int sw, int sh,
                                         int auto_mode) {
  int tw = sw / 2, th = sh / 2;
  if (tw < 4 || th < 4) {
    fprintf(stderr,
            "Auto blurcompress needs an input of at least 8x8 pixels for its "
            "internal 2x validation image.\n");
    return 0;
  }
  uint8_t *train = downsample_pm_box(in, sw, sh, tw, th);
  uint8_t *recon = malloc((size_t)sw * sh * 4);
  if (!train || !recon) {
    free(train);
    free(recon);
    fprintf(stderr, "Auto blurcompress allocation failed\n");
    return 0;
  }

  static const float radii[] = {.10f, .20f, .35f, .50f, .70f,
                                1.0f, 1.4f, 2.0f, 2.8f, 4.0f};
  static const float strengths[] = {1.0f, 1.25f, 1.6f, 2.0f, 2.6f,
                                    3.5f, 5.0f, 7.5f, 11.0f, 16.0f};
  float old_radius = blur_radius, old_strength = compress_strength;
  float best_radius = old_radius, best_strength = old_strength;
  double best = 1e300;
  for (size_t r = 0; r < sizeof radii / sizeof radii[0]; r++)
    for (size_t s = 0; s < sizeof strengths / sizeof strengths[0]; s++) {
      if (auto_mode == 2 &&
          !((r == 2 || r == 4 || r == 5 || r == 6) &&
            (s == 2 || s == 3 || s == 4 || s == 5 || s == 6)))
        continue;
      blur_radius = radii[r];
      compress_strength = strengths[s];
      int ok = 1;
      if (auto_mode == 2)
        ok = upscale_deblurcompress(train, tw, th, recon, sw, sh);
      else if (auto_mode == 1)
        upscale_safeblurcompress(train, tw, th, recon, sw, sh);
      else
        upscale_blurcompress(train, tw, th, recon, sw, sh);
      double score = ok ? image_pm_mse(recon, in, sw, sh, 2) : 1e300;
      if (score < best) {
        best = score;
        best_radius = blur_radius;
        best_strength = compress_strength;
      }
    }

  blur_radius = best_radius;
  compress_strength = best_strength;
  free(train);
  free(recon);
  fprintf(stderr,
          "Auto selected blur-radius=%.2f strength=%.2f "
          "(validation MSE %.8g).\n",
          blur_radius, compress_strength, best);
  (void)old_radius;
  (void)old_strength;
  return best < 1e299;
}


/* Deliberately discrete triangle baseline: useful for judging the
   source-cell faceting that aggressive reconstruction can introduce. */
static void upscale_triangle(const uint8_t *in, int sw, int sh, uint8_t *out,
                             int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    int iy = (int)floorf(sy);
    float fy = sy - iy;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f;
      int ix = (int)floorf(sx);
      float fx = sx - ix, p[4][4], l[4], q[4];
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          int k = j * 2 + i;
          raw_pm(in, sw, sh, ix + i, iy + j, p[k]);
          l[k] = .2126f * p[k][0] + .7152f * p[k][1] + .0722f * p[k][2] +
                 .5f * p[k][3];
        }
      float gx = l[1] + l[3] - l[0] - l[2], gy = l[2] + l[3] - l[0] - l[1];
      const float *a, *b, *c;
      float wa, wb, wc;
      if (gx * gy < 0) {
        if (fx >= fy) {
          a = p[0];
          b = p[1];
          c = p[3];
          wa = 1 - fx;
          wb = fx - fy;
          wc = fy;
        } else {
          a = p[0];
          b = p[2];
          c = p[3];
          wa = 1 - fy;
          wb = fy - fx;
          wc = fx;
        }
      } else {
        if (fx + fy <= 1) {
          a = p[0];
          b = p[1];
          c = p[2];
          wa = 1 - fx - fy;
          wb = fx;
          wc = fy;
        } else {
          a = p[3];
          b = p[2];
          c = p[1];
          wa = fx + fy - 1;
          wb = 1 - fx;
          wc = 1 - fy;
        }
      }
      for (int k = 0; k < 4; k++)
        q[k] = wa * a[k] + wb * b[k] + wc * c[k];
      put(out + 4 * ((size_t)y * dw + x), q[0], q[1], q[2], q[3]);
    }
  }
}

/* v6: supersampled smooth mode - 4x4 area avg of triangle, spread = -r.
   2x2 already kills most treads (jump 0.023), 4x4 ~0.01 and -r expands
   footprint for visible control. Guaranteed no staircase, softest. */
static void upscale_smooth(const uint8_t *in, int sw, int sh, uint8_t *out,
                           int dw, int dh) {
  float spread = 1.f;
  if (blur_radius_set) spread = clampf(blur_radius, 0.5f, 3.5f);
  int SS;
  if (spread <= 0.8f) SS = 2;
  else if (spread <= 1.3f) SS = 4;
  else if (spread <= 2.2f) SS = 6;
  else SS = 8;
  float inv = 1.f / (float)(SS * SS);
  for (int y = 0; y < dh; y++) {
    for (int x = 0; x < dw; x++) {
      float acc[4] = {0,0,0,0};
      for (int sy_sub = 0; sy_sub < SS; sy_sub++) {
        for (int sx_sub = 0; sx_sub < SS; sx_sub++) {
          float ox = (sx_sub + 0.5f) / (float)SS;
          float oy = (sy_sub + 0.5f) / (float)SS;
          float ox_eff = 0.5f + (ox - 0.5f) * spread;
          float oy_eff = 0.5f + (oy - 0.5f) * spread;
          float sx = ((float)x + ox_eff) * (float)sw / dw - 0.5f;
          float sy = ((float)y + oy_eff) * (float)sh / dh - 0.5f;
          int ix = (int)floorf(sx), iy = (int)floorf(sy);
          float fx = sx - ix, fy = sy - iy;
          float p00[4], p10[4], p01[4], p11[4];
          raw_pm(in, sw, sh, ix,     iy,     p00);
          raw_pm(in, sw, sh, ix + 1, iy,     p10);
          raw_pm(in, sw, sh, ix,     iy + 1, p01);
          raw_pm(in, sw, sh, ix + 1, iy + 1, p11);
          float w00 = (1.f - fx) * (1.f - fy);
          float w10 = fx * (1.f - fy);
          float w01 = (1.f - fx) * fy;
          float w11 = fx * fy;
          for (int k = 0; k < 4; k++) {
            acc[k] += inv * (w00 * p00[k] + w10 * p10[k] + w01 * p01[k] + w11 * p11[k]);
          }
        }
      }
      put(out + 4 * ((size_t)y * dw + x), acc[0], acc[1], acc[2], acc[3]);
    }
  }
}

static int upscale(const uint8_t *in, int sw, int sh, uint8_t *out, int dw,
                   int dh) {
  int *xi = malloc((size_t)dw * 4 * sizeof *xi),
      *yr = malloc((size_t)dh * 4 * sizeof *yr);
  float *wx = malloc((size_t)dw * 4 * sizeof *wx),
        *wy = malloc((size_t)dh * 4 * sizeof *wy);
  float *cache = malloc((size_t)4 * dw * 4 * sizeof *cache);
  int tag[4] = {INT_MIN, INT_MIN, INT_MIN, INT_MIN};
  if (!xi || !yr || !wx || !wy || !cache) {
    free(xi);
    free(yr);
    free(wx);
    free(wy);
    free(cache);
    return 0;
  }
  for (int x = 0; x < dw; x++) {
    float s = (x + .5f) * (float)sw / dw - .5f;
    int i = (int)floorf(s);
    weights(s - i, wx + 4 * x);
    for (int k = 0; k < 4; k++)
      xi[4 * x + k] = clampi(i + k - 1, 0, sw - 1);
  }
  for (int y = 0; y < dh; y++) {
    float s = (y + .5f) * (float)sh / dh - .5f;
    int i = (int)floorf(s);
    weights(s - i, wy + 4 * y);
    for (int k = 0; k < 4; k++)
      yr[4 * y + k] = clampi(i + k - 1, 0, sh - 1);
  }
  for (int y = 0; y < dh; y++) {
    float *q[4];
    for (int j = 0; j < 4; j++) {
      int slot = -1;
      for (int k = 0; k < 4; k++)
        if (tag[k] == yr[4 * y + j])
          slot = k;
      if (slot < 0) {
        slot = 0;
        for (int k = 1; k < 4; k++)
          if (tag[k] < tag[slot])
            slot = k;
        hfilter(in, sw, yr[4 * y + j], dw, xi, wx,
                cache + (size_t)slot * dw * 4);
        tag[slot] = yr[4 * y + j];
      }
      q[j] = cache + (size_t)slot * dw * 4;
    }
    for (int x = 0; x < dw; x++) {
      float v[4] = {0, 0, 0, 0}, lo[4], hi[4];
      for (int c = 0; c < 4; c++) {
        lo[c] = hi[c] = q[1][4 * x + c];
        for (int j = 0; j < 4; j++)
          v[c] += q[j][4 * x + c] * wy[4 * y + j];
      }
      /* Horizontal passes were bounded to their central pair; bound this
         vertical pass to its central pair. Thus the final value is bounded
         by the 2x2 neighbourhood, eliminating cubic overshoot. */
      for (int c = 0; c < 4; c++) {
        for (int j = 1; j <= 2; j++) {
          float z = q[j][4 * x + c];
          if (z < lo[c])
            lo[c] = z;
          if (z > hi[c])
            hi[c] = z;
        }
        v[c] = clampf(v[c], lo[c], hi[c]);
      }
      put(out + ((size_t)y * dw + x) * 4, v[0], v[1], v[2], v[3]);
    }
  }
  free(xi);
  free(yr);
  free(wx);
  free(wy);
  free(cache);
  return 1;
}
/* Input hygiene for web-asset sources (v4.2, default on).
   Lossy WebP encoders leave garbage in fully- and nearly-transparent
   pixels: random bright RGB under alpha 0, plus semi-transparent salt
   (alpha 1..~150) sprinkled over "empty" regions.  Straight resampling
   reproduces that as confetti and every sharpening mode amplifies it.
   alpha_despeckle (threshold from --alpha-clean/-A, 0 disables):
   1. alpha == 0         -> zero RGB (cosmetic; pm math ignores it anyway);
   2. 0 < alpha <= floor -> zero unless part of a connected faint region
      (needs >2 of 8 neighbours above floor);
   3. isolated salt      -> zero mid-alpha specks: alpha in (floor, 160],
      alpha brighter than every neighbour by > 3x + 24 and no neighbour
      reaching half its alpha.  Genuine dots/sparkles are >= 2 px and
      always have a comparable-alpha neighbour, so they survive. */
static float alpha_clean_floor = 10.f;
static void alpha_despeckle(uint8_t *px, int w, int h, int floor_) {
  size_t n = (size_t)w * h;
  for (size_t k = 0; k < n; k++)
    if (px[4 * k + 3] == 0)
      px[4 * k + 0] = px[4 * k + 1] = px[4 * k + 2] = 0;
  if (floor_ <= 0)
    return;
  uint8_t *al = malloc(n), *zero = malloc(n);
  if (!al || !zero) {
    free(al);
    free(zero);
    return;
  }
  for (size_t k = 0; k < n; k++)
    al[k] = px[4 * k + 3];
  memset(zero, 0, n);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      size_t k = (size_t)y * w + x;
      int a = al[k];
      if (!a)
        continue;
      int above = 0, nmax = 0, half = 0;
      for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
          if (!i && !j)
            continue;
          int ax = x + i, ay = y + j;
          if (ax < 0 || ax >= w || ay < 0 || ay >= h)
            continue;
          int na = al[(size_t)ay * w + ax];
          if (na > floor_)
            above++;
          if (na > nmax)
            nmax = na;
          if (na >= (a + 1) / 2)
            half++;
        }
      if (a <= floor_) {
        if (above <= 2)
          zero[k] = 1;
      } else if (a <= 160 && a > 3 * nmax + 24 && !half)
        zero[k] = 1;
    }
  for (size_t k = 0; k < n; k++)
    if (zero[k])
      memset(px + 4 * k, 0, 4);
  free(al);
  free(zero);
}
static uint8_t *slurp(const char *name, size_t *n) {
  FILE *f = fopen(name, "rb");
  long z;
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) || ((z = ftell(f)) <= 0) || fseek(f, 0, SEEK_SET)) {
    fclose(f);
    return NULL;
  }
  uint8_t *p = malloc((size_t)z);
  if (!p || fread(p, 1, z, f) != (size_t)z) {
    free(p);
    p = NULL;
  }
  fclose(f);
  *n = (size_t)z;
  return p;
}
static void print_help(const char *argv0) {
  printf(
      "celup_lab -- premultiplied-linear WebP upscaler (research build "
      "v7.3-merge)\n"
      "\n"
      "Usage: %s in.webp out.webp SCALE [options]\n"
      "  SCALE is the upsampling factor, real number in (1,32] "
      "(e.g. 2, 3, 4.5, 10).\n"
      "\n"
      "Modes (-m MODE, default cubic) -- recommended:\n"
      "  adaptive      natural images and mixed art; classifies each patch\n"
      "                (edge/checker/junction/line/gradient) and picks a\n"
      "                safe interpolation policy per patch; -r controls\n"
      "                lowpass sigma + tangential spread, -s sharpness\n"
      "  autoblur      fits the blur the source was probably downsampled\n"
      "                through and renders at target resolution -- smooth,\n"
      "                round contours at high scale, no sawtooth; -k/-c/-r pin\n"
      "  autodeblur    autoblur base + gradient-slope steepening: sharp\n"
      "                edges with no halos/ringing; flats gently cleaned.\n"
      "                Best for AI-upscaled/diffusion anime art; -g/-s/-r/-D/-k/-c honored\n"
      "  triangle      soft, no ringing or halos; safe default for art\n"
      "  smooth        supersampled triangle (4x4 area avg), -r controls\n"
      "                extra spread (0.5..3); guaranteed no staircase, softest\n"
      "  sdf           fitted signed-distance contour sharpening on top of\n"
      "                adaptive; crispest edges, tune with -s\n"
      "  nearest       pixel art / hard 1px texture\n"
      "  jinc2_bilateral  Hyllian Jinc2-Bilateral: windowed-jinc 2-lobe +\n"
      "                bilateral edge-preserving reconstruction (any scale);\n"
      "                low ringing / hourglass, smooth diagonals\n"
      "  jinc2_auto    jinc2_bilateral with WB/STR auto-tuned per image by a\n"
      "                2x-downscale proxy (picks the lowest-stepladder pair\n"
      "                among near-best reconstruction MSE)\n"
      "  xbrz          Zenju xBRZ 1.8: cellular-automata pixel-art upscaler;\n"
      "                integer scale 2..6, sharp corners, no halos\n"
      "  xbr           Hyllian xBR: simpler cellular-automata pixel-art upscaler;\n"
      "                integer scale 2..4, sharp corners, no halos\n"
      "  hybrid        auto-classify image type and pick best mode\n"
      "                (pixel art -> xBRZ, natural -> adaptive, line art -> autodeblur)\n"
      "\n"
      "Modes -- other (accepted, but expect artifacts on art):\n"
      "  bilinear linear cubic mitchell lanczos2 lanczos3 blur\n"
      "  compress safecompress blurcompress safeblurcompress edgecompress\n"
      "  deblurcompress dehourglass consistentcompress hourglasscompress\n"
      "  scale2x classmap   (classmap = diagnostic class-map dump)\n"
      "\n"
      "Options (general):\n"
      "  -m, --mode MODE           one of the modes listed above\n"
      "  -A, --alpha-clean T       transparent-pixel garbage cleanup 0..64\n"
      "                            (default 10; 0 disables): wipes hidden-RGB\n"
      "                            and isolated semi-transparent salt left by\n"
      "                            lossy encoders in empty regions\n"
      "  -M, --max-mib M           memory budget in MiB 32..65536 (default\n"
      "                            512); sdf/adaptive need roughly 40..80 B\n"
      "                            per output pixel -- big images at 3x+ need\n"
      "                            -M 2048 or more\n"
      "  -h, --help                this text\n"
      "\n"
      "Options (compress family / adaptive / sdf):\n"
      "  -s, --strength N          compress/sharpen strength 1..100 (default\n"
      "                            4); adaptive: sharp=0.02*(N-1) cap 0.90\n"
      "                            autodeblur: k=1+.25*(N-1) cap 16, honored\n"
      "                            with -g override; all modes respect -s\n"
      "  -r, --blur-radius R       blur radius .1..40 (default 1): radius\n"
      "                            for blurcompress; pins autoblur sigma;\n"
      "                            autodeblur: ASSUMED source blur (-r is\n"
      "                            minimal blur hiding lattice staircase);\n"
      "                            adaptive: lowpass sigma 0.1..2 plus tangential\n"
      "                            AA spread; smooth: spread 0.5..3 (area avg)\n"
      "  -a, --auto-tune           auto-tune -r and -s for the *blurcompress\n"
      "                            modes only (implies -m deblurcompress)\n"
      "  -P, --checker-policy P    adaptive checker policy: lowpass|bilinear|\n"
      "                            nearest|mitchell|scale2x|auto\n"
      "  -d, --adaptive-debug N    class-map debug bitmask 0..15: 1 drops\n"
      "                            the edge class, 2 checker, 4 junction,\n"
      "                            8 line; 0 = normal render\n"
      "\n"
      "Options (autoblur / autodeblur):\n"
      "  -k, --blur-kernel K       kernel box|triangle|gaussian|bspline|auto\n"
      "                            (default auto = fit)\n"
      "  -c, --blur-curve C        gradient curve linear|sigmoid|cubic|exp|\n"
      "                            log|sqrt|circle|nearest|auto (default fit)\n"
      "  -p, --curve-param P       curve shape parameter 0..40 (0 = family\n"
      "                            default/fit; exp/log = k, sqrt = p)\n"
      "  -e, --edge-goal W         goal width for strong edges in src px\n"
      "                            0..8 (default 0=off): escalates the\n"
      "                            autoblur fit toward enough blur for smooth\n"
      "                            edges, and adapts autodeblur steepness\n"
      "                            per edge\n"
      "  -D, --deblur-method M     autodeblur method auto|remap|push|compress2x2\n"
      "                            (v4.9.2: 'remake' accepted as alias;\n"
      "                            default auto = 2x proxy picks per image);\n"
      "                            remap = evaluate the slope-steepened\n"
      "                            profile fit at the pixel's own position,\n"
      "                            push = evaluate the original fit at a\n"
      "                            position displaced toward the nearer\n"
      "                            plateau (Anime4K push)\n"
      "  -T, --texgain G           autodeblur lattice-texture crispening\n"
      "                            FLOAT 0..1 (default 0=off).  Hull-clamped\n"
      "                            crispening applied only where the 1D step\n"
      "                            model abstained (AUTODEBLUR_NOTES queue #1).\n"
      "  -g, --deblur-steepness K  autodeblur slope multiplier FLOAT 1..64\n"
      "                            (default 0=auto); overrides -s and -e\n"
      "                            per-edge adaptation; anchored evaluation\n"
      "                            keeps colour noise at gain 1, so K far\n"
      "                            above 8 stays artifact-safe\n"
      "\n"
      "autoblur/autodeblur: what is automatic, and how to pin it\n"
      "  parameter    chosen automatically by...        pin manually with\n"
      "  kernel       validation-proxy fit              -k (any value but auto)\n"
      "  sigma        fit, then -e escalation           -r R (exact; -e then\n"
      "                                                 backs off: manual wins).\n"
      "                                                 autodeblur: R = assumed\n"
      "                                                 blur; base render sigma\n"
      "                                                 = R/min(K,8) (v4.9)\n"
      "  curve        validation-proxy fit              -c (any value but auto)\n"
      "  curve param  fit                               -p P\n"
      "  method       2x-downscale proxy MSE            -D remap|push\n"
      "  steepness    -s formula, or -e per edge        -g K (exact float,\n"
      "                                                 1..64)\n"
      "  Only the unpinned parameters are fitted.  Every effective value is\n"
      "  echoed to stderr, so an automatic run can be reproduced exactly by\n"
      "  re-running with its reported values pinned.\n"
      "\n"
      "Options (jinc2_bilateral / jinc2_auto) -- all in [0,1]:\n"
      "  --j2b-wa  A   window A (default .50)\n"
      "  --j2b-wb  B   window B (default .88); lower B smooths diagonals\n"
      "  --j2b-str S   bilateral strength (default 1.0); lower S reduces\n"
      "                stepladder on hard pixel art\n"
      "  --j2b-ar  R   anti-ringing (default 1.0); lower R relaxes clamp\n"
      "\n"
      "autodeblur internals (v4.9): one gradient direction per pixel for\n"
      "the whole premultiplied RGBA vector (4D structure tensor);\n"
      "transition lobes along that normal are segmented (|du| runs) and\n"
      "each pixel is fit by an error-function profile on ITS OWN lobe\n"
      "with one-sided plateau colours -- a line's two flanks and two\n"
      "backgrounds never mix into one fit.  The slope is steepened ON\n"
      "THE FIT by k and the fit is evaluated at the pixel's GEOMETRIC\n"
      "position, with the pixel's own fit residual re-added at gain 1:\n"
      "colours stay anchored (no k-amplified speckle, no phi^-1 halo,\n"
      "hue texture kept), misassigned lobes degrade to identity, and k\n"
      "may be a large float.  k is capped so output ramps stay >= .6 px\n"
      "sigma (no re-aliased sawtooth); dense multi-crossing windows\n"
      "(text, crosshatch) are left alone via a window-level crossing-\n"
      "count trust gate.  v4.9: (1) a junction measure from the SAME\n"
      "tensor (lambda2/lambda1) scales the tangent span of sampling and\n"
      "the pass-2 smoothing -- straight contours keep full tangential\n"
      "averaging, corners/tips keep their own radial fit and stay\n"
      "SHARP; (2) the base reconstruction sigma is decoupled from the\n"
      "assumed blur (-r) to max(.6, r/min(K,8)), so low-trust pixels\n"
      "fall back to a crisp source sample, never to a sigma-wide skirt\n"
      "(-- the v4.8 'neon glow' at mismatched -r).  -r is ASSUMED\n"
      "SOURCE BLUR in this mode: grossly oversetting it (hard pixelated\n"
      "source at -r 6) oversizes windows; moderate values (~1-2.3)\n"
      "behave best on art with light antialiasing.\n"
      "\n"
      "Output is always lossless WebP.  Hourglass/speckle suppression in the\n"
      "compress family and adaptive/sdf only acts on directed gradients;\n"
      "symmetric content (dots, checkerboards, junctions) is passed through\n"
      "verbatim.\n",
      argv0);
}

/* Intelligent multi-class classifier for hybrid mode */
typedef enum {
  IMG_TYPE_PIXEL_ART = 0,
  IMG_TYPE_LINE_ART = 1,
  IMG_TYPE_NATURAL_PHOTO = 2
} image_type_t;

static image_type_t classify_image_type(const uint8_t *in, int sw, int sh) {
  size_t n_pixels = (size_t)sw * sh;
  size_t sample_step = n_pixels / 50000 + 1;
  uint32_t unique_colors[512];
  int num_unique = 0;
  for (size_t i = 0; i < n_pixels; i += sample_step) {
    const uint8_t *p = in + i * 4;
    uint32_t c = ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    int found = 0;
    for (int k = 0; k < num_unique; k++) {
      if (unique_colors[k] == c) { found = 1; break; }
    }
    if (!found) {
      if (num_unique < 512) unique_colors[num_unique++] = c;
      else { num_unique = 513; break; }
    }
  }
  if (num_unique <= 256 && sw <= 512 && sh <= 512)
    return IMG_TYPE_PIXEL_ART;
  class_map_t cm;
  if (build_class_map(in, sw, sh, &cm)) {
    double sum_edge = 0.0, sum_coh = 0.0;
    size_t count = 0;
    for (size_t k = 0; k < n_pixels; k += sample_step) {
      float e = cm.w_edge[k], c = cm.w_checker[k];
      sum_edge += e;
      if (e > 0.10f && c < 0.3f) {
        float gx = cm.edge_gx[k], gy = cm.edge_gy[k];
        float g2 = gx * gx + gy * gy;
        if (g2 > 0.002f) sum_coh += e;
      }
      count++;
    }
    free_class_map(&cm);
    double avg_edge = sum_edge / (count ? count : 1);
    double avg_coh = sum_coh / (count ? count : 1);
    if (avg_coh > 0.0005 || avg_edge > 0.005)
      return IMG_TYPE_LINE_ART;
  }
  return IMG_TYPE_NATURAL_PHOTO;
}

static int upscale_xbrz(const uint8_t *in, int sw, int sh, uint8_t *out,
                        int dw, int dh) {
  int factor = dw / sw;
  if (factor < 2) factor = 2;
  if (factor > 6) factor = 6;
  return xbrz_scale(in, sw, sh, out, dw, dh, factor) == 0;
}

static int upscale_xbr(const uint8_t *in, int sw, int sh, uint8_t *out,
                       int dw, int dh) {
  int factor = dw / sw;
  if (factor < 2) factor = 2;
  if (factor > 4) factor = 4;
  return xbr_scale(in, sw, sh, out, dw, dh, factor) == 0;
}

static int upscale_hybrid(const uint8_t *in, int sw, int sh, uint8_t *out,
                          int dw, int dh, float scale) {
  image_type_t type = classify_image_type(in, sw, sh);
  int is_int_scale = (fabsf(scale - roundf(scale)) < 1e-4f);
  int factor = (int)roundf(scale);

  if (type == IMG_TYPE_PIXEL_ART && is_int_scale && factor >= 2 && factor <= 6) {
    fprintf(stderr, "hybrid mode: classified as PIXEL_ART -> xBRZ %dx\n", factor);
    return xbrz_scale(in, sw, sh, out, dw, dh, factor) == 0;
  } else if (type == IMG_TYPE_NATURAL_PHOTO) {
    fprintf(stderr, "hybrid mode: classified as NATURAL_PHOTO -> adaptive mode\n");
    return upscale_adaptive(in, sw, sh, out, dw, dh);
  } else {
    fprintf(stderr, "hybrid mode: classified as LINE_ART / DRAWING -> autodeblur mode\n");
    return upscale_autodeblur(in, sw, sh, out, dw, dh);
  }
}
int main(int ac, char **av) {
  const char *mode = "cubic";
  int mode_explicit = 0;
  for (int i = 1; i < ac; i++)
    if (!strcmp(av[i], "-h") || !strcmp(av[i], "--help")) {
      print_help(av[0]);
      return 0;
    }
  if (ac < 4) {
    fprintf(stderr, "Usage: %s in.webp out.webp scale [options]\n"
                    "Try '%s --help' for modes and options.\n",
            av[0], av[0]);
    return 2;
  }
  for (int i = 4; i < ac;) {
    if (!strcmp(av[i], "--auto-blurcompress") ||
        !strcmp(av[i], "--auto-tune") || !strcmp(av[i], "-a")) {
      auto_blurcompress = 1;
      if (!mode_explicit)
        mode = "deblurcompress";
      i++;
      continue;
    }
    if (i + 1 >= ac) {
      fprintf(stderr, "Missing option value for %s\n", av[i]);
      return 2;
    }
    if (!strcmp(av[i], "--mode") || !strcmp(av[i], "-m")) {
      mode = av[i + 1];
      mode_explicit = 1;
    }
    else if (!strcmp(av[i], "--strength") || !strcmp(av[i], "-s")) {
      char *e;
      compress_strength = strtof(av[i + 1], &e);
      if (*e || compress_strength < 1.f || compress_strength > 100.f) {
        fprintf(stderr, "Strength must be in [1,100]\n");
        return 2;
      }
    } else if (!strcmp(av[i], "--blur-radius") || !strcmp(av[i], "-r")) {
      char *e;
      blur_radius = strtof(av[i + 1], &e);
      if (*e || blur_radius < .1f || blur_radius > 40.f) {
        fprintf(stderr, "Blur radius must be in [.1,40]\n");
        return 2;
      }
      blur_radius_set = 1;
    } else if (!strcmp(av[i], "--blur-kernel") || !strcmp(av[i], "-k")) {
      if (!strcmp(av[i + 1], "box"))
        blur_kernel_kind = BK_BOX;
      else if (!strcmp(av[i + 1], "triangle"))
        blur_kernel_kind = BK_TRIANGLE;
      else if (!strcmp(av[i + 1], "gaussian"))
        blur_kernel_kind = BK_GAUSSIAN;
      else if (!strcmp(av[i + 1], "bspline"))
        blur_kernel_kind = BK_BSPLINE;
      else if (!strcmp(av[i + 1], "auto"))
        blur_kernel_kind = BK_AUTO;
      else {
        fprintf(stderr, "Unknown blur kernel: %s\n", av[i + 1]);
        return 2;
      }
    } else if (!strcmp(av[i], "--blur-curve") || !strcmp(av[i], "-c")) {
      if (!strcmp(av[i + 1], "linear"))
        blur_curve_kind = CK_LINEAR;
      else if (!strcmp(av[i + 1], "sigmoid"))
        blur_curve_kind = CK_SIGMOID;
      else if (!strcmp(av[i + 1], "cubic"))
        blur_curve_kind = CK_CUBIC;
      else if (!strcmp(av[i + 1], "exp"))
        blur_curve_kind = CK_EXP;
      else if (!strcmp(av[i + 1], "log"))
        blur_curve_kind = CK_LOG;
      else if (!strcmp(av[i + 1], "sqrt"))
        blur_curve_kind = CK_SQRT;
      else if (!strcmp(av[i + 1], "circle"))
        blur_curve_kind = CK_CIRCLE;
      else if (!strcmp(av[i + 1], "nearest"))
        blur_curve_kind = CK_NEAREST;
      else if (!strcmp(av[i + 1], "auto"))
        blur_curve_kind = CK_AUTO;
      else {
        fprintf(stderr, "Unknown blur curve: %s\n", av[i + 1]);
        return 2;
      }
    } else if (!strcmp(av[i], "--curve-param") || !strcmp(av[i], "-p")) {
      char *e;
      curve_param = strtof(av[i + 1], &e);
      if (*e || curve_param < 0.f || curve_param > 40.f) {
        fprintf(stderr, "curve-param must be in [0,40]\n");
        return 2;
      }
    } else if (!strcmp(av[i], "--checker-policy") || !strcmp(av[i], "-P")) {
      if (!strcmp(av[i + 1], "lowpass"))
        checker_policy = POLICY_LOWPASS;
      else if (!strcmp(av[i + 1], "bilinear"))
        checker_policy = POLICY_BILINEAR;
      else if (!strcmp(av[i + 1], "nearest"))
        checker_policy = POLICY_NEAREST;
      else if (!strcmp(av[i + 1], "mitchell"))
        checker_policy = POLICY_MITCHELL;
      else if (!strcmp(av[i + 1], "scale2x"))
        checker_policy = POLICY_SCALE2X;
      else if (!strcmp(av[i + 1], "auto"))
        checker_policy = POLICY_AUTO;
      else {
        fprintf(stderr, "Unknown checker policy: %s\n", av[i + 1]);
        return 2;
      }
    } else if (!strcmp(av[i], "--adaptive-debug") || !strcmp(av[i], "-d")) {
      char *e;
      adaptive_debug = (int)strtol(av[i + 1], &e, 10);
      if (*e || adaptive_debug < 0 || adaptive_debug > 15) {
        fprintf(stderr, "adaptive-debug must be in [0,15]\n");
        return 2;
      }
    } else if (!strcmp(av[i], "--alpha-clean") || !strcmp(av[i], "-A")) {
      char *e;
      alpha_clean_floor = strtof(av[i + 1], &e);
      if (*e || alpha_clean_floor < 0.f || alpha_clean_floor > 64.f) {
        fprintf(stderr, "alpha-clean must be in [0,64]\n");
        return 2;
      }
    } else if (!strcmp(av[i], "--edge-goal") || !strcmp(av[i], "-e")) {
      char *e;
      edge_goal = strtof(av[i + 1], &e);
      if (*e || edge_goal < 0.f || edge_goal > 8.f) {
        fprintf(stderr, "edge-goal must be in [0,8] (src px; 0=off)\n");
        return 2;
      }
    } else if (!strcmp(av[i], "--deblur-steepness") || !strcmp(av[i], "-g")) {
      char *e;
      deblur_steepness = strtof(av[i + 1], &e);
      if (*e || (deblur_steepness != 0.f &&
                 (deblur_steepness < 1.f || deblur_steepness > 64.f))) {
        fprintf(stderr,
                "deblur-steepness must be 0 (auto) or in [1,64]\n");
        return 2;
      }
    } else if (!strcmp(av[i], "--deblur-method") || !strcmp(av[i], "-D")) {
      if (!strcmp(av[i + 1], "auto"))
        deblur_method = 0;
      else if (!strcmp(av[i + 1], "remap") ||
               !strcmp(av[i + 1], "remake")) /* common typo of remap */
        deblur_method = 1;
      else if (!strcmp(av[i + 1], "push"))
        deblur_method = 2;
      else if (!strcmp(av[i + 1], "compress2x2") ||
               !strcmp(av[i + 1], "analytical"))
        deblur_method = 3;
      else if (!strcmp(av[i + 1], "gradient"))
        deblur_method = 4;
      else {
        fprintf(stderr, "Unknown deblur method: %s\n", av[i + 1]);
        return 2;
      }
    } else if (!strcmp(av[i], "--j2b-wa") || !strcmp(av[i], "--j2b-wb") ||
               !strcmp(av[i], "--j2b-str") || !strcmp(av[i], "--j2b-ar")) {
      char *e;
      float v = strtof(av[i + 1], &e);
      if (*e || v < 0.f || v > 1.f) {
        fprintf(stderr, "%s must be in [0,1]\n", av[i]);
        return 2;
      }
      if (!strcmp(av[i], "--j2b-wa"))
        j2b_wa = v;
      else if (!strcmp(av[i], "--j2b-wb"))
        j2b_wb = v;
      else if (!strcmp(av[i], "--j2b-str"))
        j2b_str = v;
      else
        j2b_ar = v;
    } else if (!strcmp(av[i], "--max-mib") || !strcmp(av[i], "-M")) {
      char *e;
      max_mib = strtof(av[i + 1], &e);
      if (*e || max_mib < 32.f || max_mib > 65536.f) {
        fprintf(stderr, "max-mib must be in [32,65536]\n");
        return 2;
      }
    } else {
      fprintf(stderr, "Unknown option: %s (see --help)\n", av[i]);
      return 2;
    }
    i += 2;
  }
  if (strcmp(mode, "nearest") && strcmp(mode, "bilinear") &&
      strcmp(mode, "linear") && strcmp(mode, "cubic") && strcmp(mode, "mitchell") &&
      strcmp(mode, "lanczos2") && strcmp(mode, "lanczos3") &&
      strcmp(mode, "compress") && strcmp(mode, "safecompress") &&
      strcmp(mode, "blur") && strcmp(mode, "blurcompress") &&
      strcmp(mode, "safeblurcompress") && strcmp(mode, "edgecompress") &&
      strcmp(mode, "deblurcompress") && strcmp(mode, "dehourglass") &&
      strcmp(mode, "consistentcompress") && strcmp(mode, "hourglasscompress") &&
      strcmp(mode, "triangle") && strcmp(mode, "smooth") && strcmp(mode, "adaptive") &&
      strcmp(mode, "classmap") && strcmp(mode, "scale2x") &&
      strcmp(mode, "autoblur") && strcmp(mode, "sdf") &&
      strcmp(mode, "msdf") && strcmp(mode, "dsdf") &&
      strcmp(mode, "autodeblur") &&
      strcmp(mode, "jinc2_bilateral") && strcmp(mode, "jinc2_auto") &&
      strcmp(mode, "xbrz") && strcmp(mode, "xbr") &&
      strcmp(mode, "hybrid") && strcmp(mode, "smart")) {
    fprintf(stderr, "Unknown mode: %s\n", mode);
    return 2;
  }
  if (auto_blurcompress && strcmp(mode, "blurcompress") &&
      strcmp(mode, "safeblurcompress") && strcmp(mode, "deblurcompress")) {
    fprintf(stderr,
            "--auto-blurcompress only applies to blurcompress, "
            "safeblurcompress, or deblurcompress\n");
    return 2;
  }
  char *end;
  double scale = strtod(av[3], &end);
  if (*end || scale <= 1 || scale > 32) {
    fprintf(stderr, "Scale must be in (1,32]\n");
    return 2;
  }
  init_luts();
  size_t n;
  uint8_t *d = slurp(av[1], &n);
  int w, h;
  if (!d) {
    fprintf(stderr, "Cannot read input\n");
    return 1;
  }
  uint8_t *in = WebPDecodeRGBA(d, n, &w, &h);
  free(d);
  if (!in) {
    fprintf(stderr, "WebP decode failed\n");
    return 1;
  }
  alpha_despeckle(in, w, h, (int)(alpha_clean_floor + .5f));
  int ow = (int)(w * scale + .5), oh = (int)(h * scale + .5);
  if (ow < 1 || oh < 1 || ow > 16384 || oh > 16384 ||
      (size_t)ow * oh > SIZE_MAX / 4) {
    fprintf(stderr, "Invalid output dimensions\n");
    WebPFree(in);
    return 1;
  }
  /* libwebp's convenience encoder needs the full RGBA output plus internal
     working storage and can temporarily hold compressed output. Refuse before
     allocation rather than relying on an OOM-killed process. */
  {
    /* Iterative modes hold float HR buffers (16 B/px plus equal tempo-
       raries); adaptive/classmap hold ~68 B/source pixel of class-map data
       plus an optional 16 B/px lowpass float source. */
    int iterative = !strcmp(mode, "deblurcompress") ||
                    !strcmp(mode, "dehourglass") ||
                    !strcmp(mode, "consistentcompress") ||
                    !strcmp(mode, "hourglasscompress") ||
                    !strcmp(mode, "adaptive");
    int classified = iterative || !strcmp(mode, "classmap") ||
                     !strcmp(mode, "sdf") || !strcmp(mode, "msdf") || !strcmp(mode, "dsdf");
    double in_bytes =
        (double)w * h * (classified ? 96.0 : (!strcmp(mode, "autoblur") ||
                                        !strcmp(mode, "autodeblur") &&
      strcmp(mode, "jinc2_bilateral") && strcmp(mode, "jinc2_auto")) ? 24.0
                                                                      : 4.0);
    double out_bytes =
        (double)ow * oh * (iterative          ? 36.0
                           : !strcmp(mode, "sdf") || !strcmp(mode, "msdf") || !strcmp(mode, "dsdf") ? 36.0 + 40.0 /* adaptive +
                                                       SDF delta/blur buffers */
                           : !strcmp(mode, "autoblur") ? 8.0
                           : !strcmp(mode, "autodeblur")
                               ? 8.0 + 84.0 /* A f16 + DEL f16 + PF f40
                                               + LOH 8 + dst 4 */
                                                       : 4.0);
    double estimated =
        (in_bytes + out_bytes + 32.0 * 1024 * 1024) / (1024.0 * 1024.0);
    if (estimated > max_mib) {
      fprintf(stderr,
              "Refusing %dx%d output: estimated peak %.0f MiB exceeds "
              "--max-mib %.0f MiB. Use a smaller scale, increase --max-mib "
              "only if RAM is available, or use a tiled output backend.\n",
              ow, oh, estimated, max_mib);
      WebPFree(in);
      return 1;
    }
  }
  if (auto_blurcompress &&
      !auto_tune_blurcompress_params(
          in, w, h, !strcmp(mode, "deblurcompress")   ? 2
                    : !strcmp(mode, "safeblurcompress") ? 1
                                                          : 0)) {
    WebPFree(in);
    return 1;
  }
  uint8_t *out = malloc((size_t)ow * oh * 4);
  if (!out) {
    fprintf(stderr, "Allocation failed\n");
    WebPFree(in);
    return 1;
  }
  int ok = 1;
  if (!strcmp(mode, "nearest"))
    upscale_nearest(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "bilinear"))
    upscale_bilinear(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "linear"))
    upscale_linear(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "cubic"))
    ok = upscale(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "mitchell"))
    ok = upscale_kernel(in, w, h, out, ow, oh, 2, kernel_mitchell);
  else if (!strcmp(mode, "lanczos2"))
    ok = upscale_kernel(in, w, h, out, ow, oh, 2, kernel_lanczos2);
  else if (!strcmp(mode, "lanczos3"))
    ok = upscale_kernel(in, w, h, out, ow, oh, 3, kernel_lanczos3);
  else if (!strcmp(mode, "blur"))
    upscale_blur(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "compress"))
    upscale_compress(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "safecompress"))
    upscale_safecompress(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "blurcompress"))
    upscale_blurcompress(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "safeblurcompress"))
    upscale_safeblurcompress(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "edgecompress"))
    upscale_edgecompress(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "deblurcompress"))
    ok = upscale_deblurcompress(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "dehourglass"))
    ok = upscale_dehourglass(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "consistentcompress") || !strcmp(mode, "hourglasscompress"))
    ok = upscale_consistentcompress(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "triangle"))
    upscale_triangle(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "smooth"))
    upscale_smooth(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "adaptive"))
    ok = upscale_adaptive(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "classmap"))
    ok = upscale_classmap(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "scale2x"))
    upscale_scale2x(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "autoblur"))
    ok = upscale_autoblur(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "autodeblur") &&
      strcmp(mode, "jinc2_bilateral") && strcmp(mode, "jinc2_auto"))
    ok = upscale_autodeblur(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "sdf"))
    ok = upscale_sdf(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "msdf"))
    ok = upscale_msdf(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "dsdf"))
    ok = upscale_dsdf(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "jinc2_bilateral")) {
    upscale_jinc2_bilateral(in, w, h, out, ow, oh);
    ok = 1;
  }
  else if (!strcmp(mode, "jinc2_auto")) {
    auto_tune_j2b(in, w, h);
    upscale_jinc2_bilateral(in, w, h, out, ow, oh);
    ok = 1;
  }
  else if (!strcmp(mode, "hybrid") || !strcmp(mode, "smart")) {
    ok = upscale_hybrid(in, w, h, out, ow, oh, (float)scale);
  }
  else if (!strcmp(mode, "xbrz")) {
    int f = (int)roundf((float)scale);
    if (fabsf((float)scale - f) < 1e-4f && f >= 2 && f <= 6)
      ok = upscale_xbrz(in, w, h, out, ow, oh);
    else {
      fprintf(stderr, "xbrz: scale %.2f not integer in [2,6]; fallback to bilinear\n", scale);
      upscale_bilinear(in, w, h, out, ow, oh);
      ok = 1;
    }
  }
  else if (!strcmp(mode, "xbr")) {
    int f = (int)roundf((float)scale);
    if (fabsf((float)scale - f) < 1e-4f && f >= 2 && f <= 4)
      ok = upscale_xbr(in, w, h, out, ow, oh);
    else {
      fprintf(stderr, "xbr: scale %.2f not integer in [2,4]; fallback to bilinear\n", scale);
      upscale_bilinear(in, w, h, out, ow, oh);
      ok = 1;
    }
  }
  if (!ok) {
    fprintf(stderr, "Allocation failed\n");
    free(out);
    WebPFree(in);
    return 1;
  }
  WebPFree(in);
  uint8_t *enc;
  size_t z = WebPEncodeLosslessRGBA(out, ow, oh, ow * 4, &enc);
  free(out);
  FILE *f = z ? fopen(av[2], "wb") : NULL;
  if (!f || fwrite(enc, 1, z, f) != z) {
    fprintf(stderr, "Write failed\n");
    if (f)
      fclose(f);
    WebPFree(enc);
    return 1;
  }
  fclose(f);
  WebPFree(enc);
  printf("Done: %dx%d -> %dx%d, mode=%s, strength=%.2f, blur-radius=%.2f", w,
         h, ow, oh, mode, compress_strength, blur_radius);
  if (!strcmp(mode, "adaptive")) {
    static const char *names[] = {"lowpass", "bilinear", "nearest",
                                  "mitchell", "scale2x", "auto"};
    printf(", checker-policy=%s", names[checker_policy]);
  } else if (!strcmp(mode, "autoblur") || !strcmp(mode, "autodeblur") &&
      strcmp(mode, "jinc2_bilateral") && strcmp(mode, "jinc2_auto")) {
    printf(", kernel=%s sigma=%.2f curve=%s param=%.2f",
           kernel_name(fitted_kernel), fitted_sigma, curve_name(fitted_curve),
           fitted_cp);
    if (!strcmp(mode, "autodeblur") &&
      strcmp(mode, "jinc2_bilateral") && strcmp(mode, "jinc2_auto")) {
      printf(", method=%s", last_deblur_method == 2 ? "push" : last_deblur_method == 3 ? "analytical" : last_deblur_method == 4 ? "gradient" : "remap");
      if (last_deblur_k > 0.f)
        printf(", steepness=%.2f%s", last_deblur_k,
               deblur_steepness > 0.f ? "(manual)" : "");
      else
        printf(", steepness=adaptive(-e %.2f)", edge_goal);
    }
  }
  printf("\n");
  return 0;
}
