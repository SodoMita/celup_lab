#ifndef WEBP_DECODE_H
#define WEBP_DECODE_H
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum VP8StatusCode { VP8_STATUS_OK = 0 } VP8StatusCode;
uint8_t* WebPDecodeRGBA(const uint8_t* data, size_t data_size, int* width, int* height);
uint8_t* WebPDecodeRGB(const uint8_t* data, size_t data_size, int* width, int* height);
void WebPFree(void* ptr);
#ifdef __cplusplus
}
#endif
#endif
