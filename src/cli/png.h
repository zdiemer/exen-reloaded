#ifndef EXEN_CLI_PNG_H
#define EXEN_CLI_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Write w*h RGB332 pixels as an RGB PNG, each pixel scaled to scale x scale. 0 on success. */
int png_write_rgb332(const char *path, const uint8_t *px, int w, int h, int scale);
/* The same for w*h 0x??RRGGBB pixels (libretro XRGB8888 frames). */
int png_write_xrgb8888(const char *path, const uint32_t *px, int w, int h, int scale);

/* The PNG file image in memory instead (malloc'd, free() it; NULL if out of memory), for
 * callers with their own file I/O. */
uint8_t *png_encode_rgb332(const uint8_t *px, int w, int h, int scale, size_t *len);
uint8_t *png_encode_xrgb8888(const uint32_t *px, int w, int h, int scale, size_t *len);

#endif
