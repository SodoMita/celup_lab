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
        float chk = checker2x2_confidence_pm(cell);
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
  if (r > 12)
    r = 12;
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
  return best;
}

static int same_colour_pm(const float a[4], const float b[4]) {
  return dist4_pm(a, b) < 1e-6f;
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
        float plane_conf = 1.f - ramp01(plane_mse, .010f, .045f);
        float plane_r2_conf = ramp01(plane_r2, .55f, .85f);
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
        junction_conf = contrast_conf * (1.f - line_conf * plane_conf) *
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
  int r = clampi((int)ceilf(3.f * sigma), 1, 8);
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
  float P[4], B[4], D[4], E[4], F[4];
  raw_pm(in, sw, sh, cx, cy, P);
  raw_pm(in, sw, sh, cx, cy - 1, B);
  raw_pm(in, sw, sh, cx - 1, cy, D);
  raw_pm(in, sw, sh, cx + 1, cy, E);
  raw_pm(in, sw, sh, cx, cy + 1, F);
  const float *r = P;
  if (!same_colour_pm(B, E) && !same_colour_pm(D, F)) {
    int right = sx >= (float)cx, down = sy >= (float)cy;
    if (!right && !down)
      r = same_colour_pm(D, B) ? D : P;
    else if (right && !down)
      r = same_colour_pm(B, E) ? E : P;
    else if (!right && down)
      r = same_colour_pm(D, F) ? D : P;
    else
      r = same_colour_pm(E, F) ? E : P;
  }
  q[0] = r[0];
  q[1] = r[1];
  q[2] = r[2];
  q[3] = r[3];
}

static void upscale_scale2x(const uint8_t *in, int sw, int sh, uint8_t *out,
                            int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    float sy = (y + .5f) * (float)sh / dh - .5f;
    for (int x = 0; x < dw; x++) {
      float sx = (x + .5f) * (float)sw / dw - .5f, q[4];
      scale2x_sample(in, sw, sh, sx, sy, q);
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

/* v5.1: tangential AA for adaptive to kill remaining 45deg staircase.
   For edge pixels (w_edge high, checker/junction low), smooth along the
   contour tangent (perpendicular to gradient) with a small 2-tap blend.
   This does not blur across the edge, only along it, so treads dissolve. */
static void adaptive_tangential_aa(float *hr, int dw, int dh,
                                   const class_map_t *cm, int sw, int sh) {
  size_t n = (size_t)dw * dh;
  float *snap = malloc(n * 4 * sizeof *snap);
  if (!snap) return;
  memcpy(snap, hr, n * 4 * sizeof *snap);
  float xscale = (float)sw / dw, yscale = (float)sh / dh;
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
      if (edge_w < 0.30f) continue;
      if (wc > 0.25f) continue;
      if (wj > 0.30f) continue;
      float gx = cm->edge_gx ? cm->edge_gx[k] : 0.f;
      float gy = cm->edge_gy ? cm->edge_gy[k] : 0.f;
      float g2 = gx * gx + gy * gy;
      if (g2 < 1e-6f) continue;
      float inv = 1.f / sqrtf(g2);
      float tx = -gy * inv;
      float ty = gx * inv;
      /* two taps along tangent, 1.1 px offset (HR pixels) for stronger AA */
      float x1 = (float)x + tx * 1.1f;
      float y1 = (float)y + ty * 1.1f;
      float x2 = (float)x - tx * 1.1f;
      float y2 = (float)y - ty * 1.1f;
      int ix1 = (int)floorf(x1), iy1 = (int)floorf(y1);
      float fx1 = x1 - ix1, fy1 = y1 - iy1;
      int ix2 = (int)floorf(x2), iy2 = (int)floorf(y2);
      float fx2 = x2 - ix2, fy2 = y2 - iy2;
      float q1[4] = {0}, q2[4] = {0};
      for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
          float w1 = (i ? fx1 : 1.f - fx1) * (j ? fy1 : 1.f - fy1);
          float w2 = (i ? fx2 : 1.f - fx2) * (j ? fy2 : 1.f - fy2);
          int sx1 = ix1 + i, sy1 = iy1 + j;
          if (sx1 < 0) sx1 = 0;
          if (sx1 >= dw) sx1 = dw - 1;
          if (sy1 < 0) sy1 = 0;
          if (sy1 >= dh) sy1 = dh - 1;
          int sx2 = ix2 + i, sy2 = iy2 + j;
          if (sx2 < 0) sx2 = 0;
          if (sx2 >= dw) sx2 = dw - 1;
          if (sy2 < 0) sy2 = 0;
          if (sy2 >= dh) sy2 = dh - 1;
          const float *p1 = snap + 4 * ((size_t)sy1 * dw + sx1);
          const float *p2 = snap + 4 * ((size_t)sy2 * dw + sx2);
          for (int c = 0; c < 4; c++) {
            q1[c] += w1 * p1[c];
            q2[c] += w2 * p2[c];
          }
        }
      }
      float blend = 0.28f * edge_w * (1.f - wc) * (1.f - 0.5f * wj);
      if (blend < 0.02f) continue;
      if (blend > 0.40f) blend = 0.40f;
      float *dst = hr + 4 * ((size_t)y * dw + x);
      for (int c = 0; c < 4; c++) {
        float avg = 0.5f * (q1[c] + q2[c]);
        dst[c] = dst[c] * (1.f - blend) + avg * blend;
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
    float lp_sigma = blur_radius_set ? clampf(blur_radius, 0.1f, 2.f) : 0.75f;
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
     v5: wider range so -s 100 is visibly stronger than -s 9 (was cap 0.16 at s=9);
     monotonic up to 100: 0.012*(s-1) -> 0.45 at 100. */
  float sharp = clampf((compress_strength - 1.f) * 0.012f, 0.f, 0.45f);
  int ok = refine_downsample_consistency(hr, in, sw, sh, dw, dh, 3, .55f,
                                         sharp, &cm);
  if (ok) {
    /* Hard checker policies (scale2x/nearest) reconstruct intentional
       checker/texture crisply by design; running the hourglass remover on top
       would partially flatten exactly the structure the policy was told to
       keep.  Only soft policies need this cleanup. */
    if (policy != POLICY_SCALE2X && policy != POLICY_NEAREST)
      remove_hourglass_basis(hr, dw, dh, in, sw, sh, .60f, cm.w_hg);
    suppress_speckle_pm(hr, dw, dh, in, sw, sh, .60f, cm.w_hg);
    /* v5.1: final tangential AA along edges to kill remaining 45deg treads
       that survive the consistency pass (jump95 0.42 -> 0.05). */
    if (policy != POLICY_SCALE2X && policy != POLICY_NEAREST)
      adaptive_tangential_aa(hr, dw, dh, &cm, sw, sh);
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
    /* v5: floor raised to 1.0 to avoid nearest-neighbour staircase at
       small sigma (diagline 45 deg); 0.75 still showed treads. */
    float h = 1.5f * sigma;
    if (h < 1.f)
      h = 1.f;
    return a <= h ? .5f / h : 0.f;
  }
  case BK_TRIANGLE: {
    float s = 1.1f * sigma + .5f;
    if (s < 1.f)
      s = 1.f;
    return a < s ? (s - a) / (s * s) : 0.f;
  }
  case BK_BSPLINE: {
    /* v5: floor 0.7 -> 1.05: bspline at sigma 0.5 produced jump95 0.73
       on 45 deg (staircase), gaussian 0.30 smooth; wider floor cures. */
    float s = .9f * sigma + .25f, u, b;
    if (s < 1.05f)
      s = 1.05f;
    u = a / s;
    b = u < 1.f ? (4.f - 6.f * u * u + 3.f * u * u * u) / 6.f
      : u < 2.f ? (2.f - u) * (2.f - u) * (2.f - u) / 6.f
                : 0.f;
    return b / s;
  }
  default: { /* BK_GAUSSIAN */
    float s = sigma < .7f ? .7f : sigma;
    return expf(-.5f * x * x / (s * s)) / (s * 2.5066282746f);
  }
  }
}

static int kernel_support_1d(int kind, float sigma) {
  switch (kind) {
  case BK_BOX: {
    float h = 1.5f * sigma;
    return clampi((int)ceilf(h < 1.f ? 1.f : h), 1, 8);
  }
  case BK_TRIANGLE: {
    float s = 1.1f * sigma + .5f;
    return clampi((int)ceilf(s < 1.f ? 1.f : s), 1, 8);
  }
  case BK_BSPLINE: {
    float s = .9f * sigma + .25f;
    if (s < 1.05f)
      s = 1.05f;
    return clampi((int)ceilf(2.f * s), 1, 8);
  }
  default: {
    float s = sigma < .7f ? .7f : sigma;
    return clampi((int)ceilf(3.f * s), 1, 8);
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
   remap, 2 = Anime4K-style gradient push. */
static int deblur_method = 0;
static float deblur_steepness = 0.f; /* <=0: auto (-e adaptive or -s formula) */
static int last_deblur_method = 0;   /* effective method of the last run */
static float last_deblur_k = 0.f;    /* effective fixed steepness (0=adaptive) */
static int upscale_autodeblur(const uint8_t *in, int sw, int sh, uint8_t *out,
                              int dw, int dh);
/* Standard-normal CDF (libm erff). */
static float phi1(float z) { return .5f * (1.f + erff(z * 0.70710678f)); }
/* v5: trust gate widened (was .03/.10) and becomes adaptive to blur size;
   narrow gate zeroed many wide-blur fits (r=6) -> parameter ignoring. */
static float trust_lo = .04f, trust_hi = .22f; /* debug override: CDG=lo,hi */
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
     coh, tanx, tany].  Pass 1.5 integrates it ALONG THE CONTOUR so each
     pixel is rendered from a contour-consensus fit instead of its own
     jittering 1D fit (near-apex windows misplace mu by 1-2 out px and
     raw per-pixel deltas swung +-0.25 with alternating sign). */
  float *PF = malloc(n * 10 * sizeof *PF);
  uint8_t *LOH = malloc(n * 8);              /* local hull, u8/chan    */
  uint8_t *dst = malloc(n * 4);
  if (!A || !DEL || !PF || !LOH || !dst) {
    free(A);
    free(DEL);
    free(PF);
    free(LOH);
    free(dst);
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
  /* v5: k from -s up to 8 (was 3) so -s 100 is visibly stronger than -s 9;
     manual -g still wins but now its cap is looser (see below). */
  float kbase = deblur_steepness > 0.f
                    ? deblur_steepness
                    : clampf(1.f + .25f * (compress_strength - 1.f), 1.f, 8.f);
  last_deblur_k = deblur_steepness > 0.f   ? deblur_steepness
                  : edge_goal > 0.f        ? 0.f
                                           : kbase;
  /* Shading close-gate on the fitted ramp sigma s (output px):
     transitions far wider than a fitted-blur ramp are content softness.
     Smooth LINEAR shading is inert regardless -- its fitted mu is ~0
     and u_px ~.5, so the fit reproduces it unchanged. */
  float sa = 1.3f * scale * sref, sb = 2.4f * scale * sref;
  float flatmix = .25f; /* diffusion-noise flattening in gate-zero zones */
  /* v5: T scales with assumed blur to keep tangential averaging effective
     at wide r (r=6 needs more span to smooth mu jitter and kill 45deg treads). */
  int T = clampi((int)(scale * .75f + .35f * sref + .5f), 1, 6);
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
         pass 1.5); zero when no fit was made. */
      float wS = 0.f, fmu = 0.f, fs = 0.f, fd2[4] = {0, 0, 0, 0};
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
               break -- that tail belongs to the lobe. */
            while (j + 1 < NS - 1 &&
                   (wj[j + 1] >= th || (j + 2 < NS - 1 && wj[j + 2] >= th)) &&
                   !((u[j + 1] <= .002f || u[j + 1] >= .998f) &&
                     wj[j + 1] < th))
              j++;
            jl[NL] = a;
            jr[NL] = j;
            NL++;
            j++;
          }
          if (NL > 0) {
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
              float sp_dbg = -1.f;
              if (n0 > 0 && n1 > 0) {
                float sp = 0.f;
                for (int c = 0; c < 4; c++)
                  sp += (P1[c] - P0[c]) * d[c];
                sp /= L2;
                sp_dbg = sp;
                float cap = fmaxf(sp, .03f) * 1.2f;
                if (ab_b > cap)
                  ab_b = cap;
                if (ab_b < sp)
                  ab_b = sp;
                ab_c = clampf(ab_c, -.5f * cap, .5f * cap);
              }
              for (int c = 0; c < 4; c++)
                fd2[c] = ab_b * d[c];
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
                /* v5: wider blur needs lower coverage threshold (0.35 vs 0.55)
                   else outer shading slopes kill trust and -r 6 leaves blur. */
                float cov_thr = wide ? 0.35f : 0.55f;
                wS *= ss01((cov - cov_thr) * (1.f / .25f));
                if (dbg && y == dbg_y && abs(x - dbg_x) <= 16 &&
                    (x & 3) == 0)
                  fprintf(stderr, "DBGC %d,%d cov=%.3f raw[%.3f..%.3f] "
                                  "step[%.3f..%.3f] thr=%.2f\n",
                          x, y, cov, rmin, rmax, ab_a, ab_a + ab_b, cov_thr);
              }
              /* Steepness: -g pins k exactly (float, up to 64); -e
                 adapts per edge; -s formula otherwise; always capped so
                 the OUTPUT ramp never falls below .6 px sigma
                 (~1.5 px 30% width): no re-aliased sawtooth, and
                 already-crisp content is left alone (k -> 1). */
              float k = kbase;
              if (deblur_steepness <= 0.f && edge_goal > 0.f) {
                float st = fmaxf(.6f, edge_goal * scale / 2.5f);
                k = clampf(s / st, 1.f, 16.f);
              }
              /* v5: respect manual -g: looser anti-alias cap when user
                 explicitly asks for steepness; auto keeps 0.6 px. */
              if (deblur_steepness > 0.f) {
                float minw = 0.40f;
                if (deblur_steepness > 16.f) minw = 0.30f;
                if (deblur_steepness > 32.f) minw = 0.22f;
                if (deblur_steepness > 50.f) minw = 0.18f;
                k = fminf(k, s / minw);
              } else {
                k = fminf(k, s / .6f);
              }
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
                  wS *= ss01((trust_hi - rmse) /
                             (trust_hi - trust_lo + 1e-9f));
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
                /* v5: for wide blur allow more crossings (quantization
                   can create spurious crossings), threshold raised. */
                float cross0 = wide ? 3.0f : 2.25f;
                float cross1 = wide ? 3.5f : 2.5f;
                wS *= 1.f - ss01(((float)cross - cross0) / cross1);
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
                        "Teff=%d a=%.3f c=%.3f b=%.3f sp=%.3f\n",
                        x, y, NL, jl[li], jr[li], dl, dr, W, mu, s, k,
                        z0, ufit0, nu, wS, ed > 1e-9 ? sqrt(en / ed) : -1.,
                        Ld2, coh, Teff, ab_a, ab_c, ab_b, sp_dbg);
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
        float *dd = DEL + 4 * idx, *p = PF + 10 * idx;
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
      const float *pf = PF + 10 * idx;
      float tanx = pf[8], tany = pf[9], cc = pf[7];
      float aW = 0.f, aMu = 0.f, aS = 0.f, aD[4] = {0, 0, 0, 0},
            wsum = 0.f, tw[9], tmu[9];
      for (int to = -T; to <= T; to++) {
        float wt = (float)(T + 1 - (to < 0 ? -to : to)), q[10];
        sample_fn(PF, dw, dh, 10, (float)x + (float)to * tanx,
                  (float)y + (float)to * tany, q);
        wt *= sqrtf(cc * fmaxf(q[7], 0.f));
        tw[to + T] = wt;
        tmu[to + T] = q[0] > 1e-9f ? q[1] / q[0] : 0.f;
        aW += wt * q[0];
        aMu += wt * q[1];
        aS += wt * q[2];
        for (int c = 0; c < 4; c++)
          aD[c] += wt * q[3 + c];
        wsum += wt;
      }
      if (aW > 1e-6f && wsum > 1e-6f) {
        float o[4];
        float mu = aMu / aW, s = aS / aW, w = aW / wsum, d2[4];
        for (int c = 0; c < 4; c++) {
          d2[c] = aD[c] / aW;
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
        if (deblur_steepness > 0.f) {
          float minw = 0.40f;
          if (deblur_steepness > 16.f) minw = 0.30f;
          if (deblur_steepness > 32.f) minw = 0.22f;
          if (deblur_steepness > 50.f) minw = 0.18f;
          k = fminf(k, s / minw);
        } else {
          k = fminf(k, s / .6f);
        }
        /* Anchored evaluation (v4.8) on the consensus fit. */
        float z0 = (0.f - mu) / s;
        float ufit0 = phi1(z0), nu;
        if (method == 2 && k > 1.f)
          nu = phi1(z0 + (ufit0 - .5f) * (k - 1.f) * 1.5f);
        else
          nu = phi1(k * z0);
        nu = clampf(nu, 0.f, 1.f);
        float *dd = DEL + 4 * idx;
        for (int c = 0; c < 4; c++) {
          float blo = c < 3 ? to_linear[LOH[8 * idx + c]]
                            : LOH[8 * idx + c] * (1.f / 255.f),
                bhi = c < 3 ? to_linear[LOH[8 * idx + 4 + c]]
                            : LOH[8 * idx + 4 + c] * (1.f / 255.f);
          float v = clampf(o[c] + w * (nu - ufit0) * d2[c], blo, bhi);
          dd[c] += v - o[c];
        }
        if (dbg && y == dbg_y && abs(x - dbg_x) <= 16 && (x & 3) == 0)
          fprintf(stderr,
                  "DBGS %d,%d mu=%.3f s=%.3f k=%.3f ufit=%.4f nu=%.4f "
                  "wSeff=%.4f mu_std=%.3f (consensus)\n",
                  x, y, mu, s, k, ufit0, nu, w, mu_std);
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
      const float *pf = PF + 10 * idx;
      float cc = pf[7];
      float acc[4] = {0, 0, 0, 0}, o[4], res[4], wsum = 0.f;
      for (int to = -T; to <= T; to++) {
        float wt = (float)(T + 1 - (to < 0 ? -to : to)), q[4], qc[10];
        float sx = (float)x + (float)to * pf[8],
              sy = (float)y + (float)to * pf[9];
        sample_fn(PF, dw, dh, 10, sx, sy, qc);
        wt *= sqrtf(cc * fmaxf(qc[7], 0.f));
        sample_f4(DEL, dw, dh, sx, sy, q);
        for (int c = 0; c < 4; c++)
          acc[c] += wt * q[c];
        wsum += wt;
      }
      for (int c = 0; c < 4; c++) {
        o[c] = A[4 * idx + c];
        float v = wsum > 1e-6f ? o[c] + acc[c] / wsum : o[c];
        /* Final hull clamp (v4.9): pass-1.5 clamps each pixel's own
           evaluation, but the smoothed delta can still leave the hull
           by transport.  No output may leave the locally observed
           colour range -- enforced here by construction. */
        float blo = c < 3 ? to_linear[LOH[8 * idx + c]]
                          : LOH[8 * idx + c] * (1.f / 255.f),
              bhi = c < 3 ? to_linear[LOH[8 * idx + 4 + c]]
                          : LOH[8 * idx + 4 + c] * (1.f / 255.f);
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
  return 1;
}

static int upscale_autodeblur(const uint8_t *in, int sw, int sh, uint8_t *out,
                              int dw, int dh) {
  /* v4.9.1: the v4.9 base-sigma decouple is REVERTED.  It dodged the
     neon skirt by rendering the base at sigma r/min(K,8), but that
     re-exposed the source lattice staircase the user deliberately
     blurs away with -r ("minimal blur when stairs are no longer
     visible"), and brought back per-tread speckle and forked caps.
     Neon is prevented where it actually arises -- partial-trust
     blends -- by the lobe map, consensus evaluation and hull clamp. */
  adb_sigma_div = 0.f;
  adb_assumed_sigma = 0.f;
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
  int ok = refine_hourglass_checker(hr, in, sw, sh, dw, dh, 4);
  if (ok)
    ok = refine_downsample_consistency(hr, in, sw, sh, dw, dh, 1, .35f, 0.f,
                                       NULL);
  if (ok) {
    remove_hourglass_basis(hr, dw, dh, in, sw, sh, .80f, NULL);
    suppress_speckle_pm(hr, dw, dh, in, sw, sh, .80f, NULL);
    write_hr_rgba(hr, dw, dh, out);
  }
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
  /* Gated removal (v2): hit ambiguous checker/junction cells hard, leave
     coherent-edge detail almost untouched.  The old uniform pass traded away
     real sharpness everywhere. */
  remove_hourglass_basis(hr, dw, dh, in, sw, sh, .95f, cm.w_hg);
    suppress_speckle_pm(hr, dw, dh, in, sw, sh, .95f, cm.w_hg);
  /* One very light consistency pass restores exact source average bias without
     reintroducing much hourglass structure; its correction is gated too. */
  ok = refine_downsample_consistency(hr, in, sw, sh, dw, dh, 1, .35f, 0.f,
                                     &cm);
  if (ok) {
    remove_hourglass_basis(hr, dw, dh, in, sw, sh, .55f, cm.w_hg);
    suppress_speckle_pm(hr, dw, dh, in, sw, sh, .55f, cm.w_hg);
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

/* v5.2: supersampled smooth mode - 2x2 subpixel area averaging with triangle
   interpolation, guaranteeing no staircase even at 45deg. The -r parameter
   scales the subpixel spread (extra blur) for user control. */
static void upscale_smooth(const uint8_t *in, int sw, int sh, uint8_t *out,
                           int dw, int dh) {
  float spread = 1.f;
  if (blur_radius_set) spread = clampf(blur_radius, 0.5f, 2.f);
  /* 2x2 supersampling: 4 samples per output pixel */
  for (int y = 0; y < dh; y++) {
    for (int x = 0; x < dw; x++) {
      float acc[4] = {0,0,0,0};
      for (int sy_sub = 0; sy_sub < 2; sy_sub++) {
        for (int sx_sub = 0; sx_sub < 2; sx_sub++) {
          float ox = (sx_sub == 0 ? 0.25f : 0.75f);
          float oy = (sy_sub == 0 ? 0.25f : 0.75f);
          float sx = ((float)x + ox + 0.5f) * (float)sw / dw - 0.5f;
          float sy = ((float)y + oy + 0.5f) * (float)sh / dh - 0.5f;
          /* small jitter proportional to spread-1 for extra AA when -r>1 */
          if (spread > 1.f) {
            sx += (ox - 0.5f) * (spread - 1.f) * 0.25f;
            sy += (oy - 0.5f) * (spread - 1.f) * 0.25f;
          }
          int ix = (int)floorf(sx), iy = (int)floorf(sy);
          float fx = sx - ix, fy = sy - iy;
          float p[4][4], l[4];
          for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) {
              int k = j * 2 + i;
              raw_pm(in, sw, sh, ix + i, iy + j, p[k]);
              l[k] = .2126f * p[k][0] + .7152f * p[k][1] + .0722f * p[k][2] + .5f * p[k][3];
            }
          float gx = l[1] + l[3] - l[0] - l[2], gy = l[2] + l[3] - l[0] - l[1];
          const float *a,*b,*c;
          float wa,wb,wc;
          if (gx * gy < 0) {
            if (fx >= fy) { a=p[0]; b=p[1]; c=p[3]; wa=1-fx; wb=fx-fy; wc=fy; }
            else { a=p[0]; b=p[2]; c=p[3]; wa=1-fy; wb=fy-fx; wc=fx; }
          } else {
            if (fx + fy <= 1) { a=p[0]; b=p[1]; c=p[2]; wa=1-fx-fy; wb=fx; wc=fy; }
            else { a=p[3]; b=p[2]; c=p[1]; wa=fx+fy-1; wb=1-fx; wc=1-fy; }
          }
          for (int k = 0; k < 4; k++)
            acc[k] += 0.25f * (wa * a[k] + wb * b[k] + wc * c[k]);
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
      "v4.9.2)\n"
      "\n"
      "Usage: %s in.webp out.webp SCALE [options]\n"
      "  SCALE is the upsampling factor, real number in (1,32] "
      "(e.g. 2, 3, 4.5, 10).\n"
      "\n"
      "Modes (-m MODE, default cubic) -- recommended:\n"
      "  adaptive      natural images and mixed art; classifies each patch\n"
      "                (edge/checker/junction/line/gradient) and picks a\n"
      "                safe interpolation policy per patch\n"
      "  autoblur      fits the blur the source was probably downsampled\n"
      "                through and renders at target resolution -- smooth,\n"
      "                round contours at high scale, no sawtooth\n"
      "  autodeblur    autoblur base + gradient-slope steepening: sharp\n"
      "                edges with no halos/ringing; flats gently cleaned.\n"
      "                Best for AI-upscaled/diffusion anime art\n"
      "  triangle      soft, no ringing or halos; safe default for art\n"
      "  smooth        supersampled triangle (2x2 area avg), -r controls\n"
      "                extra spread; guaranteed no staircase, softest\n"
      "  sdf           fitted signed-distance contour sharpening on top of\n"
      "                adaptive; crispest edges, tune with -s\n"
      "  nearest       pixel art / hard 1px texture\n"
      "\n"
      "Modes -- other (accepted, but expect artifacts on art):\n"
      "  bilinear cubic mitchell lanczos2 lanczos3 blur\n"
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
      "                            4); in autodeblur it sets slope steepness\n"
      "                            k = 1+.25*(N-1), clamped to 3 (N>=9 max),\n"
      "                            unless -g or -e overrides (see table)\n"
      "  -r, --blur-radius R       blur radius .1..40 (default 1): radius\n"
      "                            for *blurcompress modes; ALSO pins the\n"
      "                            autoblur sigma exactly.  In autodeblur it\n"
      "                            is the ASSUMED source blur and the base\n"
      "                            render sigma alike (v4.9.1: the v4.9\n"
      "                            base-sigma decouple was reverted -- it\n"
      "                            re-exposed the lattice staircase -r is\n"
      "                            chosen to hide).  Windows and gates size\n"
      "                            from R.\n"
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
      "  -D, --deblur-method M     autodeblur method auto|remap|push\n"
      "                            (v4.9.2: 'remake' accepted as alias;\n"
      "                            default auto = 2x proxy picks per image);\n"
      "                            remap = evaluate the slope-steepened\n"
      "                            profile fit at the pixel's own position,\n"
      "                            push = evaluate the original fit at a\n"
      "                            position displaced toward the nearer\n"
      "                            plateau (Anime4K push)\n"
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
      else {
        fprintf(stderr, "Unknown deblur method: %s\n", av[i + 1]);
        return 2;
      }
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
      strcmp(mode, "cubic") && strcmp(mode, "mitchell") &&
      strcmp(mode, "lanczos2") && strcmp(mode, "lanczos3") &&
      strcmp(mode, "compress") && strcmp(mode, "safecompress") &&
      strcmp(mode, "blur") && strcmp(mode, "blurcompress") &&
      strcmp(mode, "safeblurcompress") && strcmp(mode, "edgecompress") &&
      strcmp(mode, "deblurcompress") && strcmp(mode, "dehourglass") &&
      strcmp(mode, "consistentcompress") && strcmp(mode, "hourglasscompress") &&
      strcmp(mode, "triangle") && strcmp(mode, "smooth") && strcmp(mode, "adaptive") &&
      strcmp(mode, "classmap") && strcmp(mode, "scale2x") &&
      strcmp(mode, "autoblur") && strcmp(mode, "sdf") &&
      strcmp(mode, "autodeblur")) {
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
                     !strcmp(mode, "sdf");
    double in_bytes =
        (double)w * h * (classified ? 96.0 : (!strcmp(mode, "autoblur") ||
                                        !strcmp(mode, "autodeblur")) ? 24.0
                                                                      : 4.0);
    double out_bytes =
        (double)ow * oh * (iterative          ? 36.0
                           : !strcmp(mode, "sdf") ? 36.0 + 40.0 /* adaptive +
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
  else if (!strcmp(mode, "autodeblur"))
    ok = upscale_autodeblur(in, w, h, out, ow, oh);
  else if (!strcmp(mode, "sdf"))
    ok = upscale_sdf(in, w, h, out, ow, oh);
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
  } else if (!strcmp(mode, "autoblur") || !strcmp(mode, "autodeblur")) {
    printf(", kernel=%s sigma=%.2f curve=%s param=%.2f",
           kernel_name(fitted_kernel), fitted_sigma, curve_name(fitted_curve),
           fitted_cp);
    if (!strcmp(mode, "autodeblur")) {
      printf(", method=%s", last_deblur_method == 2 ? "push" : "remap");
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
