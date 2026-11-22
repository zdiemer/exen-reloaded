/* In-Fusio packed-data codecs and the "ExEn PNG" container. See codec.h and docs/re/CODECS.md.
 *
 * Every codec here is a translation of the routine in gensimu.exe at the address in its comment.
 * The simulator's decoders are resumable (they keep a "pending bits" state for a second decode
 * call) and decode in place (the packed data sits at the tail of the output buffer, with a 0x400
 * byte stash for when the writer catches up with the reader). Both call sites, the PNG decoder
 * and Resource.readBytes, call decode exactly once on a fresh decoder into a buffer of their own,
 * so the resume state never matters and the stash never changes the result. Both are left out.
 *
 * The bit reader and writer are kept bit-exact, including their quirks for fields that straddle
 * more than 32 bits and value bits that spill to the left of the written field. Bytes past the
 * end of the input read as 0; writes past dst_cap are dropped.
 */
#include <stdlib.h>
#include <string.h>

#include "codec.h"

/* ---- x86 shift semantics (count taken mod 32) ---------------------------------------------- */

static uint32_t shl(uint32_t v, uint32_t n) { return v << (n & 31); }
static uint32_t shr(uint32_t v, uint32_t n) { return v >> (n & 31); }

/* ---- bit I/O ---------------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t len;
} BitIn;

typedef struct {
    uint8_t *p;
    size_t cap;
    uint32_t pos;   /* bit position */
    uint32_t hi;    /* highest bit position reached, for out_len */
} BitOut;

static uint32_t in_u8(const BitIn *in, size_t i) { return i < in->len ? in->p[i] : 0; }

static uint32_t in_be32(const BitIn *in, size_t i)
{
    return in_u8(in, i) << 24 | in_u8(in, i + 1) << 16 | in_u8(in, i + 2) << 8 | in_u8(in, i + 3);
}

/* getBits 0x412ea0 (also inlined in every codec): n bits MSB first at *pos.
 * Fields with (pos & 7) + n > 32 come out wrong in the original (the second word's bits are
 * masked away and the first part is shifted left by 2); that is kept. No stream uses them. */
static uint32_t get_bits(const BitIn *in, uint32_t *pos, uint32_t n)
{
    uint32_t off = *pos & 7;
    size_t i = *pos >> 3;
    uint32_t mask = shl(1, n) - 1;
    *pos += n;
    if (n + off <= 8)
        return shr(in_u8(in, i), 8 - off - n) & mask;
    if (n + off <= 16)
        return shr(in_u8(in, i) << 8 | in_u8(in, i + 1), 16 - off - n) & mask;
    if (n + off <= 32)
        return shr(in_be32(in, i), 32 - off - n) & mask;
    uint32_t k = off + n - 32;
    uint32_t v = shl(in_be32(in, i), k) & mask;
    uint32_t w = shr(in_be32(in, i + 4), 32 - k) & ~(shl(1, k) - 1);
    return v << 2 | w;
}

static uint32_t out_u8(const BitOut *o, size_t i) { return i < o->cap ? o->p[i] : 0; }

static void out_set(BitOut *o, size_t i, uint32_t v)
{
    if (i < o->cap)
        o->p[i] = (uint8_t)v;
}

static uint32_t out_be32(const BitOut *o, size_t i)
{
    return out_u8(o, i) << 24 | out_u8(o, i + 1) << 16 | out_u8(o, i + 2) << 8 | out_u8(o, i + 3);
}

static void out_set_be32(BitOut *o, size_t i, uint32_t v)
{
    out_set(o, i, v >> 24);
    out_set(o, i + 1, v >> 16);
    out_set(o, i + 2, v >> 8);
    out_set(o, i + 3, v);
}

/* putBits 0x413071 (also inlined): write n bits MSB first at o->pos through a 1, 2 or 4 byte
 * window. The value is not masked, so bits above n spill into the field's left neighbours
 * inside the window (codec 4 relies on this, writing 0xff for short runs). */
static void put_bits(BitOut *o, uint32_t value, uint32_t n)
{
    uint32_t off = o->pos & 7;
    size_t i = o->pos >> 3;
    uint32_t mask = shl(1, n) - 1;
    if (n + off <= 8) {
        uint32_t s = 8 - off - n;
        out_set(o, i, (out_u8(o, i) & ~shl(mask, s)) | shl(value, s));
    } else if (n + off <= 16) {
        uint32_t s = 16 - off - n;
        uint32_t v = out_u8(o, i) << 8 | out_u8(o, i + 1);
        v = (v & ~shl(mask, s)) | shl(value, s);
        out_set(o, i + 1, v);
        out_set(o, i, v >> 8);
    } else if (n + off <= 32) {
        uint32_t s = 32 - off - n;
        out_set_be32(o, i, (out_be32(o, i) & ~shl(mask, s)) | shl(value, s));
    } else {
        uint32_t k = off + n - 32;
        out_set_be32(o, i, (out_be32(o, i) & ~shr(mask, k)) | shr(value, k));
        uint32_t m2 = shl(1, k) - 1;
        out_set_be32(o, i + 4, (out_be32(o, i + 4) & ~shl(m2, 32 - k)) | shl(value, 32 - k));
    }
    o->pos += n;
    if (o->pos > o->hi)
        o->hi = o->pos;
}

/* Past its end a stream reads as zeros. For codec 4 that means zero-length runs forever (the
 * simulator would spin, reading whatever memory follows); stop once well past the end. The other
 * codecs' loops are bounded by the header size, so they read zeros and finish as the original
 * would with zeroed memory after the input. */
static int overread(const BitIn *in, uint32_t pos)
{
    return (uint64_t)pos > (uint64_t)in->len * 8 + 64 * 8;
}

/* ---- header (0x432d20, 0x432e03) ------------------------------------------------------------ */

static void header_ab(const BitIn *in, uint32_t *a, uint32_t *b)
{
    *a = (in_u8(in, 0) & 15) << 28 | in_u8(in, 1) << 20 | in_u8(in, 2) << 12 | in_u8(in, 3) << 4 |
         in_u8(in, 4) >> 4;
    *b = (in_u8(in, 4) & 15) << 28 | in_u8(in, 5) << 20 | in_u8(in, 6) << 12 | in_u8(in, 7) << 4 |
         in_u8(in, 8) >> 4;
}

/* Total stream size in bits: ((A + 7) & ~7) * B, 32-bit arithmetic as in the original. */
static uint32_t header_bits(const BitIn *in)
{
    uint32_t a, b;
    header_ab(in, &a, &b);
    return ((a + 7) & ~7u) * b;
}

#define HEADER_BITS 68u /* nibble + A + B */

/* ---- codec 1: move-to-front cache (create 0x431750, decode 0x4318f6) ------------------------ */
/* Header: 5 bits N (cache size), 5 bits W (symbol width). Then, per symbol, while W bits of the
 * stream remain:
 *   1, idx:5   -> symbol = cache[idx]
 *   0, lit:W   -> symbol = lit, and idx = N - 1 (the oldest entry falls out)
 * cache[1..idx] = cache[0..idx-1]; cache[0] = symbol; emit the symbol in W bits.
 * Bits left over (fewer than W) are copied verbatim. */
static int codec1(const BitIn *in, BitOut *o, uint32_t out_bits)
{
    uint32_t pos = HEADER_BITS;
    uint32_t remain = header_bits(in);
    uint16_t n = (uint16_t)get_bits(in, &pos, 5);
    uint16_t w = (uint16_t)get_bits(in, &pos, 5);
    uint16_t cache[256];    /* the original mallocs n entries; idx is 5 bits, and n - 1 wraps
                               to 255 when n is 0 */
    memset(cache, 0, sizeof cache);

    if (w == 0)
        return EXEN_CODEC_ERR_CORRUPT; /* zero-width symbols: the original never terminates */
    while (remain >= w) {
        uint32_t sym;
        uint8_t idx;
        if ((uint8_t)get_bits(in, &pos, 1) == 1) {
            idx = (uint8_t)get_bits(in, &pos, 5);
            sym = cache[idx];
        } else {
            /* read as 8-bit chunks and a remainder, which is the same as get_bits(w) */
            uint32_t k = w;
            sym = 0;
            while (k >= 8) {
                k -= 8;
                sym = sym << 8 | (uint8_t)get_bits(in, &pos, 8);
            }
            if (k > 0)
                sym = shl(sym, k) | (uint8_t)get_bits(in, &pos, k);
            idx = (uint8_t)(n - 1);
        }
        for (; idx > 0; idx--)
            cache[idx] = cache[idx - 1];
        cache[0] = (uint16_t)sym;
        remain -= w;
        if (out_bits < w) {
            /* output full mid-symbol: write its top bits, keep the rest for a next call */
            if (out_bits > 0)
                put_bits(o, shr(sym, w - out_bits), out_bits);
            return EXEN_CODEC_OK;
        }
        put_bits(o, sym, w);
        out_bits -= w;
    }
    while (remain >= 8 && out_bits >= 8) {
        put_bits(o, (uint16_t)get_bits(in, &pos, 8), 8);
        remain -= 8;
        out_bits -= 8;
    }
    if (remain > 0 && out_bits > 0)
        put_bits(o, get_bits(in, &pos, remain), out_bits);
    return EXEN_CODEC_OK;
}

/* ---- codec 2: repeat-previous RLE (create 0x42f560 / 0x42f646, decode 0x42f724) ------------- */
/* Per unit of U bits: flag 1 -> read a new U-bit value; flag 0 -> repeat the previous value
 * (initially 0). Trailing bits (fewer than U) are copied verbatim. The standard create reads
 * U from 4 bits after the header; codec 3 also starts this decoder mid-stream (0x42f646). */
static int codec2_run(const BitIn *in, uint32_t pos, uint32_t unit, uint32_t total, BitOut *o,
                      uint32_t out_bits)
{
    if (out_bits > total)
        return EXEN_CODEC_ERR_SIZE; /* the original returns without decoding */
    uint16_t prev = 0;
    uint32_t remain = total;
    if (unit == 0)
        return EXEN_CODEC_ERR_CORRUPT; /* zero-width units: the original never terminates */
    while (remain >= unit) {
        if ((uint8_t)get_bits(in, &pos, 1) == 1)
            prev = (uint16_t)get_bits(in, &pos, unit);
        remain -= unit;
        if (out_bits < unit) {
            if (out_bits > 0)
                put_bits(o, shr(prev, unit - out_bits), out_bits);
            return EXEN_CODEC_OK;
        }
        put_bits(o, prev, unit);
        out_bits -= unit;
    }
    while (remain >= 8 && out_bits >= 8) {
        put_bits(o, (uint16_t)get_bits(in, &pos, 8), 8);
        remain -= 8;
        out_bits -= 8;
    }
    if (remain > 0 && out_bits > 0)
        put_bits(o, (uint16_t)get_bits(in, &pos, remain), remain);
    return EXEN_CODEC_OK;
}

static int codec2(const BitIn *in, BitOut *o, uint32_t out_bits)
{
    uint32_t pos = HEADER_BITS;
    uint8_t unit = (uint8_t)get_bits(in, &pos, 4);
    return codec2_run(in, pos, unit, header_bits(in), o, out_bits);
}

/* ---- codec 3: 10-entry dictionary, multi-pass rows (create 0x42d500, decode 0x42d960) ------- */
/* Header: 4 bits U (unit width), 4 bits K (units per symbol), then 10 dictionary symbols of
 * U*K bits each. Symbol codes: 00 -> dict[0], 01 -> dict[1], 10 xxx -> dict[2 + xxx],
 * 11 lit:U*K -> literal. The output is cut into rows of R = (A + 7) & ~7 bits. Each row is
 * decoded K times from the same position: pass p (1..K) emits unit p of every symbol, most
 * significant first, so a K = 2 stream stores two output rows (row 2i holding the high units,
 * row 2i+1 the low units) per encoded row. With an odd row count and K = 2 the last row is
 * coded with codec 2 (U = 8) instead. */
static int codec3(const BitIn *in, BitOut *o, uint32_t out_bits)
{
    uint32_t pos = HEADER_BITS;
    uint32_t a, b;
    header_ab(in, &a, &b);
    uint32_t unit_bits = get_bits(in, &pos, 4);     /* S+0x28 */
    uint32_t units = get_bits(in, &pos, 4);         /* S+0x2c */
    uint32_t sym_bits = unit_bits * units;          /* S+0x14 */
    uint32_t total = header_bits(in);               /* S+0x0c */
    uint32_t row_bits = ((a + 7) >> 3) << 3;        /* S+0x30 */
    uint32_t remain = row_bits * b;                 /* S+0x18 */
    if ((b & 1) && units == 2)
        remain -= row_bits;
    uint32_t tail_bits = total - remain;            /* S+0x0c after create */

    uint16_t dict[10];
    for (int i = 0; i < 10; i++) {
        uint32_t k = sym_bits;
        uint16_t v = 0;
        while (k >= 8) {
            k -= 8;
            v = (uint16_t)(v << 8);
            v |= (uint8_t)get_bits(in, &pos, 8);
        }
        if (k > 0) {
            v = (uint16_t)shl(v, k);
            v |= (uint8_t)get_bits(in, &pos, k);
        }
        dict[i] = v;
    }

    uint32_t row_start = pos;       /* S+0x24 */
    uint32_t row_left = row_bits;   /* S+0x34 */
    uint32_t pass = 1;              /* S+0x38 */
    uint8_t lit_bits = (uint8_t)sym_bits;

    if (unit_bits == 0 && remain > 0)
        return EXEN_CODEC_ERR_CORRUPT; /* zero-width units: the original never terminates */
    while (remain > 0) {
        uint32_t u = unit_bits;
        uint32_t val;
        if (row_left >= u) {
            uint32_t sym;
            uint32_t code = get_bits(in, &pos, 2);
            if (code <= 1)
                sym = dict[code];
            else if (code == 2)
                sym = dict[2 + get_bits(in, &pos, 3)];
            else
                sym = get_bits(in, &pos, lit_bits);
            remain -= u;
            row_left -= u;
            val = shr(sym, u * (units - pass)) & (shl(1, u) - 1);
        } else {
            /* row ends inside a unit: a short symbol of row_left * K bits */
            uint32_t sym = get_bits(in, &pos, row_left * units);
            val = shr(sym, row_left * (units - pass)) & (shl(1, row_left) - 1);
            remain -= row_left;
            u = row_left;
            row_left = 0;
        }
        if (out_bits >= u) {
            put_bits(o, val, u);
            out_bits -= u;
        } else if (out_bits > 0) {
            /* Output full mid-unit: write the unit's top bits. The original does not reduce
             * out_bits here, so it goes on decoding until the rows run out, writing past the
             * end of its buffer (o->pos + out_bits == dst_cap * 8 always holds, so every later
             * write lands beyond dst_cap). Nothing inside the buffer changes any more: stop. */
            put_bits(o, shr(val, u - out_bits), out_bits);
            return EXEN_CODEC_OK;
        }
        if (row_left == 0) {
            row_left = row_bits;
            if (++pass > units) {
                pass = 1;
                row_start = pos;
            } else {
                pos = row_start;
            }
        }
        if (out_bits == 0)
            return EXEN_CODEC_OK;
    }
    if (b > 1 && out_bits > 0) {
        /* 0x42f447: hand the rest to codec 2, starting at the current position */
        BitOut sub = { o->p + (o->pos >> 3), o->pos >> 3 < o->cap ? o->cap - (o->pos >> 3) : 0,
                       0, 0 };
        int rc = codec2_run(in, pos, 8, tail_bits, &sub, (out_bits >> 3) << 3);
        if (o->pos / 8 * 8 + sub.hi > o->hi)
            o->hi = o->pos / 8 * 8 + sub.hi;
        return rc;
    }
    return EXEN_CODEC_OK;
}

/* ---- codec 4: bit-run RLE (create 0x432180, decode 0x43228b) -------------------------------- */
/* Header: 4 bits L. Then runs of L bits: the top bit is the colour (1 -> ones, 0 -> zeros) and
 * the low L-1 bits the run length in bits. Runs shorter than 8 bits are written as the value
 * 0xff (or 0), so a run of ones also sets the bits left of it in its byte window (putBits
 * does not mask the value). Used for 1-bit masks in type-0x81 resources. */
static int codec4(const BitIn *in, BitOut *o, uint32_t out_bits)
{
    uint32_t pos = HEADER_BITS;
    uint8_t l = (uint8_t)get_bits(in, &pos, 4);
    uint32_t remain = header_bits(in);
    if (out_bits > remain)
        return EXEN_CODEC_ERR_SIZE;
    if (l == 0)
        return EXEN_CODEC_ERR_CORRUPT; /* zero-width runs: the original never terminates */
    uint16_t lenmask = (uint16_t)(shl(1, (uint32_t)l - 1) - 1);
    while (remain > 0 && out_bits > 0) {
        if (overread(in, pos))
            return EXEN_CODEC_ERR_CORRUPT;
        uint16_t fill = 0;
        uint16_t run = (uint16_t)(get_bits(in, &pos, l) & 0xff);
        if (run > lenmask) {
            fill = 0xff;
            run &= lenmask;
        }
        remain -= run;
        while (run >= 8 && out_bits >= 8) {
            put_bits(o, fill, 8);
            out_bits -= 8;
            run = (uint16_t)(run - 8);
        }
        if (out_bits >= run && run > 0) {
            put_bits(o, fill, run);
            out_bits -= run;
        } else if (run > 0) {
            if (out_bits > 0)
                put_bits(o, fill, out_bits);
            out_bits = 0;
        }
    }
    return EXEN_CODEC_OK;
}

/* ---- codec 5: LZ77 (create 0x432a00, decode 0x432a76) --------------------------------------- */
/* Bytes 9..11: unpacked size (24-bit LE). From byte 13: groups of a 32-bit BE control word and
 * 30 items, one per control bit from the MSB: 0 -> literal byte; 1 -> u16 BE match word with a
 * (14 - c) bit distance-1 and a (2 + c) bit length-3, c = control & 3. */
static int codec5(const BitIn *in, uint8_t *dst, size_t cap, size_t *written)
{
    uint32_t size = in_u8(in, 9) | in_u8(in, 10) << 8 | in_u8(in, 11) << 16;
    size_t ip = 13, op = 0;
    int rc = EXEN_CODEC_OK;
    size_t end = size;
    if (end > cap) {
        end = cap;
        rc = EXEN_CODEC_ERR_SIZE;
    }
    *written = 0;
    if (size == 0)
        return rc; /* the original writes one item before checking; not reachable in practice */
    for (;;) {
        int32_t ctl = (int32_t)in_be32(in, ip);
        ip += 4;
        uint32_t dist_bits = 14 - (ctl & 3);
        uint32_t dist_mask = 0x3fffu >> (ctl & 3);
        for (int k = 0; k < 30; k++) {
            if (ctl < 0) {
                uint32_t wv = in_u8(in, ip) << 8 | in_u8(in, ip + 1);
                ip += 2;
                uint32_t n = (wv >> dist_bits) + 3;
                uint32_t dist = (wv & dist_mask) + 1;
                if (op + n > size)
                    n = (uint32_t)(size - op);
                while (n--) {
                    uint8_t c = op >= dist && op - dist < end ? dst[op - dist] : 0;
                    if (op < end)
                        dst[op] = c;
                    op++;
                }
            } else {
                uint8_t c = (uint8_t)in_u8(in, ip++);
                if (op < end)
                    dst[op] = c;
                op++;
            }
            if (op >= size) {
                *written = end;
                return rc;
            }
            ctl = (int32_t)((uint32_t)ctl << 1);
        }
    }
}

/* ---- public API ------------------------------------------------------------------------------ */

int exen_unpack_method(const uint8_t *src, size_t src_len)
{
    if (!src)
        return EXEN_CODEC_ERR_ARG;
    if (src_len < 1)
        return EXEN_CODEC_ERR_SHORT;
    int m = src[0] >> 4;
    return m >= 1 && m <= 5 ? m : EXEN_CODEC_ERR_METHOD;
}

int exen_unpacked_size(const uint8_t *src, size_t src_len, size_t *size)
{
    int m = exen_unpack_method(src, src_len);
    if (m < 0)
        return m;
    if (src_len < (m == 5 ? 12u : 9u))
        return EXEN_CODEC_ERR_SHORT;
    BitIn in = { src, src_len };
    if (m == 5)
        *size = src[9] | (size_t)src[10] << 8 | (size_t)src[11] << 16;
    else
        *size = header_bits(&in) >> 3;
    return EXEN_CODEC_OK;
}

int exen_unpack(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!dst && dst_cap)
        return EXEN_CODEC_ERR_ARG;
    int m = exen_unpack_method(src, src_len);
    if (m < 0)
        return m;
    if (src_len < (m == 5 ? 13u : 9u))
        return EXEN_CODEC_ERR_SHORT;
    BitIn in = { src, src_len };
    if (m == 5) {
        size_t n = 0;
        int rc = codec5(&in, dst, dst_cap, &n);
        if (out_len)
            *out_len = n;
        return rc;
    }
    if (dst_cap > 0x1fffffff)
        return EXEN_CODEC_ERR_SIZE; /* outSize * 8 must fit the original's 32-bit counter */
    BitOut o = { dst, dst_cap, 0, 0 };
    uint32_t out_bits = (uint32_t)dst_cap * 8;
    int rc;
    switch (m) {
    case 1: rc = codec1(&in, &o, out_bits); break;
    case 2: rc = codec2(&in, &o, out_bits); break;
    case 3: rc = codec3(&in, &o, out_bits); break;
    default: rc = codec4(&in, &o, out_bits); break;
    }
    if (out_len) {
        size_t n = (o.hi + 7) / 8;
        *out_len = n < dst_cap ? n : dst_cap;
    }
    return rc;
}

/* ---- ExEn PNG (0x41dacd) ---------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

void exen_image_free(ExenDecodedImage *img)
{
    if (!img)
        return;
    free(img->pixels);
    free(img->raw);
    img->pixels = NULL;
    img->raw = NULL;
}

static void image_init(ExenDecodedImage *out)
{
    memset(out, 0, sizeof *out);
    memset(out->alpha, 0xff, sizeof out->alpha);
    out->transparent_index = -1;
}

static int image_1bpp(const uint8_t *bits, size_t len, uint32_t w, uint32_t h,
                      ExenDecodedImage *out)
{
    if (w == 0 || h == 0 || w > 4096 || h > 4096)
        return EXEN_CODEC_ERR_FORMAT;
    uint32_t pitch = (w + 7) >> 3;
    size_t n = (size_t)pitch * h;
    if (!bits || len < n)
        return EXEN_CODEC_ERR_SHORT;
    out->width = w;
    out->height = h;
    out->bit_depth = 1;
    out->color_type = 0;
    out->codec = 0;
    out->pitch = pitch;
    out->raw_len = n;
    out->raw = malloc(n);
    out->pixels = malloc((size_t)w * h);
    if (!out->raw || !out->pixels) {
        exen_image_free(out);
        return EXEN_CODEC_ERR_NOMEM;
    }
    memcpy(out->raw, bits, n);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            out->pixels[(size_t)y * w + x] = (bits[(size_t)y * pitch + (x >> 3)] >> (7 - (x & 7))) & 1;
    static const uint8_t bw[6] = { 0, 0, 0, 255, 255, 255 };
    memcpy(out->palette, bw, sizeof bw);
    out->palette_count = 2;
    return EXEN_CODEC_OK;
}

int exen_image_from_1bpp(const uint8_t *bits, size_t len, uint32_t width, uint32_t height,
                         ExenDecodedImage *out)
{
    if (!out)
        return EXEN_CODEC_ERR_ARG;
    image_init(out);
    return image_1bpp(bits, len, width, height, out);
}

int exen_image_decode(const uint8_t *res, size_t len, ExenDecodedImage *out)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (!res || !out)
        return EXEN_CODEC_ERR_ARG;
    image_init(out);
    if (len < 6)
        return EXEN_CODEC_ERR_FORMAT; /* readImage throws when fewer than 4 bytes remain */
    out->res_width = (uint16_t)(res[0] | res[1] << 8);
    out->res_height = (uint16_t)(res[2] | res[3] << 8);
    out->res_stream_len = (uint16_t)(res[4] | res[5] << 8);
    if (len < 6 + 8 || memcmp(res + 6, sig, 8) != 0) {
        /* Not a PNG: raw 1-bit bitmap at the cursor (after the 6-byte header). The simulator
         * reads it from the file even past the resource's end; we refuse a short one. */
        int rc = image_1bpp(res + 6, len - 6, out->res_width, out->res_height, out);
        if (rc != EXEN_CODEC_OK) {
            uint16_t rw = out->res_width, rh = out->res_height, rl = out->res_stream_len;
            image_init(out);
            out->res_width = rw;
            out->res_height = rh;
            out->res_stream_len = rl;
        }
        return rc;
    }

    const uint8_t *png = res + 6;
    size_t plen = len - 6, p = 8;
    int have_ihdr = 0;
    while (p + 8 <= plen) {
        uint32_t clen = be32(png + p), type = be32(png + p + 4);
        const uint8_t *d = png + p + 8;
        size_t avail = plen - (p + 8);
        if (clen > avail)
            clen = (uint32_t)avail; /* the simulator reads on into the next resource */
        switch (type) {
        case FOURCC('I', 'H', 'D', 'R'):
            if (clen < 13)
                return EXEN_CODEC_ERR_FORMAT;
            out->width = be32(d);
            out->height = be32(d + 4);
            out->bit_depth = d[8];
            out->color_type = d[9];
            /* compression (always 1), filter and interlace bytes are ignored */
            have_ihdr = 1;
            break;
        case FOURCC('P', 'L', 'T', 'E'): {
            uint32_t n = clen / 3;
            out->palette_count = (uint16_t)(n > 256 ? 256 : n);
            memcpy(out->palette, d, (size_t)out->palette_count * 3);
            break;
        }
        case FOURCC('t', 'R', 'N', 'S'):
            out->has_trns = 1;
            if (out->color_type == 3) {
                for (uint32_t i = 0; i < clen; i++) {
                    if (i < 256)
                        out->alpha[i] = d[i];
                    if (d[i] == 0) {
                        out->transparent_index = (int)i;
                        out->transparent_zero_count++;
                    }
                }
                out->alpha_count = (uint16_t)(clen > 256 ? 256 : clen);
            }
            /* colour type 2 carries an RGB key; truecolour images are refused at IDAT anyway */
            break;
        case FOURCC('I', 'D', 'A', 'T'): {
            if (!have_ihdr)
                return EXEN_CODEC_ERR_FORMAT;
            /* The simulator draws "True color PNG not supported" for type 2 and leaves other
             * non-indexed types blank. */
            if (out->color_type != 3)
                return EXEN_CODEC_ERR_UNSUPPORTED;
            uint32_t w = out->width, h = out->height;
            if (w == 0 || h == 0 || w > 4096 || h > 4096)
                return EXEN_CODEC_ERR_FORMAT;
            out->pitch = out->bit_depth == 4 ? (w + 1) >> 1 : w;
            out->raw_len = (size_t)out->pitch * h;
            int m = exen_unpack_method(d, clen);
            if (m < 0)
                return m;
            out->codec = (uint8_t)m;
            out->raw = calloc(out->raw_len ? out->raw_len : 1, 1);
            out->pixels = malloc((size_t)w * h);
            if (!out->raw || !out->pixels) {
                exen_image_free(out);
                return EXEN_CODEC_ERR_NOMEM;
            }
            size_t got = 0;
            int rc = exen_unpack(d, clen, out->raw, out->raw_len, &got);
            if (rc == EXEN_CODEC_OK && got < out->raw_len)
                rc = EXEN_CODEC_ERR_SIZE;
            if (rc != EXEN_CODEC_OK) {
                exen_image_free(out);
                return rc;
            }
            if (out->bit_depth == 4) {
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t *r = out->raw + (size_t)y * out->pitch;
                    uint8_t *px = out->pixels + (size_t)y * w;
                    for (uint32_t x = 0; x < w; x++)
                        px[x] = (x & 1) ? r[x >> 1] & 15 : r[x >> 1] >> 4;
                }
            } else {
                memcpy(out->pixels, out->raw, (size_t)w * h);
            }
            return EXEN_CODEC_OK;
        }
        case FOURCC('I', 'E', 'N', 'D'):
            return EXEN_CODEC_ERR_FORMAT; /* the simulator returns success with no pixels */
        default:
            break;
        }
        p += 8 + (size_t)clen; /* no CRC */
    }
    return EXEN_CODEC_ERR_FORMAT;
}
