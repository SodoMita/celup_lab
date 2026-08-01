/* xBR pixel art upscaler - faithful implementation of Hyllian's algorithm
 * Based on FFmpeg's vf_xbr.c and libxbr-standalone
 * Copyright (c) 2011, 2012 Hyllian/Jararaca
 * Copyright (c) 2015 Treeki (standalone version)
 * Supports 2x, 3x, 4x scaling
 */
#include "celup_lab_xbr.h"
#include <stdlib.h>
#include <string.h>

typedef uint32_t px_t;

/* YUV conversion lookup table - allocated on heap (16M entries = 64MB) */
static uint32_t *rgbtoyuv = NULL;

static inline int _max(int a, int b) { return (a > b) ? a : b; }
static inline int _min(int a, int b) { return (a < b) ? a : b; }

static void xbr_init_data(void) {
    if (rgbtoyuv) return; /* Already initialized */
    
    rgbtoyuv = (uint32_t *)calloc(1 << 24, sizeof(uint32_t));
    if (!rgbtoyuv) return;
    
    uint32_t c;
    int bg, rg, g;
    
    for (bg = 0; bg < 256; bg++) {
        for (rg = 0; rg < 256; rg++) {
            const uint32_t u = (uint32_t)((-169 * rg + 500 * bg) / 1000) + 128;
            const uint32_t v = (uint32_t)((500 * rg - 81 * bg) / 1000) + 128;
            int startg = _max(-bg, _max(-rg, 0));
            int endg = _min(255 - bg, _min(255 - rg, 255));
            uint32_t y = (uint32_t)((299 * rg + 1000 * startg + 114 * bg) / 1000);
            
            /* c must be in range [0, 2^24) */
            c = (uint32_t)(bg + (rg << 16) + 0x010101 * startg);
            if (c >= (1 << 24)) continue;
            
            for (g = startg; g <= endg; g++) {
                if (c < (1 << 24)) {
                    rgbtoyuv[c] = ((y++) << 16) + (u << 8) + v;
                    c += 0x010101;
                }
            }
        }
    }
}

#define LB_MASK 0x00FEFEFE
#define RED_BLUE_MASK 0x00FF00FF
#define GREEN_MASK 0x0000FF00
#define PART_MASK 0x00FF00FF

static uint32_t pixel_diff(uint32_t x, uint32_t y) {
#define YMASK 0xff0000
#define UMASK 0x00ff00
#define VMASK 0x0000ff
    uint32_t yuv1 = rgbtoyuv[x & 0xffffff];
    uint32_t yuv2 = rgbtoyuv[y & 0xffffff];
    return (abs((int)((x >> 24) - (y >> 24)))) +
           (abs((int)((yuv1 & YMASK) - (yuv2 & YMASK))) >> 16) +
           (abs((int)((yuv1 & UMASK) - (yuv2 & UMASK))) >> 8) +
           abs((int)((yuv1 & VMASK) - (yuv2 & VMASK)));
}

#define ALPHA_BLEND_BASE(a, b, m, s) ( \
    (PART_MASK & (((a) & PART_MASK) + (((((b) & PART_MASK) - ((a) & PART_MASK)) * (m)) >> (s)))) | \
    ((PART_MASK & ((((a) >> 8) & PART_MASK) + ((((((b) >> 8) & PART_MASK) - (((a) >> 8) & PART_MASK)) * (m)) >> (s)))) << 8))

#define ALPHA_BLEND_32_W(a, b)  ALPHA_BLEND_BASE(a, b, 1, 3)
#define ALPHA_BLEND_64_W(a, b)  ALPHA_BLEND_BASE(a, b, 1, 2)
#define ALPHA_BLEND_128_W(a, b) ALPHA_BLEND_BASE(a, b, 1, 1)
#define ALPHA_BLEND_192_W(a, b) ALPHA_BLEND_BASE(a, b, 3, 2)
#define ALPHA_BLEND_224_W(a, b) ALPHA_BLEND_BASE(a, b, 7, 3)

#define df(A, B) pixel_diff(A, B)
#define eq(A, B) (df(A, B) < 155)

#define FILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, \
    N0, N1, N2, N3) do { \
    if (PE != PH && PE != PF) { \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2); \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2); \
        if (e <= i) { \
            const unsigned px = df(PE,PF) <= df(PE,PH) ? PF : PH; \
            if (e < i && (!eq(PF,PB) && !eq(PH,PD) || eq(PE,PI) \
                && (!eq(PF,I4) && !eq(PH,I5)) \
                || eq(PE,PG) || eq(PE,PC))) { \
                const unsigned ke = df(PF,PG); \
                const unsigned ki = df(PH,PC); \
                const int left = ke<<1 <= ki && PE != PG && PD != PG; \
                const int up = ke >= ki<<1 && PE != PC && PB != PC; \
                if (left && up) { \
                    E[N3] = ALPHA_BLEND_224_W(E[N3], px); \
                    E[N2] = ALPHA_BLEND_64_W( E[N2], px); \
                    E[N1] = E[N2]; \
                } else if (left) { \
                    E[N3] = ALPHA_BLEND_192_W(E[N3], px); \
                    E[N2] = ALPHA_BLEND_64_W( E[N2], px); \
                } else if (up) { \
                    E[N3] = ALPHA_BLEND_192_W(E[N3], px); \
                    E[N1] = ALPHA_BLEND_64_W( E[N1], px); \
                } else { /* diagonal */ \
                    E[N3] = ALPHA_BLEND_128_W(E[N3], px); \
                } \
            } else { \
                E[N3] = ALPHA_BLEND_128_W(E[N3], px); \
            } \
        } \
    } \
} while (0)

#define FILT3(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, \
    N0, N1, N2, N3, N4, N5, N6, N7, N8) do { \
    if (PE != PH && PE != PF) { \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2); \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2); \
        if (e <= i) { \
            const unsigned px = df(PE,PF) <= df(PE,PH) ? PF : PH; \
            if (e < i && (!eq(PF,PB) && !eq(PF,PC) || !eq(PH,PD) && !eq(PH,PG) || eq(PE,PI) \
                && (!eq(PF,F4) && !eq(PF,I4) || !eq(PH,H5) && !eq(PH,I5)) \
                || eq(PE,PG) || eq(PE,PC))) { \
                const unsigned ke = df(PF,PG); \
                const unsigned ki = df(PH,PC); \
                const int left = ke<<1 <= ki && PE != PG && PD != PG; \
                const int up = ke >= ki<<1 && PE != PC && PB != PC; \
                if (left && up) { \
                    E[N7] = ALPHA_BLEND_192_W(E[N7], px); \
                    E[N6] = ALPHA_BLEND_64_W( E[N6], px); \
                    E[N5] = E[N7]; \
                    E[N2] = E[N6]; \
                    E[N8] = px; \
                } else if (left) { \
                    E[N7] = ALPHA_BLEND_192_W(E[N7], px); \
                    E[N5] = ALPHA_BLEND_64_W( E[N5], px); \
                    E[N6] = ALPHA_BLEND_64_W( E[N6], px); \
                    E[N8] = px; \
                } else if (up) { \
                    E[N5] = ALPHA_BLEND_192_W(E[N5], px); \
                    E[N7] = ALPHA_BLEND_64_W( E[N7], px); \
                    E[N2] = ALPHA_BLEND_64_W( E[N2], px); \
                    E[N8] = px; \
                } else { /* diagonal */ \
                    E[N8] = ALPHA_BLEND_224_W(E[N8], px); \
                    E[N5] = ALPHA_BLEND_32_W( E[N5], px); \
                    E[N7] = ALPHA_BLEND_32_W( E[N7], px); \
                } \
            } else { \
                E[N8] = ALPHA_BLEND_128_W(E[N8], px); \
            } \
        } \
    } \
} while (0)

#define FILT4(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, \
    N15, N14, N11, N3, N7, N10, N13, N12, N9, N6, N2, N1, N5, N8, N4, N0) do { \
    if (PE != PH && PE != PF) { \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2); \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2); \
        if (e <= i) { \
            const unsigned px = df(PE,PF) <= df(PE,PH) ? PF : PH; \
            if (e < i && (!eq(PF,PB) && !eq(PH,PD) || eq(PE,PI) \
                && (!eq(PF,I4) && !eq(PH,I5)) \
                || eq(PE,PG) || eq(PE,PC))) { \
                const unsigned ke = df(PF,PG); \
                const unsigned ki = df(PH,PC); \
                const int left = ke<<1 <= ki && PE != PG && PD != PG; \
                const int up = ke >= ki<<1 && PE != PC && PB != PC; \
                if (left && up) { \
                    E[N13] = ALPHA_BLEND_192_W(E[N13], px); \
                    E[N12] = ALPHA_BLEND_64_W( E[N12], px); \
                    E[N15] = E[N14] = E[N11] = px; \
                    E[N10] = E[N3] = E[N12]; \
                    E[N7] = E[N13]; \
                } else if (left) { \
                    E[N11] = ALPHA_BLEND_192_W(E[N11], px); \
                    E[N13] = ALPHA_BLEND_192_W(E[N13], px); \
                    E[N10] = ALPHA_BLEND_64_W( E[N10], px); \
                    E[N12] = ALPHA_BLEND_64_W( E[N12], px); \
                    E[N14] = px; \
                    E[N15] = px; \
                } else if (up) { \
                    E[N14] = ALPHA_BLEND_192_W(E[N14], px); \
                    E[N7 ] = ALPHA_BLEND_192_W(E[N7 ], px); \
                    E[N10] = ALPHA_BLEND_64_W( E[N10], px); \
                    E[N3 ] = ALPHA_BLEND_64_W( E[N3 ], px); \
                    E[N11] = px; \
                    E[N15] = px; \
                } else { /* diagonal */ \
                    E[N11] = ALPHA_BLEND_128_W(E[N11], px); \
                    E[N14] = ALPHA_BLEND_128_W(E[N14], px); \
                    E[N15] = px; \
                } \
            } else { \
                E[N15] = ALPHA_BLEND_128_W(E[N15], px); \
            } \
        } \
    } \
} while (0)

static void xbr_filter(const uint8_t *input, uint8_t *output, int inWidth, int inHeight, int inPitch, int outPitch, int n) {
    xbr_init_data();
    int x, y;
    const int nl = outPitch >> 2;
    const int nl1 = nl + nl;
    const int nl2 = nl1 + nl;

    for (y = 0; y < inHeight; y++) {
        uint32_t *E = (uint32_t *)(output + y * outPitch * n);
        const uint32_t *sa2 = (const uint32_t *)(input + y * inPitch);
        const uint32_t *sa1 = (y > 0) ? sa2 - (inPitch >> 2) : sa2;
        const uint32_t *sa0 = (y > 1) ? sa1 - (inPitch >> 2) : sa1;
        const uint32_t *sa3 = (y < inHeight - 1) ? sa2 + (inPitch >> 2) : sa2;
        const uint32_t *sa4 = (y < inHeight - 2) ? sa3 + (inPitch >> 2) : sa3;

        for (x = 0; x < inWidth; x++) {
            const uint32_t B1 = (y > 0 && x > 0) ? sa0[-1] : sa2[0];
            const uint32_t PB = (y > 0) ? sa1[0] : sa2[0];
            const uint32_t PE = sa2[0];
            const uint32_t PH = (y < inHeight - 1) ? sa3[0] : sa2[0];
            const uint32_t H5 = (y < inHeight - 2) ? sa4[0] : sa3[0];

            const int pprev = (x > 0) ? -1 : 0;
            const uint32_t A1 = (y > 0 && x > 0) ? sa0[pprev] : sa2[0];
            const uint32_t PA = (x > 0) ? sa1[pprev] : sa2[0];
            const uint32_t PD = (x > 0) ? sa2[pprev] : sa2[0];
            const uint32_t PG = (y < inHeight - 1 && x > 0) ? sa3[pprev] : sa2[0];
            const uint32_t G5 = (y < inHeight - 2 && x > 0) ? sa4[pprev] : sa3[0];

            const int pprev2 = (x > 1) ? -2 : (x > 0 ? -1 : 0);
            const uint32_t A0 = (y > 0 && x > 1) ? sa1[pprev2] : sa2[0];
            const uint32_t D0 = (x > 1) ? sa2[pprev2] : sa2[0];
            const uint32_t G0 = (y < inHeight - 1 && x > 1) ? sa3[pprev2] : sa2[0];

            const int pnext = (x < inWidth - 1) ? 1 : 0;
            const uint32_t C1 = (y > 0 && x < inWidth - 1) ? sa0[pnext] : sa2[0];
            const uint32_t PC = (x < inWidth - 1) ? sa1[pnext] : sa2[0];
            const uint32_t PF = (x < inWidth - 1) ? sa2[pnext] : sa2[0];
            const uint32_t PI = (y < inHeight - 1 && x < inWidth - 1) ? sa3[pnext] : sa2[0];
            const uint32_t I5 = (y < inHeight - 2 && x < inWidth - 1) ? sa4[pnext] : sa3[0];

            const int pnext2 = (x < inWidth - 2) ? 2 : (x < inWidth - 1 ? 1 : 0);
            const uint32_t C4 = (y > 0 && x < inWidth - 2) ? sa1[pnext2] : sa2[0];
            const uint32_t F4 = (x < inWidth - 2) ? sa2[pnext2] : sa2[0];
            const uint32_t I4 = (y < inHeight - 1 && x < inWidth - 2) ? sa3[pnext2] : sa2[0];

            if (n == 2) {
                E[0] = E[1] = E[nl] = E[nl + 1] = PE;
                FILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, 0, 1, nl, nl+1);
                FILT2(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, nl, 0, nl+1, 1);
                FILT2(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, nl+1, nl, 1, 0);
                FILT2(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, 1, nl+1, 0, nl);
            } else if (n == 3) {
                E[0] = E[1] = E[2] = E[nl] = E[nl+1] = E[nl+2] = E[nl1] = E[nl1+1] = E[nl1+2] = PE;
                FILT3(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, 0, 1, 2, nl, nl+1, nl+2, nl1, nl1+1, nl1+2);
                FILT3(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, nl1, nl, 0, nl1+1, nl+1, 1, nl1+2, nl+2, 2);
                FILT3(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, nl1+2, nl1+1, nl1, nl+2, nl+1, nl, 2, 1, 0);
                FILT3(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, 2, nl+2, nl1+2, 1, nl+1, nl1+1, 0, nl, nl1);
            } else if (n == 4) {
                E[0] = E[1] = E[2] = E[3] = E[nl] = E[nl+1] = E[nl+2] = E[nl+3] = E[nl1] = E[nl1+1] = E[nl1+2] = E[nl1+3] = E[nl2] = E[nl2+1] = E[nl2+2] = E[nl2+3] = PE;
                FILT4(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, nl2+3, nl2+2, nl1+3, 3, nl+3, nl1+2, nl2+1, nl2, nl1+1, nl+2, 2, 1, nl+1, nl1, nl, 0);
                FILT4(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, 3, nl+3, 2, 0, 1, nl+2, nl1+3, nl2+3, nl1+2, nl+1, nl, nl1, nl1+1, nl2+2, nl2+1, nl2);
                FILT4(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, 0, 1, nl, nl2, nl1, nl+1, 2, 3, nl+2, nl1+1, nl2+1, nl2+2, nl1+2, nl+3, nl1+3, nl2+3);
                FILT4(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, nl2, nl1, nl2+1, nl2+3, nl2+2, nl1+1, nl, 0, nl+1, nl1+2, nl1+3, nl+3, nl+2, 1, 2, 3);
            }

            sa0++; sa1++; sa2++; sa3++; sa4++;
            E += n;
        }
    }
}

int xbr_scale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, int factor) {
    if (factor < 2 || factor > 4) return -1;
    if (sw <= 0 || sh <= 0) return -1;
    if (dw != sw * factor || dh != sh * factor) return -1;
    
    int inPitch = sw * 4;
    int outPitch = dw * 4;
    
    xbr_filter(src, dst, sw, sh, inPitch, outPitch, factor);
    
    return 0;
}
