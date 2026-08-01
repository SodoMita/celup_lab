/* xBR pixel art upscaler - simpler predecessor to xBRZ
 * Based on Hyllian's xBR algorithm
 * Supports 2x, 3x, 4x scaling
 */
#include "celup_lab_xbr.h"
#include <stdlib.h>
#include <string.h>

typedef uint32_t px_t;
static inline uint8_t px_r(px_t p){ return p & 0xFF; }
static inline uint8_t px_g(px_t p){ return (p>>8) & 0xFF; }
static inline uint8_t px_b(px_t p){ return (p>>16) & 0xFF; }
static inline uint8_t px_a(px_t p){ return (p>>24) & 0xFF; }
static inline px_t px_mk(uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    return r | ((uint32_t)g<<8) | ((uint32_t)b<<16) | ((uint32_t)a<<24);
}

/* Color distance in RGB space */
static inline int color_dist(px_t a, px_t b){
    int dr = (int)px_r(a) - (int)px_r(b);
    int dg = (int)px_g(a) - (int)px_g(b);
    int db = (int)px_b(a) - (int)px_b(b);
    return dr*dr + dg*dg + db*db;
}

static inline int colors_equal(px_t a, px_t b, int threshold){
    return color_dist(a, b) < threshold;
}

/* Bilinear interpolation */
static inline px_t lerp(px_t a, px_t b, int t, int n){
    return px_mk(
        px_r(a) + (px_r(b) - px_r(a)) * t / n,
        px_g(a) + (px_g(b) - px_g(a)) * t / n,
        px_b(a) + (px_b(b) - px_b(a)) * t / n,
        px_a(a) + (px_a(b) - px_a(a)) * t / n
    );
}

/* 2x scaling with xBR rules */
static void scale_2x(const px_t *src, px_t *dst, int w, int h){
    for (int y = 0; y < h; y++){
        for (int x = 0; x < w; x++){
            /* Get 3x3 neighborhood */
            px_t E = src[y * w + x];
            px_t A = (y > 0 && x > 0) ? src[(y-1) * w + (x-1)] : E;
            px_t B = (y > 0) ? src[(y-1) * w + x] : E;
            px_t C = (y > 0 && x < w-1) ? src[(y-1) * w + (x+1)] : E;
            px_t D = (x > 0) ? src[y * w + (x-1)] : E;
            px_t F = (x < w-1) ? src[y * w + (x+1)] : E;
            px_t G = (y < h-1 && x > 0) ? src[(y+1) * w + (x-1)] : E;
            px_t H = (y < h-1) ? src[(y+1) * w + x] : E;
            px_t I = (y < h-1 && x < w-1) ? src[(y+1) * w + (x+1)] : E;
            
            int dst_y = y * 2;
            int dst_x = x * 2;
            
            /* Default: nearest neighbor */
            dst[dst_y * (w*2) + dst_x] = E;
            dst[dst_y * (w*2) + dst_x + 1] = E;
            dst[(dst_y+1) * (w*2) + dst_x] = E;
            dst[(dst_y+1) * (w*2) + dst_x + 1] = E;
            
            /* xBR rules for corner pixels */
            if (!colors_equal(E, H, 30) && !colors_equal(E, F, 30)){
                if (colors_equal(B, F, 30) && colors_equal(H, D, 30) && !colors_equal(B, D, 30)){
                    dst[(dst_y+1) * (w*2) + dst_x + 1] = lerp(E, H, 1, 2);
                }
            }
            if (!colors_equal(E, D, 30) && !colors_equal(E, B, 30)){
                if (colors_equal(H, D, 30) && colors_equal(B, F, 30) && !colors_equal(H, F, 30)){
                    dst[dst_y * (w*2) + dst_x] = lerp(E, B, 1, 2);
                }
            }
            if (!colors_equal(E, F, 30) && !colors_equal(E, H, 30)){
                if (colors_equal(D, H, 30) && colors_equal(F, B, 30) && !colors_equal(D, B, 30)){
                    dst[dst_y * (w*2) + dst_x + 1] = lerp(E, F, 1, 2);
                }
            }
            if (!colors_equal(E, B, 30) && !colors_equal(E, D, 30)){
                if (colors_equal(F, B, 30) && colors_equal(D, H, 30) && !colors_equal(F, H, 30)){
                    dst[(dst_y+1) * (w*2) + dst_x] = lerp(E, D, 1, 2);
                }
            }
        }
    }
}

/* 3x scaling with xBR rules */
static void scale_3x(const px_t *src, px_t *dst, int w, int h){
    for (int y = 0; y < h; y++){
        for (int x = 0; x < w; x++){
            px_t E = src[y * w + x];
            px_t A = (y > 0 && x > 0) ? src[(y-1) * w + (x-1)] : E;
            px_t B = (y > 0) ? src[(y-1) * w + x] : E;
            px_t C = (y > 0 && x < w-1) ? src[(y-1) * w + (x+1)] : E;
            px_t D = (x > 0) ? src[y * w + (x-1)] : E;
            px_t F = (x < w-1) ? src[y * w + (x+1)] : E;
            px_t G = (y < h-1 && x > 0) ? src[(y+1) * w + (x-1)] : E;
            px_t H = (y < h-1) ? src[(y+1) * w + x] : E;
            px_t I = (y < h-1 && x < w-1) ? src[(y+1) * w + (x+1)] : E;
            
            int dst_y = y * 3;
            int dst_x = x * 3;
            int dw = w * 3;
            
            /* Default: nearest neighbor */
            for (int dy = 0; dy < 3; dy++){
                for (int dx = 0; dx < 3; dx++){
                    dst[(dst_y+dy) * dw + (dst_x+dx)] = E;
                }
            }
            
            /* xBR rules for 3x */
            if (!colors_equal(E, H, 30) && !colors_equal(E, F, 30)){
                if (colors_equal(B, F, 30) && colors_equal(H, D, 30) && !colors_equal(B, D, 30)){
                    dst[(dst_y+2) * dw + (dst_x+2)] = H;
                    dst[(dst_y+2) * dw + (dst_x+1)] = lerp(E, H, 1, 4);
                    dst[(dst_y+1) * dw + (dst_x+2)] = lerp(E, H, 1, 4);
                }
            }
            if (!colors_equal(E, D, 30) && !colors_equal(E, B, 30)){
                if (colors_equal(H, D, 30) && colors_equal(B, F, 30) && !colors_equal(H, F, 30)){
                    dst[dst_y * dw + dst_x] = B;
                    dst[dst_y * dw + (dst_x+1)] = lerp(E, B, 1, 4);
                    dst[(dst_y+1) * dw + dst_x] = lerp(E, B, 1, 4);
                }
            }
            if (!colors_equal(E, F, 30) && !colors_equal(E, H, 30)){
                if (colors_equal(D, H, 30) && colors_equal(F, B, 30) && !colors_equal(D, B, 30)){
                    dst[dst_y * dw + (dst_x+2)] = F;
                    dst[dst_y * dw + (dst_x+1)] = lerp(E, F, 1, 4);
                    dst[(dst_y+1) * dw + (dst_x+2)] = lerp(E, F, 1, 4);
                }
            }
            if (!colors_equal(E, B, 30) && !colors_equal(E, D, 30)){
                if (colors_equal(F, B, 30) && colors_equal(D, H, 30) && !colors_equal(F, H, 30)){
                    dst[(dst_y+2) * dw + dst_x] = D;
                    dst[(dst_y+2) * dw + (dst_x+1)] = lerp(E, D, 1, 4);
                    dst[(dst_y+1) * dw + dst_x] = lerp(E, D, 1, 4);
                }
            }
        }
    }
}

/* 4x scaling with xBR rules */
static void scale_4x(const px_t *src, px_t *dst, int w, int h){
    for (int y = 0; y < h; y++){
        for (int x = 0; x < w; x++){
            px_t E = src[y * w + x];
            px_t A = (y > 0 && x > 0) ? src[(y-1) * w + (x-1)] : E;
            px_t B = (y > 0) ? src[(y-1) * w + x] : E;
            px_t C = (y > 0 && x < w-1) ? src[(y-1) * w + (x+1)] : E;
            px_t D = (x > 0) ? src[y * w + (x-1)] : E;
            px_t F = (x < w-1) ? src[y * w + (x+1)] : E;
            px_t G = (y < h-1 && x > 0) ? src[(y+1) * w + (x-1)] : E;
            px_t H = (y < h-1) ? src[(y+1) * w + x] : E;
            px_t I = (y < h-1 && x < w-1) ? src[(y+1) * w + (x+1)] : E;
            
            int dst_y = y * 4;
            int dst_x = x * 4;
            int dw = w * 4;
            
            /* Default: nearest neighbor */
            for (int dy = 0; dy < 4; dy++){
                for (int dx = 0; dx < 4; dx++){
                    dst[(dst_y+dy) * dw + (dst_x+dx)] = E;
                }
            }
            
            /* xBR rules for 4x - more detailed blending */
            if (!colors_equal(E, H, 30) && !colors_equal(E, F, 30)){
                if (colors_equal(B, F, 30) && colors_equal(H, D, 30) && !colors_equal(B, D, 30)){
                    dst[(dst_y+3) * dw + (dst_x+3)] = H;
                    dst[(dst_y+3) * dw + (dst_x+2)] = lerp(E, H, 1, 3);
                    dst[(dst_y+2) * dw + (dst_x+3)] = lerp(E, H, 1, 3);
                    dst[(dst_y+2) * dw + (dst_x+2)] = lerp(E, H, 1, 2);
                }
            }
            if (!colors_equal(E, D, 30) && !colors_equal(E, B, 30)){
                if (colors_equal(H, D, 30) && colors_equal(B, F, 30) && !colors_equal(H, F, 30)){
                    dst[dst_y * dw + dst_x] = B;
                    dst[dst_y * dw + (dst_x+1)] = lerp(E, B, 1, 3);
                    dst[(dst_y+1) * dw + dst_x] = lerp(E, B, 1, 3);
                    dst[(dst_y+1) * dw + (dst_x+1)] = lerp(E, B, 1, 2);
                }
            }
            if (!colors_equal(E, F, 30) && !colors_equal(E, H, 30)){
                if (colors_equal(D, H, 30) && colors_equal(F, B, 30) && !colors_equal(D, B, 30)){
                    dst[dst_y * dw + (dst_x+3)] = F;
                    dst[dst_y * dw + (dst_x+2)] = lerp(E, F, 1, 3);
                    dst[(dst_y+1) * dw + (dst_x+3)] = lerp(E, F, 1, 3);
                    dst[(dst_y+1) * dw + (dst_x+2)] = lerp(E, F, 1, 2);
                }
            }
            if (!colors_equal(E, B, 30) && !colors_equal(E, D, 30)){
                if (colors_equal(F, B, 30) && colors_equal(D, H, 30) && !colors_equal(F, H, 30)){
                    dst[(dst_y+3) * dw + dst_x] = D;
                    dst[(dst_y+3) * dw + (dst_x+1)] = lerp(E, D, 1, 3);
                    dst[(dst_y+2) * dw + dst_x] = lerp(E, D, 1, 3);
                    dst[(dst_y+2) * dw + (dst_x+1)] = lerp(E, D, 1, 2);
                }
            }
        }
    }
}

int xbr_scale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, int factor){
    if (factor < 2 || factor > 4) return -1;
    if (sw <= 0 || sh <= 0) return -1;
    if (dw != sw * factor || dh != sh * factor) return -1;
    
    const px_t *src_px = (const px_t *)src;
    px_t *dst_px = (px_t *)dst;
    
    switch(factor){
        case 2: scale_2x(src_px, dst_px, sw, sh); break;
        case 3: scale_3x(src_px, dst_px, sw, sh); break;
        case 4: scale_4x(src_px, dst_px, sw, sh); break;
        default: return -1;
    }
    
    return 0;
}
