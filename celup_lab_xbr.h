#ifndef CELUP_LAB_XBR_H
#define CELUP_LAB_XBR_H

#include <stdint.h>

/* xBR pixel art upscaler - simpler than xBRZ
 * Supports integer scale factors 2, 3, 4
 */
int xbr_scale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, int factor);

#endif
