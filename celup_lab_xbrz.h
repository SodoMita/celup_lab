/* xBRZ pixel-art upscaler, C port of Zenju's xBRZ 1.8
 * (via bell345/xbrz-rs Rust port).  GPLv3.
 *
 * Supports integer scale factors 2..6.  Operates on premultiplied-linear
 * RGBA u8 input/output, same convention as the rest of celup_lab.
 */
#ifndef CELUP_LAB_XBRZ_H
#define CELUP_LAB_XBRZ_H

#include <stdint.h>
#include <stddef.h>

/* Scale an RGBA image by an integer factor (2..6).
 * src/dst are premultiplied-linear u8 RGBA, row-major.
 * Returns 0 on success, -1 on bad args. */
int xbrz_scale(const uint8_t *src, int sw, int sh,
               uint8_t *dst, int dw, int dh, int factor);

#endif
