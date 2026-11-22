/* imgdump: decode every image (resource type 0x0d) in ExEn .exn files and write it as a PNG.
 *
 *   imgdump [-q] <outdir> <game.exn>...
 *
 * Output files are <outdir>/<game>_<resource index>.png: 8-bit indexed PNGs with the image's
 * PLTE and tRNS. Each image is checked against the resource header (w, h) and against the
 * unpacked size its packed header declares. Type-0x0d resources without a PNG signature are raw
 * 1-bit bitmaps (as the simulator reads them) and are written too. Type-0x81 resources are
 * packed 1-bit bitmaps (`u16 w, u16 h`, then the packed stream); they are unpacked, checked
 * against ((w + 7) / 8) * h and written as <game>_<index>_81.png. A per-codec summary is
 * printed at the end; the exit status is 1 if anything failed (empty placeholder resources,
 * which the game can never load, are reported as failures too).
 *
 * Build (standalone, C11, standard library only):
 *   cmake -S tools/imgdump -B build/imgdump -G Ninja && cmake --build build/imgdump
 * or
 *   cc -std=c11 -O2 -Isrc/core tools/imgdump/imgdump.c src/core/codec.c -o imgdump
 *
 * The PNG writer is self-contained: zlib stream of stored (uncompressed) deflate blocks, with
 * CRC-32 and Adler-32 computed here. No zlib dependency.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0777)
#endif

/* ---- PNG writer (stored deflate) -------------------------------------------------------- */

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc_update(0xffffffffu, hdr + 4, 4);
    crc = crc_update(crc, data, len) ^ 0xffffffffu;
    uint8_t tail[4];
    put_be32(tail, crc);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4, f) == 4;
}

/* zlib stream of stored blocks for `raw` (rows already prefixed with filter byte 0). */
static uint8_t *zlib_stored(const uint8_t *raw, size_t n, size_t *out_len)
{
    size_t blocks = n / 65535 + 1;
    uint8_t *z = malloc(2 + blocks * 5 + n + 4);
    if (!z)
        return NULL;
    size_t o = 0;
    z[o++] = 0x78; /* CM 8, CINFO 7 */
    z[o++] = 0x01; /* FLEVEL 0, FCHECK so that 0x7801 % 31 == 0 */
    size_t pos = 0;
    do {
        size_t len = n - pos > 65535 ? 65535 : n - pos;
        int final = pos + len == n;
        z[o++] = (uint8_t)final; /* BFINAL, BTYPE 00 */
        z[o++] = (uint8_t)len;
        z[o++] = (uint8_t)(len >> 8);
        z[o++] = (uint8_t)~len;
        z[o++] = (uint8_t)(~len >> 8);
        memcpy(z + o, raw + pos, len);
        o += len;
        pos += len;
    } while (pos < n);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(z + o, b << 16 | a);
    o += 4;
    *out_len = o;
    return z;
}

static int write_png(const char *path, const ExenDecodedImage *img)
{
    uint32_t w = img->width, h = img->height;
    size_t rawn = (size_t)(w + 1) * h;
    uint8_t *raw = malloc(rawn);
    if (!raw)
        return -1;
    unsigned maxidx = 0;
    for (uint32_t y = 0; y < h; y++) {
        raw[(size_t)y * (w + 1)] = 0; /* filter: none */
        memcpy(raw + (size_t)y * (w + 1) + 1, img->pixels + (size_t)y * w, w);
        for (uint32_t x = 0; x < w; x++)
            if (img->pixels[(size_t)y * w + x] > maxidx)
                maxidx = img->pixels[(size_t)y * w + x];
    }
    size_t zn;
    uint8_t *z = zlib_stored(raw, rawn, &zn);
    free(raw);
    if (!z)
        return -1;

    /* PNG needs a palette entry for every index used; pad with black if the image has fewer. */
    unsigned npal = img->palette_count;
    if (npal < maxidx + 1)
        npal = maxidx + 1;
    uint8_t pal[256 * 3];
    memset(pal, 0, sizeof pal);
    memcpy(pal, img->palette, (size_t)img->palette_count * 3);

    uint8_t ihdr[13];
    put_be32(ihdr, w);
    put_be32(ihdr + 4, h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 3;  /* indexed */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return -1;
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    int ok = fwrite(sig, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) &&
             write_chunk(f, "PLTE", pal, (size_t)npal * 3);
    if (ok && img->has_trns && img->alpha_count > 0) {
        unsigned na = img->alpha_count < npal ? img->alpha_count : npal;
        ok = write_chunk(f, "tRNS", img->alpha, na);
    }
    ok = ok && write_chunk(f, "IDAT", z, zn) && write_chunk(f, "IEND", NULL, 0);
    free(z);
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}

/* ---- .exn container ------------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char *base_name(const char *path, char *buf, size_t cap)
{
    const char *s = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            s = p + 1;
    size_t n = strlen(s);
    if (n > 4 && (strcmp(s + n - 4, ".exn") == 0 || strcmp(s + n - 4, ".EXN") == 0))
        n -= 4;
    if (n >= cap)
        n = cap - 1;
    memcpy(buf, s, n);
    buf[n] = 0;
    return buf;
}

/* ---- main ---------------------------------------------------------------------------------- */

typedef struct {
    unsigned ok, fail;
} Count;

static Count img_by_codec[6], packed_by_codec[6], img_raw;
static unsigned img_other_fail, packed_other_fail, games, game_errors;

static const char *errname(int rc)
{
    switch (rc) {
    case EXEN_CODEC_OK: return "ok";
    case EXEN_CODEC_ERR_ARG: return "bad argument";
    case EXEN_CODEC_ERR_METHOD: return "unknown codec";
    case EXEN_CODEC_ERR_SHORT: return "data too short";
    case EXEN_CODEC_ERR_SIZE: return "size mismatch";
    case EXEN_CODEC_ERR_CORRUPT: return "corrupt stream";
    case EXEN_CODEC_ERR_NOMEM: return "out of memory";
    case EXEN_CODEC_ERR_FORMAT: return "not an image (empty or zero-sized)";
    case EXEN_CODEC_ERR_UNSUPPORTED: return "unsupported colour type";
    default: return "?";
    }
}

/* Find the IDAT payload of an image resource, for the size cross-check. */
static const uint8_t *find_idat(const uint8_t *res, size_t len, size_t *idat_len)
{
    if (len < 14)
        return NULL;
    size_t p = 6 + 8;
    while (p + 8 <= len) {
        uint32_t clen = (uint32_t)res[p] << 24 | (uint32_t)res[p + 1] << 16 |
                        (uint32_t)res[p + 2] << 8 | res[p + 3];
        if (memcmp(res + p + 4, "IDAT", 4) == 0) {
            *idat_len = clen <= len - p - 8 ? clen : len - p - 8;
            return res + p + 8;
        }
        p += 8 + (size_t)clen;
    }
    return NULL;
}

static void do_image(const char *outdir, const char *game, unsigned idx, const uint8_t *res,
                     size_t len, int quiet)
{
    ExenDecodedImage img;
    int rc = exen_image_decode(res, len, &img);
    int codec = rc == EXEN_CODEC_OK ? img.codec : -1;
    char why[1200] = "";
    if (rc != EXEN_CODEC_OK) {
        snprintf(why, sizeof why, "decode: %s (%d)", errname(rc), rc);
    } else if (img.width != img.res_width || img.height != img.res_height) {
        snprintf(why, sizeof why, "IHDR %ux%u != resource header %ux%u", (unsigned)img.width,
                 (unsigned)img.height, img.res_width, img.res_height);
    } else if (codec != 0) {
        size_t il = 0, declared = 0;
        const uint8_t *idat = find_idat(res, len, &il);
        if (!idat || exen_unpacked_size(idat, il, &declared) != EXEN_CODEC_OK)
            snprintf(why, sizeof why, "no packed size");
        else if (declared != img.raw_len)
            snprintf(why, sizeof why, "packed size %zu != pitch*h %zu", declared, img.raw_len);
    }
    if (!why[0]) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s_%03u.png", outdir, game, idx);
        if (write_png(path, &img) != 0)
            snprintf(why, sizeof why, "cannot write %s", path);
    }
    if (rc == EXEN_CODEC_OK)
        exen_image_free(&img);
    Count *c = codec >= 1 && codec <= 5 ? &img_by_codec[codec] : codec == 0 ? &img_raw : NULL;
    if (why[0]) {
        if (c)
            c->fail++;
        else
            img_other_fail++;
        printf("FAIL %s res %u (codec %d, %zu bytes): %s\n", game, idx, codec, len, why);
    } else {
        c->ok++;
        if (!quiet || codec == 0)
            printf("ok   %s res %u codec %d %ux%u depth %u pal %u%s\n", game, idx, codec,
                   (unsigned)img.width, (unsigned)img.height, img.bit_depth, img.palette_count,
                   img.has_trns ? " tRNS" : "");
    }
}

static void do_packed(const char *outdir, const char *game, unsigned idx, const uint8_t *res,
                      size_t len, int quiet)
{
    /* Resource.readImage on type 0x81: u16 w, u16 h, then readBytes(((w + 7) >> 3) * h), which
     * unpacks the rest of the resource. */
    const uint8_t *src = res + (len >= 4 ? 4 : len);
    size_t n = len >= 4 ? len - 4 : 0;
    unsigned w = len >= 4 ? (unsigned)(res[0] | res[1] << 8) : 0;
    unsigned h = len >= 4 ? (unsigned)(res[2] | res[3] << 8) : 0;
    size_t want = (size_t)((w + 7) >> 3) * h;
    int m = exen_unpack_method(src, n);
    size_t size = 0, got = 0;
    int rc = exen_unpacked_size(src, n, &size);
    uint8_t *buf = NULL;
    if (rc == EXEN_CODEC_OK && size != want)
        rc = EXEN_CODEC_ERR_SIZE;
    if (rc == EXEN_CODEC_OK) {
        buf = malloc(size ? size : 1);
        rc = buf ? exen_unpack(src, n, buf, size, &got) : EXEN_CODEC_ERR_NOMEM;
        if (rc == EXEN_CODEC_OK && got != size)
            rc = EXEN_CODEC_ERR_SIZE;
    }
    if (rc == EXEN_CODEC_OK) {
        ExenDecodedImage img;
        rc = exen_image_from_1bpp(buf, size, w, h, &img);
        if (rc == EXEN_CODEC_OK) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s_%03u_81.png", outdir, game, idx);
            if (write_png(path, &img) != 0)
                rc = EXEN_CODEC_ERR_ARG;
            exen_image_free(&img);
        }
    }
    free(buf);
    Count *c = m >= 1 && m <= 5 ? &packed_by_codec[m] : NULL;
    if (rc != EXEN_CODEC_OK) {
        if (c)
            c->fail++;
        else
            packed_other_fail++;
        printf("FAIL %s res %u (type 0x81, codec %d): %s\n", game, idx, m, errname(rc));
    } else {
        c->ok++;
        if (!quiet)
            printf("ok   %s res %u type 0x81 codec %d %ux%u 1bpp -> %zu bytes\n", game, idx, m,
                   w, h, size);
    }
}

static void do_game(const char *outdir, const char *path, int quiet)
{
    size_t len;
    uint8_t *b = read_file(path, &len);
    char game[256];
    base_name(path, game, sizeof game);
    games++;
    if (!b || len < 0x38 || memcmp(b, "NEXE", 4) != 0) {
        printf("FAIL %s: not a NEXE file\n", path);
        game_errors++;
        free(b);
        return;
    }
    uint32_t n = le32(b + 0x34);
    size_t types = 0x38 + 4 * ((size_t)n + 1);
    if (n > 65535 || types + n > len) {
        printf("FAIL %s: bad resource table\n", path);
        game_errors++;
        free(b);
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = le32(b + 0x38 + 4 * i), e = le32(b + 0x38 + 4 * (i + 1));
        if (a > e || e > len)
            continue;
        uint8_t t = b[types + i];
        if (t == 0x0d)
            do_image(outdir, game, i, b + a, e - a, quiet);
        else if (t == 0x81)
            do_packed(outdir, game, i, b + a, e - a, quiet);
    }
    free(b);
}

int main(int argc, char **argv)
{
    int quiet = 0, a = 1;
    if (a < argc && strcmp(argv[a], "-q") == 0) {
        quiet = 1;
        a++;
    }
    if (argc - a < 2) {
        fprintf(stderr, "usage: imgdump [-q] <outdir> <game.exn>...\n");
        return 2;
    }
    crc_init();
    const char *outdir = argv[a++];
    make_dir(outdir); /* ok if it already exists */
    for (; a < argc; a++)
        do_game(outdir, argv[a], quiet);

    unsigned fails = img_other_fail + packed_other_fail + game_errors + img_raw.fail;
    printf("\n%u files\nimages (type 0x0d):\n", games);
    for (int c = 1; c <= 5; c++) {
        printf("  codec %d:  %5u ok, %u failed\n", c, img_by_codec[c].ok, img_by_codec[c].fail);
        fails += img_by_codec[c].fail;
    }
    printf("  raw 1bpp: %5u ok, %u failed\n", img_raw.ok, img_raw.fail);
    if (img_other_fail)
        printf("  not decodable: %u\n", img_other_fail);
    printf("packed 1bpp bitmaps (type 0x81):\n");
    for (int c = 1; c <= 5; c++) {
        if (packed_by_codec[c].ok || packed_by_codec[c].fail)
            printf("  codec %d:  %5u ok, %u failed\n", c, packed_by_codec[c].ok,
                   packed_by_codec[c].fail);
        fails += packed_by_codec[c].fail;
    }
    if (packed_other_fail)
        printf("  unknown codec: %u failed\n", packed_other_fail);
    return fails ? 1 : 0;
}
