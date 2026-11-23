/* Minimal PNG writer for frame dumps and screenshots: RGB332 or XRGB8888 in, 8-bit RGB out,
 * zlib "stored" blocks (no compression). Standard C only. */
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

/* Append a chunk at `o` in `out`; returns the new end. */
static uint8_t *chunk(uint8_t *o, const char *type, const uint8_t *data, uint32_t len)
{
    be32(o, len);
    memcpy(o + 4, type, 4);
    if (len)
        memcpy(o + 8, data, len);
    be32(o + 8 + len, crc_update(0xffffffffu, o + 4, 4 + (size_t)len) ^ 0xffffffffu);
    return o + 12 + len;
}

/* RGB332 -> RGB888 by bit replication */
static void expand(uint8_t px, uint8_t *o)
{
    uint8_t r = (uint8_t)(px >> 5), g = (uint8_t)((px >> 2) & 7), b = (uint8_t)(px & 3);
    o[0] = (uint8_t)(r << 5 | r << 2 | r >> 1);
    o[1] = (uint8_t)(g << 5 | g << 2 | g >> 1);
    o[2] = (uint8_t)(b << 6 | b << 4 | b << 2 | b);
}

/* Encode W x H RGB rows already laid out in `raw` (filter byte + 3 bytes per pixel each) as a
 * PNG file image; frees `raw`. */
static uint8_t *encode_rgb_rows(uint8_t *raw, size_t raw_len, int W, int H, size_t *out_len)
{
    crc_init();
    /* zlib stream of stored blocks */
    size_t nblk = raw_len / 65535 + 1;
    size_t z_len = 2 + raw_len + 5 * nblk + 4;
    uint8_t *png = malloc(8 + 25 + 12 + z_len + 12);
    if (!png) {
        free(raw);
        return NULL;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    memcpy(png, sig, 8);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)W);
    be32(ihdr + 4, (uint32_t)H);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* truecolour */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    uint8_t *o = chunk(png + 8, "IHDR", ihdr, 13);
    /* IDAT: built in place after its 8-byte header */
    uint8_t *z = o + 8;
    size_t zo = 0;
    z[zo++] = 0x78;
    z[zo++] = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_len; i++) {
        s1 = (s1 + raw[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    size_t pos = 0;
    do {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        int last = pos + n == raw_len;
        z[zo++] = (uint8_t)last;
        z[zo++] = (uint8_t)n;
        z[zo++] = (uint8_t)(n >> 8);
        z[zo++] = (uint8_t)~n;
        z[zo++] = (uint8_t)(~n >> 8);
        memcpy(z + zo, raw + pos, n);
        zo += n;
        pos += n;
    } while (pos < raw_len);
    be32(z + zo, s2 << 16 | s1);
    zo += 4;
    free(raw);
    be32(o, (uint32_t)zo);
    memcpy(o + 4, "IDAT", 4);
    be32(o + 8 + zo, crc_update(0xffffffffu, o + 4, 4 + zo) ^ 0xffffffffu);
    o += 12 + zo;
    o = chunk(o, "IEND", NULL, 0);
    *out_len = (size_t)(o - png);
    return png;
}

static int write_file(const char *path, uint8_t *png, size_t len)
{
    if (!png)
        return -1;
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(png, 1, len, f) == len;
    if (f && fclose(f) != 0)
        ok = 0;
    free(png);
    return ok ? 0 : -1;
}

static uint8_t *rows_rgb332(const uint8_t *px, int w, int h, int scale, int *W, int *H, size_t *raw_len)
{
    if (scale < 1)
        scale = 1;
    *W = w * scale;
    *H = h * scale;
    size_t row = 1 + (size_t)*W * 3;
    *raw_len = row * (size_t)*H;
    uint8_t *raw = malloc(*raw_len);
    if (!raw)
        return NULL;
    for (int y = 0; y < *H; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0; /* filter: none */
        for (int x = 0; x < *W; x++)
            expand(px[(size_t)(y / scale) * (size_t)w + (size_t)(x / scale)], r + 1 + 3 * (size_t)x);
    }
    return raw;
}

uint8_t *png_encode_rgb332(const uint8_t *px, int w, int h, int scale, size_t *len)
{
    int W, H;
    size_t raw_len;
    uint8_t *raw = rows_rgb332(px, w, h, scale, &W, &H, &raw_len);
    return raw ? encode_rgb_rows(raw, raw_len, W, H, len) : NULL;
}

int png_write_rgb332(const char *path, const uint8_t *px, int w, int h, int scale)
{
    size_t len = 0;
    uint8_t *png = png_encode_rgb332(px, w, h, scale, &len);
    return write_file(path, png, len);
}

uint8_t *png_encode_xrgb8888(const uint32_t *px, int w, int h, int scale, size_t *len)
{
    if (scale < 1)
        scale = 1;
    int W = w * scale, H = h * scale;
    size_t row = 1 + (size_t)W * 3, raw_len = row * (size_t)H;
    uint8_t *raw = malloc(raw_len);
    if (!raw)
        return NULL;
    for (int y = 0; y < H; y++) {
        uint8_t *r = raw + (size_t)y * row;
        r[0] = 0; /* filter: none */
        for (int x = 0; x < W; x++) {
            uint32_t c = px[(size_t)(y / scale) * (size_t)w + (size_t)(x / scale)];
            r[1 + 3 * (size_t)x] = (uint8_t)(c >> 16);
            r[2 + 3 * (size_t)x] = (uint8_t)(c >> 8);
            r[3 + 3 * (size_t)x] = (uint8_t)c;
        }
    }
    return encode_rgb_rows(raw, raw_len, W, H, len);
}

int png_write_xrgb8888(const char *path, const uint32_t *px, int w, int h, int scale)
{
    size_t len = 0;
    uint8_t *png = png_encode_xrgb8888(px, w, h, scale, &len);
    return write_file(path, png, len);
}
