/* Minimal PNG writer for frame dumps: RGB332 in, 8-bit RGB out, zlib "stored" blocks (no
 * compression). Standard C only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
    if (crc_table[1])
        return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t c, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    uint32_t c = crc_update(0xffffffffu, h + 4, 4);
    c = crc_update(c, data, len) ^ 0xffffffffu;
    uint8_t t[4];
    be32(t, c);
    return fwrite(h, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) &&
           fwrite(t, 1, 4, f) == 4;
}

/* RGB332 -> RGB888 by bit replication */
static void expand(uint8_t px, uint8_t *o)
{
    uint8_t r = (uint8_t)(px >> 5), g = (uint8_t)((px >> 2) & 7), b = (uint8_t)(px & 3);
    o[0] = (uint8_t)(r << 5 | r << 2 | r >> 1);
    o[1] = (uint8_t)(g << 5 | g << 2 | g >> 1);
    o[2] = (uint8_t)(b << 6 | b << 4 | b << 2 | b);
}

int png_write_rgb332(const char *path, const uint8_t *px, int w, int h, int scale)
{
    if (scale < 1)
        scale = 1;
    crc_init();
    int W = w * scale, H = h * scale;
    size_t row = 1 + (size_t)W * 3, raw_len = row * (size_t)H;
    uint8_t *raw = malloc(raw_len);
    if (!raw)
        return -1;
    for (int y = 0; y < H; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0; /* filter: none */
        for (int x = 0; x < W; x++)
            expand(px[(size_t)(y / scale) * (size_t)w + (size_t)(x / scale)], r + 1 + 3 * (size_t)x);
    }
    /* zlib stream of stored blocks */
    size_t nblk = raw_len / 65535 + 1;
    size_t z_len = 2 + raw_len + 5 * nblk + 4;
    uint8_t *z = malloc(z_len);
    if (!z) {
        free(raw);
        return -1;
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_len; i++) {
        s1 = (s1 + raw[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    size_t pos = 0;
    do {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        int last = pos + n == raw_len;
        z[o++] = (uint8_t)last;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + pos, n);
        o += n;
        pos += n;
    } while (pos < raw_len);
    be32(z + o, s2 << 16 | s1);
    o += 4;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return -1;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)W);
    be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* truecolour */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    int ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
             chunk(f, "IDAT", z, (uint32_t)o) && chunk(f, "IEND", NULL, 0);
    free(z);
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}
