/* RGB332 -> RGB conversion as the original display path computes it, for frontends.
 *
 * The core presents RGB332 frames (ExenHost.present). gensimu.exe converts them for the PC
 * screen in EXManufDisplayUpdate (0x4342f0): each 3-bit channel is multiplied by 36.4286 and
 * each 2-bit channel by 85.0, then truncated. That differs from bit replication for some levels
 * (level 2 gives 72, bit replication 73), so frontends share this table to show exactly the
 * original's colours. Header-only and dependency-free.
 */
#ifndef EXEN_RGB332_H
#define EXEN_RGB332_H

#include <stdint.h>

/* 0xffRRGGBB for an RGB332 pixel. */
static inline uint32_t exen_rgb332_to_xrgb8888(uint8_t p)
{
    uint32_t r = (uint32_t)((p >> 5) * 36.4286), g = (uint32_t)(((p >> 2) & 7) * 36.4286),
             b = (uint32_t)((p & 3) * 85.0);
    return 0xff000000u | r << 16 | g << 8 | b;
}

/* Fill lut[256] with exen_rgb332_to_xrgb8888 for every pixel value. */
static inline void exen_rgb332_lut(uint32_t lut[256])
{
    for (int p = 0; p < 256; p++)
        lut[p] = exen_rgb332_to_xrgb8888((uint8_t)p);
}

#endif
