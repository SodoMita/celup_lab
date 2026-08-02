#ifndef WEBP_ENCODE_H
#define WEBP_ENCODE_H
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t WebPEncodeLosslessRGBA(const uint8_t* rgba, int width, int height, int stride, uint8_t** output);
void WebPFree(void* ptr);
#ifdef __cplusplus
}
#endif
#endif
