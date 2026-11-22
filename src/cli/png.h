#ifndef EXEN_CLI_PNG_H
#define EXEN_CLI_PNG_H

#include <stdint.h>

/* Write w*h RGB332 pixels as an RGB PNG, each pixel scaled to scale x scale. 0 on success. */
int png_write_rgb332(const char *path, const uint8_t *px, int w, int h, int scale);
/* The same for w*h 0x??RRGGBB pixels (libretro XRGB8888 frames). */
int png_write_xrgb8888(const char *path, const uint32_t *px, int w, int h, int scale);

#endif
