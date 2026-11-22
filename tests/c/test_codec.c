/* Packed-data codecs and the ExEn PNG container (src/core/codec.c, docs/re/CODECS.md).
 * The streams here are built by hand from the format description. */
#include <stdlib.h>

#include "codec.h"
#include "support.h"
#include "test.h"

/* MSB-first bit writer, the order every codec reads in */
typedef struct {
    uint8_t b[512];
    uint32_t pos;
} Bits;

static void bits_put(Bits *w, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if ((v >> i) & 1)
            w->b[w->pos >> 3] |= (uint8_t)(0x80u >> (w->pos & 7));
        w->pos++;
    }
}

/* the common 68-bit header: codec nibble, A = row width in bits, B = row count */
static void bits_header(Bits *w, int codec, uint32_t a, uint32_t b)
{
    memset(w, 0, sizeof *w);
    bits_put(w, (uint32_t)codec, 4);
    bits_put(w, a, 32);
    bits_put(w, b, 32);
}

static size_t bits_len(const Bits *w) { return (w->pos + 7) / 8; }

/* Unpack into the middle of a guarded buffer and check nothing outside [0, cap) changed. */
#define GUARD 64
static int unpack_guarded_(int line, const uint8_t *src, size_t len, size_t cap, uint8_t *out,
                           size_t *out_len)
{
    static uint8_t buf[GUARD + 4096 + GUARD];
    if (cap > 4096)
        cap = 4096;
    memset(buf, 0xcd, sizeof buf);
    size_t n = (size_t)-1;
    int rc = exen_unpack(src, len, buf + GUARD, cap, &n);
    for (size_t i = 0; i < GUARD; i++)
        if (buf[i] != 0xcd || buf[GUARD + cap + i] != 0xcd) {
            T_FAIL("line %d: write outside the destination (offset %d)", line,
                   buf[i] != 0xcd ? (int)i - GUARD : (int)(cap + i));
            break;
        }
    t_checks++;
    if (n > cap)
        T_FAIL("line %d: out_len %zu > cap %zu", line, n, cap);
    if (out)
        memcpy(out, buf + GUARD, cap);
    if (out_len)
        *out_len = n;
    return rc;
}
#define unpack_guarded(...) unpack_guarded_(__LINE__, __VA_ARGS__)

static void test_codec5_lz77(void)
{
    /* "ABCABCABCX": three literals, a match (distance 3, length 6), a literal.
     * Control word 0x10000000: item 3 is a match; c = 0 so len = (w >> 14) + 3,
     * dist = (w & 0x3fff) + 1. */
    static const uint8_t s1[] = {
        0x50, 0, 0, 0, 0, 0, 0, 0, 0, /* codec 5; A, B unused */
        10, 0, 0,                     /* unpacked size, 24-bit LE */
        0,
        0x10, 0x00, 0x00, 0x00,
        'A', 'B', 'C', 0xc0, 0x02, 'X',
    };
    uint8_t out[64];
    size_t n = 0, size = 0;
    CHECK_EQ(exen_unpack_method(s1, sizeof s1), 5);
    CHECK_EQ(exen_unpacked_size(s1, sizeof s1, &size), 0);
    CHECK_EQ(size, 10);
    CHECK_EQ(unpack_guarded(s1, sizeof s1, 10, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 10);
    CHECK(memcmp(out, "ABCABCABCX", 10) == 0);
    /* codec 5 ignores the requested size when it is larger ... */
    CHECK_EQ(unpack_guarded(s1, sizeof s1, 40, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 10);
    CHECK(memcmp(out, "ABCABCABCX", 10) == 0);
    /* ... and clamps to dst_cap with an error when it is smaller (the original overruns) */
    CHECK_EQ(unpack_guarded(s1, sizeof s1, 5, out, &n), EXEN_CODEC_ERR_SIZE);
    CHECK_EQ(n, 5);
    CHECK(memcmp(out, "ABCAB", 5) == 0);

    /* c = 1 (low control bits): 13-bit distance, 3-bit length. 'Z' then a distance-1 match
     * of length 10 (an overlapping copy) gives 11 'Z's. */
    static const uint8_t s2[] = {
        0x50, 0, 0, 0, 0, 0, 0, 0, 0, 11, 0, 0, 0,
        0x40, 0x00, 0x00, 0x01,
        'Z', 0xe0, 0x00,
    };
    CHECK_EQ(unpack_guarded(s2, sizeof s2, 11, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 11);
    CHECK(memcmp(out, "ZZZZZZZZZZZ", 11) == 0);

    /* a match longer than what is left (length 6, distance 1) is clamped to the size */
    static const uint8_t s3[] = {
        0x50, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
        0x40, 0x00, 0x00, 0x00,
        'q', 0xc0, 0x00,
    };
    CHECK_EQ(unpack_guarded(s3, sizeof s3, 4, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 4);
    CHECK(memcmp(out, "qqqq", 4) == 0);

    /* more than one control group: 31 literals need a second control word */
    uint8_t s4[13 + 4 + 30 + 4 + 1];
    memset(s4, 0, sizeof s4);
    s4[0] = 0x50;
    s4[9] = 31;
    for (int i = 0; i < 30; i++)
        s4[17 + i] = (uint8_t)('a' + i % 26);
    s4[sizeof s4 - 1] = '!';
    CHECK_EQ(unpack_guarded(s4, sizeof s4, 31, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 31);
    CHECK_EQ(out[29], 'a' + 29 % 26);
    CHECK_EQ(out[30], '!');
}

static void test_codec1_mtf(void)
{
    /* N = 4 cache entries, W = 8-bit symbols: literal 'a', literal 'b', hit [1] ('a', moved to
     * the front), hit [0] ('a') -> "abaa" */
    Bits w;
    bits_header(&w, 1, 32, 1);
    bits_put(&w, 4, 5);
    bits_put(&w, 8, 5);
    bits_put(&w, 0, 1); bits_put(&w, 'a', 8);
    bits_put(&w, 0, 1); bits_put(&w, 'b', 8);
    bits_put(&w, 1, 1); bits_put(&w, 1, 5);
    bits_put(&w, 1, 1); bits_put(&w, 0, 5);
    uint8_t out[16];
    size_t n = 0, size = 0;
    CHECK_EQ(exen_unpacked_size(w.b, bits_len(&w), &size), 0);
    CHECK_EQ(size, 4);
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 4, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 4);
    CHECK(memcmp(out, "abaa", 4) == 0);
    /* a short destination gets the first bytes only */
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 2, out, &n), EXEN_CODEC_OK);
    CHECK(memcmp(out, "ab", 2) == 0);
}

static void test_codec2_rle(void)
{
    /* U = 8: flag 1 + value, or flag 0 = repeat. AA AA 55 55 55 */
    Bits w;
    bits_header(&w, 2, 40, 1);
    bits_put(&w, 8, 4);
    bits_put(&w, 1, 1); bits_put(&w, 0xaa, 8);
    bits_put(&w, 0, 1);
    bits_put(&w, 1, 1); bits_put(&w, 0x55, 8);
    bits_put(&w, 0, 1);
    bits_put(&w, 0, 1);
    uint8_t out[16];
    size_t n = 0;
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 5, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 5);
    static const uint8_t want[] = {0xaa, 0xaa, 0x55, 0x55, 0x55};
    CHECK(memcmp(out, want, 5) == 0);
    /* asking for more than the stream holds: the original returns without decoding */
    memset(out, 0, sizeof out);
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 6, out, &n), EXEN_CODEC_ERR_SIZE);
    CHECK_EQ(out[0], 0xcd); /* untouched guard fill */

    /* U = 4 nibbles: 0x3 x3 then 0xc -> 0x33 0x3c */
    bits_header(&w, 2, 16, 1);
    bits_put(&w, 4, 4);
    bits_put(&w, 1, 1); bits_put(&w, 3, 4);
    bits_put(&w, 0, 1);
    bits_put(&w, 0, 1);
    bits_put(&w, 1, 1); bits_put(&w, 0xc, 4);
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 2, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(out[0], 0x33);
    CHECK_EQ(out[1], 0x3c);

    /* zero-width units would never terminate in the original */
    bits_header(&w, 2, 16, 1);
    bits_put(&w, 0, 4);
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 2, out, &n), EXEN_CODEC_ERR_CORRUPT);
}

static void test_codec3_dictionary(void)
{
    /* U = 8, K = 1: 10 dictionary symbols, then codes 00 -> d[0], 01 -> d[1],
     * 10 xxx -> d[2 + xxx], 11 lit -> literal */
    Bits w;
    bits_header(&w, 3, 40, 1);
    bits_put(&w, 8, 4);
    bits_put(&w, 1, 4);
    for (int i = 0; i < 10; i++)
        bits_put(&w, (uint32_t)(0x10 + i), 8);
    bits_put(&w, 0, 2);                         /* d[0] = 0x10 */
    bits_put(&w, 1, 2);                         /* d[1] = 0x11 */
    bits_put(&w, 2, 2); bits_put(&w, 7, 3);     /* d[9] = 0x19 */
    bits_put(&w, 3, 2); bits_put(&w, 0x99, 8);  /* literal */
    bits_put(&w, 2, 2); bits_put(&w, 0, 3);     /* d[2] = 0x12 */
    uint8_t out[16];
    size_t n = 0;
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 5, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 5);
    static const uint8_t want[] = {0x10, 0x11, 0x19, 0x99, 0x12};
    CHECK(memcmp(out, want, 5) == 0);

    /* U = 4, K = 2, B = 2: each 8-bit symbol holds a nibble of row 0 (pass 1, high unit) and
     * the nibble at the same place in row 1 (pass 2, low unit). Row width 8 bits. */
    bits_header(&w, 3, 8, 2);
    bits_put(&w, 4, 4);
    bits_put(&w, 2, 4);
    for (int i = 0; i < 10; i++)
        bits_put(&w, (uint32_t)(0xa0 + i), 8);
    bits_put(&w, 3, 2); bits_put(&w, 0x12, 8);  /* literal 0x12 */
    bits_put(&w, 0, 2);                         /* d[0] = 0xa0 */
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 2, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(out[0], 0x1a); /* high units: 1, a */
    CHECK_EQ(out[1], 0x20); /* low units:  2, 0 */
}

static void test_codec4_runs(void)
{
    /* L = 4: top bit = colour, low 3 bits = run length. 8 ones, 8 zeros -> ff 00 */
    Bits w;
    bits_header(&w, 4, 16, 1);
    bits_put(&w, 4, 4);
    bits_put(&w, 0x8 | 7, 4); /* 7 ones */
    bits_put(&w, 0x8 | 1, 4); /* 1 one */
    bits_put(&w, 7, 4);       /* 7 zeros */
    bits_put(&w, 1, 4);       /* 1 zero */
    uint8_t out[8];
    size_t n = 0;
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 2, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(out[0], 0xff);
    CHECK_EQ(out[1], 0x00);
    /* L = 0 never terminates in the original */
    bits_header(&w, 4, 16, 1);
    bits_put(&w, 0, 4);
    CHECK_EQ(unpack_guarded(w.b, bits_len(&w), 2, out, &n), EXEN_CODEC_ERR_CORRUPT);
}

static void test_bad_input(void)
{
    uint8_t out[16];
    size_t n = 99, size = 0;
    static const uint8_t c5[] = {0x50, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 0, 0x10};
    CHECK_EQ(exen_unpack(NULL, 4, out, sizeof out, &n), EXEN_CODEC_ERR_ARG);
    CHECK_EQ(n, 0);
    CHECK_EQ(exen_unpack(c5, sizeof c5, NULL, 4, &n), EXEN_CODEC_ERR_ARG);
    CHECK_EQ(exen_unpack(c5, 0, out, sizeof out, &n), EXEN_CODEC_ERR_SHORT);
    CHECK_EQ(exen_unpack(c5, 12, out, sizeof out, &n), EXEN_CODEC_ERR_SHORT); /* codec 5: 13 */
    CHECK_EQ(exen_unpacked_size(c5, 11, &size), EXEN_CODEC_ERR_SHORT);
    static const uint8_t c1[] = {0x10, 0, 0, 0, 0x20, 0, 0, 0};
    CHECK_EQ(exen_unpack(c1, sizeof c1, out, sizeof out, &n), EXEN_CODEC_ERR_SHORT); /* < 9 */
    for (int m = 0; m < 16; m++) {
        uint8_t s[16] = {0};
        s[0] = (uint8_t)(m << 4);
        int want = m >= 1 && m <= 5 ? m : EXEN_CODEC_ERR_METHOD;
        CHECK_EQ(exen_unpack_method(s, sizeof s), want);
        if (want < 0)
            CHECK_EQ(exen_unpack(s, sizeof s, out, sizeof out, &n), EXEN_CODEC_ERR_METHOD);
    }
    /* a truncated codec-5 stream reads zeros past its end: no crash, bounded output */
    CHECK_EQ(unpack_guarded(c5, sizeof c5, 10, out, &n), EXEN_CODEC_OK);
    CHECK_EQ(n, 10);
    /* zero-size destination */
    CHECK(exen_unpack(c5, sizeof c5, out, 0, &n) <= 0);
    CHECK_EQ(n, 0);
}

static void test_image_decode(void)
{
    /* type-0x0d resource: u16 w, h, stream length, PNG signature, chunks without CRCs.
     * 4x2 at 4 bpp, palette of 3, tRNS making entry 2 transparent, IDAT = codec 5 literals. */
    uint8_t r[128];
    size_t p = 0;
    wr16(r + p, 4); wr16(r + p + 2, 2); wr16(r + p + 4, 0); p += 6;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    memcpy(r + p, sig, 8); p += 8;
#define CHUNK(t, len) do { r[p] = 0; r[p + 1] = 0; r[p + 2] = 0; r[p + 3] = (uint8_t)(len); \
                           memcpy(r + p + 4, t, 4); p += 8; } while (0)
    CHUNK("IHDR", 13);
    static const uint8_t ihdr[13] = {0, 0, 0, 4, 0, 0, 0, 2, 4, 3, 1, 0, 0};
    memcpy(r + p, ihdr, 13); p += 13;
    CHUNK("PLTE", 9);
    static const uint8_t plte[9] = {0, 0, 0, 255, 0, 0, 0, 255, 0};
    memcpy(r + p, plte, 9); p += 9;
    CHUNK("tRNS", 3);
    r[p++] = 255; r[p++] = 255; r[p++] = 0;
    static const uint8_t idat[] = {0x50, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
                                   0, 0, 0, 0, 0x01, 0x20, 0x12, 0x21};
    CHUNK("IDAT", sizeof idat);
    memcpy(r + p, idat, sizeof idat); p += sizeof idat;
    wr16(r + 4, (uint16_t)(p - 6));
#undef CHUNK

    ExenDecodedImage img;
    REQUIRE(exen_image_decode(r, p, &img) == EXEN_CODEC_OK);
    CHECK_EQ(img.width, 4);
    CHECK_EQ(img.height, 2);
    CHECK_EQ(img.res_width, 4);
    CHECK_EQ(img.res_stream_len, p - 6);
    CHECK_EQ(img.bit_depth, 4);
    CHECK_EQ(img.color_type, 3);
    CHECK_EQ(img.codec, 5);
    CHECK_EQ(img.pitch, 2);
    CHECK_EQ(img.raw_len, 4);
    static const uint8_t px[8] = {0, 1, 2, 0, 1, 2, 2, 1};
    CHECK(memcmp(img.pixels, px, 8) == 0);
    CHECK_EQ(img.palette_count, 3);
    CHECK_EQ(img.palette[3], 255);
    CHECK(img.has_trns);
    CHECK_EQ(img.alpha_count, 3);
    CHECK_EQ(img.alpha[2], 0);
    CHECK_EQ(img.alpha[3], 255);
    CHECK_EQ(img.transparent_index, 2);
    CHECK_EQ(img.transparent_zero_count, 1);
    exen_image_free(&img);
    CHECK(img.pixels == NULL);

    /* truncating the resource anywhere must fail cleanly (or decode a prefix) */
    for (size_t cut = 0; cut < p; cut++) {
        ExenDecodedImage t;
        int rc = exen_image_decode(r, cut, &t);
        if (rc == EXEN_CODEC_OK)
            exen_image_free(&t);
        CHECK(rc <= 0);
    }

    /* no PNG signature: a raw 1-bit bitmap after the header (decodeBitmap 0x418d0a) */
    static const uint8_t raw[6 + 2] = {8, 0, 2, 0, 2, 0, 0x81, 0x7e};
    REQUIRE(exen_image_decode(raw, sizeof raw, &img) == EXEN_CODEC_OK);
    CHECK_EQ(img.codec, 0);
    CHECK_EQ(img.width, 8);
    CHECK_EQ(img.pixels[0], 1);
    CHECK_EQ(img.pixels[1], 0);
    CHECK_EQ(img.pixels[8], 0);
    CHECK_EQ(img.pixels[9], 1);
    exen_image_free(&img);
    CHECK(exen_image_decode(raw, 7, &img) == EXEN_CODEC_ERR_SHORT);
    CHECK(exen_image_decode(raw, 3, &img) == EXEN_CODEC_ERR_FORMAT);
    CHECK(exen_image_decode(NULL, 3, &img) == EXEN_CODEC_ERR_ARG);
}

static uint32_t rng_state = 0x2545f491u;
static uint32_t rnd(void)
{
    /* xorshift32: fixed seed, same sequence everywhere */
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static void test_fuzz(void)
{
    /* random buffers, half with a valid codec nibble, small and odd destination sizes */
    static uint8_t src[256];
    int rcs[16] = {0};
    for (int iter = 0; iter < 6000; iter++) {
        size_t len = rnd() % sizeof src;
        for (size_t i = 0; i < len; i++)
            src[i] = (uint8_t)rnd();
        if (len && (iter & 1))
            src[0] = (uint8_t)((1 + rnd() % 5) << 4 | (src[0] & 15));
        if (len > 8 && (iter & 2)) {
            /* keep A * B small so the stream is not mostly zero padding */
            src[1] = src[2] = src[3] = 0;
            src[4] &= 0x0f;
            src[5] = src[6] = 0;
            src[7] &= 0x0f;
        }
        if (len > 11 && src[0] >> 4 == 5) {
            /* Codec 5 runs to its declared size (bytes 9..11) even when the destination is
             * smaller; keep it below 4 KiB so the loop stays fast. */
            src[10] &= 0x0f;
            src[11] = 0;
        }
        size_t cap = rnd() % 1500;
        size_t n = 0;
        int rc = unpack_guarded(src, len, cap, NULL, &n);
        CHECK(rc <= 0 && rc >= EXEN_CODEC_ERR_UNSUPPORTED);
        rcs[-rc & 15]++;
        if (t_failures > 20)
            return;
    }
    /* also through the image container, with a PNG signature planted half the time */
    for (int iter = 0; iter < 3000; iter++) {
        size_t len = rnd() % sizeof src;
        for (size_t i = 0; i < len; i++)
            src[i] = (uint8_t)rnd();
        if (len > 40 && (iter & 1)) {
            static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
            memcpy(src + 6, sig, 8);
            /* an IHDR chunk with a small size, then an IDAT */
            static const uint8_t ihdr[8 + 13] = {0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                                 0, 0, 0, 9, 0, 0, 0, 7, 4, 3, 1, 0, 0};
            memcpy(src + 14, ihdr, sizeof ihdr);
            memcpy(src + 35, "\0\0\0\xc8IDAT", 8);
            if (iter & 2)
                src[22 + 3] = (uint8_t)rnd(); /* random width (IHDR data starts at 22) */
            if (len > 43 + 12 && src[43] >> 4 == 5) {
                src[43 + 10] &= 0x0f; /* as above: bound codec 5's declared size */
                src[43 + 11] = 0;
            }
        }
        ExenDecodedImage img;
        int rc = exen_image_decode(src, len, &img);
        CHECK(rc <= 0);
        if (rc == EXEN_CODEC_OK) {
            CHECK(img.pixels != NULL);
            exen_image_free(&img);
        }
        if (t_failures > 20)
            return;
    }
    printf("  fuzz return codes: ok=%d arg=%d method=%d short=%d size=%d corrupt=%d\n", rcs[0],
           rcs[1], rcs[2], rcs[3], rcs[4], rcs[5]);
}

int main(void)
{
    RUN(test_codec5_lz77);
    RUN(test_codec1_mtf);
    RUN(test_codec2_rle);
    RUN(test_codec3_dictionary);
    RUN(test_codec4_runs);
    RUN(test_bad_input);
    RUN(test_image_decode);
    RUN(test_fuzz);
    TEST_MAIN_END();
}
