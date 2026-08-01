#ifndef WEBP_DECODE_H_
#define WEBP_DECODE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t *WebPDecodeRGBA(const uint8_t *data, size_t data_size, int *width, int *height);
void WebPFree(void *ptr);

#ifdef __cplusplus
}
#endif

#endif  /* WEBP_DECODE_H_ */
