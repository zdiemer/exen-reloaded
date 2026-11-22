#ifndef EXEN_CLI_PNG_H
#define EXEN_CLI_PNG_H

#include <stdint.h>

/* Write w*h RGB332 pixels as an RGB PNG, each pixel scaled to scale x scale. 0 on success. */
int png_write_rgb332(const char *path, const uint8_t *px, int w, int h, int scale);

#endif
