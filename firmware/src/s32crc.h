#ifndef S32CRC_H
#define S32CRC_H
#include <stdint.h>
static inline uint32_t s32_crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
#endif
