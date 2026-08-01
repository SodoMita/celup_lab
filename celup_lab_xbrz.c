/* xBRZ pixel-art upscaler, C port of Zenju's xBRZ 1.8 (via bell345/xbrz-rs).
 *
 * Supports integer scale factors 2..6.  Operates on premultiplied-linear
 * RGBA u8 input/output, same convention as celup_lab.
 */
#include "celup_lab_xbrz.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <stdio.h>
typedef uint32_t px_t;
static inline uint8_t px_r(px_t p){ return p & 0xFF; }
static inline uint8_t px_g(px_t p){ return (p>>8) & 0xFF; }
static inline uint8_t px_b(px_t p){ return (p>>16) & 0xFF; }
static inline uint8_t px_a(px_t p){ return (p>>24) & 0xFF; }
static inline px_t px_mk(uint8_t r,uint8_t g,uint8_t b,uint8_t a){
    return r | ((uint32_t)g<<8) | ((uint32_t)b<<16) | ((uint32_t)a<<24);
}

/* ---- YCbCr distance LUT (5-bit, ~128KB) ---- */
static float ycbcr_lut[0x8000];
static int ycbcr_ready = 0;

static double ycbcr_dist(double rd, double gd, double bd){
    const double KB=0.0593, KR=0.2627, KG=1.0-KB-KR;
    double y = KR*rd + KG*gd + KB*bd;
    double cb = 0.5/(1.0-KB)*(bd - y);
    double cr = 0.5/(1.0-KR)*(rd - y);
    return sqrt(y*y + cb*cb + cr*cr);
}

static void init_lut(void){
    if (ycbcr_ready) return;
    for (int i=0;i<0x8000;i++){
        int16_t rd=(int8_t)(((i>>10)&0x1F)<<3)*2;
        int16_t gd=(int8_t)(((i>>5)&0x1F)<<3)*2;
        int16_t bd=(int8_t)((i&0x1F)<<3)*2;
        ycbcr_lut[i]=(float)ycbcr_dist(rd,gd,bd);
    }
    ycbcr_ready=1;
}

static inline float px_dist(px_t a, px_t b){
    int rd=(int)px_r(a)-(int)px_r(b);
    int gd=(int)px_g(a)-(int)px_g(b);
    int bd=(int)px_b(a)-(int)px_b(b);
    /* Clamp to [-128,127] then map to unsigned [0,255] for LUT index */
    int rp = ((rd/2) >> 3) & 0x1F;
    int gp = ((gd/2) >> 3) & 0x1F;
    int bp = ((bd/2) >> 3) & 0x1F;
    if (rp < 0) rp += 32;
    if (gp < 0) gp += 32;
    if (bp < 0) bp += 32;
    float d = ycbcr_lut[(rp<<10)|(gp<<5)|bp];
    int a1=px_a(a), a2=px_a(b);
    if (a1<a2) return (a1/255.0f)*d + (a2-a1);
    return (a2/255.0f)*d + (a1-a2);
}

/* ---- Config (xBRZ 1.8 defaults) ---- */
#define EQUAL_TOL  30.0f
#define DIR_BIAS    4.0f
#define DOM_THRESH  3.6f
#define STEEP_THRESH 2.2f

/* ---- Kernel: 4x4 with positions
   A B C D
   E F G H
   I J K L
   M N O P
*/
typedef struct { px_t a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p; } K;

typedef enum { BN=0, BNorm=1, BDom=2 } bt;
typedef struct { bt tl,tr,bl,br; } B2;

/* ---- Alpha blend ---- */
static inline uint8_t bchan(uint8_t bk,uint8_t ft,int m,int n){
    return (uint8_t)(bk + (ft-bk)*m/n);
}
static inline px_t pxblend(px_t bk,px_t ft,int m,int n){
    return px_mk(bchan(px_r(bk),px_r(ft),m,n),
                 bchan(px_g(bk),px_g(ft),m,n),
                 bchan(px_b(bk),px_b(ft),m,n),
                 bchan(px_a(bk),px_a(ft),m,n));
}

/* ---- Corner preprocessing ---- */
static B2 preproc(const K *k){
    B2 r={BN,BN,BN,BN};
    if (k->f==k->g && k->j==k->k) return r;
    if (k->f==k->j && k->g==k->k) return r;
    float jg = px_dist(k->i,k->f)+px_dist(k->f,k->c)+px_dist(k->n,k->k)+px_dist(k->k,k->h)+DIR_BIAS*px_dist(k->j,k->g);
    float fk = px_dist(k->e,k->j)+px_dist(k->j,k->o)+px_dist(k->b,k->g)+px_dist(k->g,k->l)+DIR_BIAS*px_dist(k->f,k->k);
    if (jg<fk){
        bt m = (DOM_THRESH*jg<fk)?BDom:BNorm;
        if (k->f!=k->g && k->f!=k->j) r.tl=m;
        if (k->k!=k->j && k->k!=k->g) r.br=m;
    } else if (fk<jg){
        bt m = (DOM_THRESH*fk<jg)?BDom:BNorm;
        if (k->j!=k->f && k->j!=k->k) r.bl=m;
        if (k->g!=k->f && k->g!=k->k) r.tr=m;
    }
    return r;
}

/* ---- Per-scale blend patterns.
   Each takes (col, out_block, stride) and blends into the BOTTOM-RIGHT
   corner of the factor x factor block.  Rotated by the caller for other corners.
*/

/* 2x2: out[2][2], only bottom-right [1][1] is blended */
static void bl2_shallow(px_t c, px_t *o, int s){
    o[0*s+1]=pxblend(o[0*s+1],c,1,4);
    o[1*s+1]=pxblend(o[1*s+1],c,3,4);
}
static void bl2_steep(px_t c, px_t *o, int s){
    o[1*s+0]=pxblend(o[1*s+0],c,1,4);
    o[1*s+1]=pxblend(o[1*s+1],c,3,4);
}
static void bl2_both(px_t c, px_t *o, int s){
    o[0*s+1]=pxblend(o[0*s+1],c,1,4);
    o[1*s+0]=pxblend(o[1*s+0],c,1,4);
    o[1*s+1]=pxblend(o[1*s+1],c,5,6);
}
static void bl2_diag(px_t c, px_t *o, int s){
    o[1*s+1]=pxblend(o[1*s+1],c,1,2);
}
static void bl2_corner(px_t c, px_t *o, int s){
    o[1*s+1]=pxblend(o[1*s+1],c,21,100);
}

/* 3x3 */
static void bl3_shallow(px_t c, px_t *o, int s){
    o[0*s+2]=pxblend(o[0*s+2],c,1,4);
    o[2*s+1]=pxblend(o[2*s+1],c,1,4);
    o[1*s+2]=pxblend(o[1*s+2],c,3,4);
    o[2*s+2]=c;
}
static void bl3_steep(px_t c, px_t *o, int s){
    o[2*s+0]=pxblend(o[2*s+0],c,1,4);
    o[1*s+2]=pxblend(o[1*s+2],c,1,4);
    o[2*s+1]=pxblend(o[2*s+1],c,3,4);
    o[2*s+2]=c;
}
static void bl3_both(px_t c, px_t *o, int s){
    o[0*s+2]=pxblend(o[0*s+2],c,1,4);
    o[2*s+0]=pxblend(o[2*s+0],c,1,4);
    o[1*s+2]=pxblend(o[1*s+2],c,3,4);
    o[2*s+1]=pxblend(o[2*s+1],c,3,4);
    o[2*s+2]=c;
}
static void bl3_diag(px_t c, px_t *o, int s){
    o[1*s+2]=pxblend(o[1*s+2],c,1,8);
    o[2*s+1]=pxblend(o[2*s+1],c,1,8);
    o[2*s+2]=pxblend(o[2*s+2],c,7,8);
}
static void bl3_corner(px_t c, px_t *o, int s){
    o[2*s+2]=pxblend(o[2*s+2],c,45,100);
}

/* 4x4 */
static void bl4_shallow(px_t c, px_t *o, int s){
    o[0*s+3]=pxblend(o[0*s+3],c,1,4);
    o[2*s+2]=pxblend(o[2*s+2],c,1,4);
    o[1*s+3]=pxblend(o[1*s+3],c,3,4);
    o[3*s+2]=pxblend(o[3*s+2],c,3,4);
    o[2*s+3]=c; o[3*s+3]=c;
}
static void bl4_steep(px_t c, px_t *o, int s){
    o[3*s+0]=pxblend(o[3*s+0],c,1,4);
    o[2*s+2]=pxblend(o[2*s+2],c,1,4);
    o[3*s+1]=pxblend(o[3*s+1],c,3,4);
    o[2*s+3]=pxblend(o[2*s+3],c,3,4);
    o[3*s+2]=c; o[3*s+3]=c;
}
static void bl4_both(px_t c, px_t *o, int s){
    o[1*s+3]=pxblend(o[1*s+3],c,3,4);
    o[3*s+1]=pxblend(o[3*s+1],c,3,4);
    o[0*s+3]=pxblend(o[0*s+3],c,1,4);
    o[3*s+0]=pxblend(o[3*s+0],c,1,4);
    o[2*s+2]=pxblend(o[2*s+2],c,1,3);
    o[3*s+2]=c; o[2*s+3]=c; o[3*s+3]=c;
}
static void bl4_diag(px_t c, px_t *o, int s){
    o[2*s+3]=pxblend(o[2*s+3],c,1,2);
    o[3*s+2]=pxblend(o[3*s+2],c,1,2);
    o[3*s+3]=c;
}
static void bl4_corner(px_t c, px_t *o, int s){
    o[3*s+3]=pxblend(o[3*s+3],c,68,100);
    o[3*s+2]=pxblend(o[3*s+2],c,9,100);
    o[2*s+3]=pxblend(o[2*s+3],c,9,100);
}

/* 5x5 */
static void bl5_shallow(px_t c, px_t *o, int s){
    o[0*s+4]=pxblend(o[0*s+4],c,1,4);
    o[2*s+3]=pxblend(o[2*s+3],c,1,4);
    o[4*s+2]=pxblend(o[4*s+2],c,1,4);
    o[1*s+4]=pxblend(o[1*s+4],c,3,4);
    o[3*s+3]=pxblend(o[3*s+3],c,3,4);
    o[2*s+4]=c; o[3*s+4]=c; o[4*s+3]=c; o[4*s+4]=c;
}
static void bl5_steep(px_t c, px_t *o, int s){
    o[4*s+0]=pxblend(o[4*s+0],c,1,4);
    o[3*s+2]=pxblend(o[3*s+2],c,1,4);
    o[2*s+4]=pxblend(o[2*s+4],c,1,4);
    o[4*s+1]=pxblend(o[4*s+1],c,3,4);
    o[3*s+3]=pxblend(o[3*s+3],c,3,4);
    o[4*s+2]=c; o[4*s+3]=c; o[3*s+4]=c; o[4*s+4]=c;
}
static void bl5_both(px_t c, px_t *o, int s){
    o[0*s+4]=pxblend(o[0*s+4],c,1,4);
    o[2*s+3]=pxblend(o[2*s+3],c,1,4);
    o[1*s+4]=pxblend(o[1*s+4],c,3,4);
    o[3*s+3]=pxblend(o[3*s+3],c,3,4);
    o[4*s+0]=pxblend(o[4*s+0],c,1,4);
    o[3*s+2]=pxblend(o[3*s+2],c,1,4);
    o[4*s+1]=pxblend(o[4*s+1],c,3,4);
    o[3*s+3]=pxblend(o[3*s+3],c,2,3);
    o[2*s+4]=c; o[3*s+4]=c; o[4*s+2]=c; o[4*s+3]=c; o[4*s+4]=c;
}
static void bl5_diag(px_t c, px_t *o, int s){
    o[1*s+4]=pxblend(o[1*s+4],c,1,8);
    o[2*s+3]=pxblend(o[2*s+3],c,1,8);
    o[4*s+2]=pxblend(o[4*s+2],c,1,8);
    o[3*s+4]=pxblend(o[3*s+4],c,7,8);
    o[4*s+3]=pxblend(o[4*s+3],c,7,8);
    o[4*s+4]=c;
}
static void bl5_corner(px_t c, px_t *o, int s){
    o[4*s+4]=pxblend(o[4*s+4],c,86,100);
    o[4*s+3]=pxblend(o[4*s+3],c,23,100);
    o[3*s+4]=pxblend(o[3*s+4],c,23,100);
}

/* 6x6 */
static void bl6_shallow(px_t c, px_t *o, int s){
    o[0*s+5]=pxblend(o[0*s+5],c,1,4);
    o[2*s+4]=pxblend(o[2*s+4],c,1,4);
    o[4*s+3]=pxblend(o[4*s+3],c,1,4);
    o[1*s+5]=pxblend(o[1*s+5],c,3,4);
    o[3*s+4]=pxblend(o[3*s+4],c,3,4);
    o[5*s+3]=pxblend(o[5*s+3],c,3,4);
    o[2*s+5]=c; o[3*s+5]=c; o[4*s+5]=c; o[5*s+5]=c;
    o[4*s+4]=c;
}
static void bl6_steep(px_t c, px_t *o, int s){
    o[5*s+0]=pxblend(o[5*s+0],c,1,4);
    o[4*s+2]=pxblend(o[4*s+2],c,1,4);
    o[3*s+4]=pxblend(o[3*s+4],c,1,4);
    o[5*s+1]=pxblend(o[5*s+1],c,3,4);
    o[4*s+3]=pxblend(o[4*s+3],c,3,4);
    o[3*s+5]=pxblend(o[3*s+5],c,3,4);
    o[5*s+2]=c; o[5*s+3]=c; o[5*s+4]=c; o[5*s+5]=c;
    o[4*s+4]=c;
}
static void bl6_both(px_t c, px_t *o, int s){
    o[0*s+5]=pxblend(o[0*s+5],c,1,4);
    o[2*s+4]=pxblend(o[2*s+4],c,1,4);
    o[1*s+5]=pxblend(o[1*s+5],c,3,4);
    o[3*s+4]=pxblend(o[3*s+4],c,3,4);
    o[5*s+0]=pxblend(o[5*s+0],c,1,4);
    o[4*s+2]=pxblend(o[4*s+2],c,1,4);
    o[5*s+1]=pxblend(o[5*s+1],c,3,4);
    o[4*s+3]=pxblend(o[4*s+3],c,3,4);
    o[2*s+5]=c; o[3*s+5]=c; o[4*s+5]=c; o[5*s+5]=c;
    o[4*s+4]=c; o[5*s+4]=c;
    o[5*s+2]=c; o[5*s+3]=c;
}
static void bl6_diag(px_t c, px_t *o, int s){
    o[2*s+5]=pxblend(o[2*s+5],c,1,2);
    o[3*s+4]=pxblend(o[3*s+4],c,1,2);
    o[4*s+3]=pxblend(o[4*s+3],c,1,2);
    o[4*s+5]=c; o[5*s+5]=c; o[5*s+4]=c;
}
static void bl6_corner(px_t c, px_t *o, int s){
    o[5*s+5]=pxblend(o[5*s+5],c,97,100);
    o[4*s+5]=pxblend(o[4*s+5],c,42,100);
    o[5*s+4]=pxblend(o[5*s+4],c,42,100);
    o[5*s+3]=pxblend(o[5*s+3],c,6,100);
    o[3*s+5]=pxblend(o[3*s+5],c,6,100);
}

/* ---- Rotation helpers ----
   The 3x3 kernel (from 4x4, dropping D/H/L/P column):
     a b c
     e f g
     i j k
   Rotation maps (rot=0,90,180,270 CW):
     a b c     i e a     k j i     c g k
     e f g  -> j f b  -> g f e  -> f f f   (wrong, let me redo)
     i j k     k j i     c b a     a e i
   Correct 90 CW: a->c->k->i->a, b->g->j->e->b, c->k->i->a, etc.
   Actually the standard rotation of a 3x3 matrix:
     rot0:   [0,0]=a [0,1]=b [0,2]=c
             [1,0]=e [1,1]=f [1,2]=g
             [2,0]=i [2,1]=j [2,2]=k
     rot90:  [0,0]=i [0,1]=e [0,2]=a
             [1,0]=j [1,1]=f [1,2]=b
             [2,0]=k [2,1]=g [2,2]=c
     rot180: [0,0]=k [0,1]=j [0,2]=i
             [1,0]=g [1,1]=f [1,2]=e
             [2,0]=c [2,1]=b [2,2]=a
     rot270: [0,0]=c [0,1]=g [0,2]=k
             [1,0]=b [1,1]=f [1,2]=j
             [2,0]=a [2,1]=e [2,2]=i
*/
static inline px_t kget(const K *k, int row, int col, int rot){
    px_t base[9] = {k->a,k->b,k->c, k->e,k->f,k->g, k->i,k->j,k->k};
    int idx;
    switch(rot){
    case 0: idx = row*3+col; break;
    case 1: idx = (2-col)*3+row; break;       /* 90 CW */
    case 2: idx = (2-row)*3+(2-col); break;   /* 180 */
    case 3: idx = col*3+(2-row); break;       /* 270 CW */
    default: idx = row*3+col;
    }
    return base[idx];
}

/* Apply blend function rotated.  The blend function writes to positions
   relative to the BOTTOM-RIGHT corner (factor-1, factor-1).  We rotate
   the output coordinates. */
static inline void rot_pos(int factor, int rot, int *pr, int *pc){
    /* Rotate position (r,c) in factor x factor block. */
    int n = factor - 1;
    switch(rot){
    case 0: *pr = *pr; *pc = *pc; break;
    case 1: { int t=*pr; *pr = n-*pc; *pc = t; break; }
    case 2: { *pr = n-*pr; *pc = n-*pc; break; }
    case 3: { int t=*pr; *pr = *pc; *pc = n-t; break; }
    }
}

/* ---- Main scale ---- */
int xbrz_scale(const uint8_t *src, int sw, int sh,
               uint8_t *dst, int dw, int dh, int factor){
    if (factor<2||factor>6) return -1;
    if (sw<=0||sh<=0) return -1;
    if (dw!=sw*factor||dh!=sh*factor) return -1;

    init_lut();

    /* Step 1: fill output with nearest-neighbor. */
    for (int y=0;y<sh;y++){
        const px_t *row = (const px_t *)(src + 4*y*sw);
        for (int x=0;x<sw;x++){
            px_t p = row[x];
            for (int dy=0;dy<factor;dy++){
                px_t *drow = (px_t *)(dst + 4*((y*factor+dy)*dw + x*factor));
                for (int dx=0;dx<factor;dx++) drow[dx]=p;
            }
        }
    }
    /* Step 2: per-pixel edge blending. */
    for (int y=0;y<sh;y++){
        for (int x=0;x<sw;x++){
            #define KG(dx,dy) (*(const px_t*)(src+4*((y+(dy)<0?0:y+(dy)>=sh?sh-1:y+(dy))*sw+(x+(dx)<0?0:x+(dx)>=sw?sw-1:x+(dx)))))
            K k = { .a=KG(-1,-1),.b=KG(0,-1),.c=KG(1,-1),.d=KG(2,-1),
                    .e=KG(-1,0),.f=KG(0,0),.g=KG(1,0),.h=KG(2,0),
                    .i=KG(-1,1),.j=KG(0,1),.k=KG(1,1),.l=KG(2,1),
                    .m=KG(-1,2),.n=KG(0,2),.o=KG(1,2),.p=KG(2,2) };
            #undef KG

            B2 blend = preproc(&k);
            if (blend.tl==BN && blend.tr==BN && blend.bl==BN && blend.br==BN) continue;

            px_t px = (px_dist(k.f,k.g)<=px_dist(k.f,k.k)) ? k.g : k.k;

            /* Line-blend decision. */
            int doline = 1;
            if (blend.tl!=BNorm){
                /* Only dominant gets full line blend unconditionally. */
            }
            if (blend.tr!=BN && k.f!=k.i) doline=0;
            if (blend.bl!=BN && k.f!=k.c) doline=0;
            if (k.f!=k.j && k.i==k.h && k.h==k.j && k.j==k.g && k.g==k.c) doline=0;

            /* Pick blend fns for this scale. */
            void (*fn_sh)(px_t,px_t*,int)=0, (*fn_st)(px_t,px_t*,int)=0,
                 (*fn_bo)(px_t,px_t*,int)=0, (*fn_di)(px_t,px_t*,int)=0,
                 (*fn_co)(px_t,px_t*,int)=0;
            switch(factor){
            case 2: fn_sh=bl2_shallow; fn_st=bl2_steep; fn_bo=bl2_both; fn_di=bl2_diag; fn_co=bl2_corner; break;
            case 3: fn_sh=bl3_shallow; fn_st=bl3_steep; fn_bo=bl3_both; fn_di=bl3_diag; fn_co=bl3_corner; break;
            case 4: fn_sh=bl4_shallow; fn_st=bl4_steep; fn_bo=bl4_both; fn_di=bl4_diag; fn_co=bl4_corner; break;
            case 5: fn_sh=bl5_shallow; fn_st=bl5_steep; fn_bo=bl5_both; fn_di=bl5_diag; fn_co=bl5_corner; break;
            case 6: fn_sh=bl6_shallow; fn_st=bl6_steep; fn_bo=bl6_both; fn_di=bl6_diag; fn_co=bl6_corner; break;
            }

            /* Apply to each corner that needs blending.  The blend function
               writes relative to bottom-right; we rotate for other corners. */
            int oy = y*factor, ox = x*factor;
            for (int rot=0;rot<4;rot++){
                B2 rb = blend;
                /* Rotate blend info so rb.br is the current corner. */
                switch(rot){
                case 0: break;
                case 1: rb=(B2){blend.bl,blend.tl,blend.br,blend.tr}; break;
                case 2: rb=(B2){blend.br,blend.bl,blend.tr,blend.tl}; break;
                case 3: rb=(B2){blend.tr,blend.br,blend.tl,blend.bl}; break;
                }
                if (rb.br==BN) continue;

                /* Adjust output pointer to the correct quadrant for this corner.
                   rot=0: top-left, rot=1: top-right, rot=2: bottom-right, rot=3: bottom-left */
                int qrow = (rot >= 2) ? (factor/2) : 0;
                int qcol = (rot == 1 || rot == 2) ? (factor/2) : 0;
                px_t *out = (px_t*)(dst + 4*((oy+qrow)*dw + (ox+qcol)));

                /* Rotate kernel for this corner. */
                K rk;
                rk.a=kget(&k,0,0,rot); rk.b=kget(&k,0,1,rot); rk.c=kget(&k,0,2,rot);
                rk.e=kget(&k,1,0,rot); rk.f=kget(&k,1,1,rot); rk.g=kget(&k,1,2,rot);
                rk.i=kget(&k,2,0,rot); rk.j=kget(&k,2,1,rot); rk.k=kget(&k,2,2,rot);
                /* Recompute px for this rotation. */
                px_t rpx = (px_dist(rk.f,rk.g)<=px_dist(rk.f,rk.k)) ? rk.g : rk.k;

                if (doline){
                    float fg = px_dist(rk.g,rk.h);
                    float hc = px_dist(rk.k,rk.c);
                    int shallow = (STEEP_THRESH*fg<=hc) && (rk.f!=rk.i) && (rk.e!=rk.i);
                    int steep   = (STEEP_THRESH*hc<=fg) && (rk.f!=rk.c) && (rk.b!=rk.c);
                    void (*fn)(px_t,px_t*,int)=0;
                    if (shallow&&steep) fn=fn_bo;
                    else if (shallow) fn=fn_sh;
                    else if (steep) fn=fn_st;
                    else fn=fn_di;
                    fn(rpx, out, dw);
                } else {
                    fn_co(rpx, out, dw);
                }
            }
        }
    }
    return 0;
}

/* Additional blend patterns for better corner handling */
static void bl2_corner_enhanced(px_t c, px_t *o, int s){
    /* Stronger corner blend for 2x */
    o[1*s+1]=pxblend(o[1*s+1],c,35,100);
}

static void bl3_corner_enhanced(px_t c, px_t *o, int s){
    /* Stronger corner blend for 3x */
    o[2*s+2]=pxblend(o[2*s+2],c,55,100);
    o[1*s+2]=pxblend(o[1*s+2],c,15,100);
    o[2*s+1]=pxblend(o[2*s+1],c,15,100);
}

/* Future enhancements:
 * - Multi-pass refinement for smoother gradients
 * - Integration with autodeblur pipeline (xBRZ as pre-processor)
 * - Configurable blend strengths via command-line options
 * - SIMD optimization for YCbCr distance calculation
 * - Support for non-integer scales via intermediate rendering
 */
