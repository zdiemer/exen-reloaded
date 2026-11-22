/* exen.Graphics (natives 0-14) and exen.Image (15-29), with the built-in font.
 *
 * Sources: docs/re/NATIVES.md §2.1 and docs/re/natives/{eximg_common, graphics_raster,
 * graphics_drawImage, graphics_font, image_decode}.c.txt. Every quirk documented there is kept
 * (clearRect is always 0xFF, setPixel stores the raw byte, getPixel returns -5 outside the clip,
 * paint modes have no effect, new 8bpp images are white, the triangle top-clip bug, the lossy
 * blit-mode transitions, the alpha-into-blue tRNS bug, ...).
 *
 * Native image state ("EXimg")
 * ----------------------------
 * The original keeps a 0x58-byte C struct inside the byte[] that Image's bytecode allocates for
 * image_imgT (its size comes from getSizeOfEXimgStruct), with host pointers in it. We keep the
 * same struct in the same byte[], but every pointer becomes an arena reference, so the image
 * state is part of the arena and save states stay a plain copy. Layout (u32 little-endian):
 *
 *   +0x00 data       ref of the image_Data byte[] (pixels; re-synced on every access, as the
 *                    original re-syncs its data pointer in getImage 0x426785)
 *   +0x04 w  +0x08 h  +0x0c size (= pitch*h)  +0x10 depth (4 or 8)  +0x14 pitch
 *   +0x18 clipX +0x1c clipY +0x20 clipW +0x24 clipH   (copied from the Graphics before each op)
 *   +0x28 ops        4 or 8 when the depth has an ops table, else 0 (the vtable pointer)
 *   +0x2c blitMode   0x10..0x50
 *   +0x30..+0x3c     unused (blitter function pointers in the original)
 *   +0x40 transpIdx  +0x44 pal  +0x48 palCount  +0x4c palVersion  +0x50 alphaMode  +0x54 (=1)
 *   +0x58 u32[256]   palette storage for palettes the decoder creates natively
 *
 * `pal` is either the ref of a Java int[] (Palette.colorTabRGBA, which the original aliases),
 * EXIMG_PAL_NATIVE for the inline storage at +0x58 (the original mallocs it), or 0.
 * `palVersion` is the ref of the Palette object whose paletteVersion field keys the global LUT
 * (the original stores a pointer to that field). Neither ref is traced by the GC: the int[] and
 * the Palette are kept alive by Image.palette, exactly as in the original, and every use
 * validates the ref before reading through it.
 *
 * The one deviation: getSizeOfEXimgStruct returns 0x458 instead of 0x58 to make room for the
 * inline palette. The method is private and only sizes that byte[].
 *
 * Pixels live in image_Data: 8bpp is one byte per pixel (RGB332, or a palette index), 4bpp is
 * two pixels per byte, the even x in the high nibble, pitch (w+1)/2. Every access is bounds
 * checked against the array; the original's out-of-range reads and writes become no-ops.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include "platform.h"

#define EXIMG_SIZE 0x458u
#define EXIMG_NATIVE_PAL 0x58u

/* Field slots of the framework classes (tools/cvp.py dump of the vendored gensimu.exe). */
enum { G_PIXIMA, G_COLOR, G_PAINTMODE, G_CLIPX, G_CLIPY, G_CLIPW, G_CLIPH };
enum { I_WIDTH, I_HEIGHT, I_DEPTH, I_DATA, I_IMGT, I_PALETTE };
enum { PAL_VERSION, PAL_TAB };
enum { TM_PTU, TM_PTV, TM_PIXIMAGE };
enum { R_START, R_LEN, R_OFFSET, R_ID, R_ARRAY };

/* ------------------------------------------------------------ platform helpers */

static const uint32_t class_hash[PC_COUNT] = {
    [PC_GRAPHICS] = 0xc6ed8e2au, [PC_IMAGE] = 0x23c5e7e8u, [PC_PALETTE] = 0x5562ca3bu,
    [PC_RESOURCE] = 0xbab5c664u, [PC_TEXTUREMAP] = 0x3834a617u, [PC_GAMELET] = 0xe127b0e1u,
    [PC_ANIMBITMAP] = 0xe7167d52u, [PC_PLAYFIELD] = 0x7219d0b4u, [PC_SPRITE] = 0x60fe5152u,
    [PC_ANIMFLASH] = 0xd414954au,
};

int plat_class(ExenVM *vm, int which)
{
    struct ExenPlatform *p = vm->plat;
    if (p->cls[which] == -2)
        p->cls[which] = find_class(vm, class_hash[which]);
    return p->cls[which];
}

void plat_warn_once(ExenVM *vm, int bit, const char *fmt, ...)
{
    if (vm->plat->warned & (1u << bit))
        return;
    vm->plat->warned |= 1u << bit;
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vm_log(vm, "%s", buf);
}

enum { W_CODEC, W_4BPP_RAW, W_NOOPS, W_TRUECOLOR, W_TEX, W_1BPP };

static int is_array_of(ExenVM *vm, ref_t r, uint16_t tw)
{
    return r && heap_is_object(vm, r) && is_array(vm, r) && arr_type(vm, r) == tw;
}

/* ------------------------------------------------------------ EXimg access */

static uint8_t *imgt_bytes(ExenVM *vm, ref_t imgt) { return arr_data(vm, imgt); }

int eximg_get(ExenVM *vm, ref_t image, EXimg *e)
{
    memset(e, 0, sizeof *e);
    int ci = plat_class(vm, PC_IMAGE);
    if (!image || ci < 0)
        return 0;
    uint32_t *f = inst(vm, image, ci);
    ref_t imgt = f[I_IMGT], data = f[I_DATA];
    if (!imgt || !data || arr_len(vm, imgt) < EXIMG_SIZE)
        return 0;
    const uint8_t *b = imgt_bytes(vm, imgt);
    e->self = imgt;
    e->data_ref = data;
    e->w = (int32_t)rd32(b + 0x04);
    e->h = (int32_t)rd32(b + 0x08);
    e->size = (int32_t)rd32(b + 0x0c);
    e->depth = (int32_t)rd32(b + 0x10);
    e->pitch = (int32_t)rd32(b + 0x14);
    e->clipX = (int32_t)rd32(b + 0x18);
    e->clipY = (int32_t)rd32(b + 0x1c);
    e->clipW = (int32_t)rd32(b + 0x20);
    e->clipH = (int32_t)rd32(b + 0x24);
    e->ops = (int32_t)rd32(b + 0x28);
    e->blitMode = rd32(b + 0x2c);
    e->transpIdx = rd32(b + 0x40);
    e->pal = rd32(b + 0x44);
    e->palCount = rd32(b + 0x48);
    e->palVersion = rd32(b + 0x4c);
    e->alphaMode = rd32(b + 0x50);
    e->f54 = rd32(b + 0x54);
    return 1;
}

void eximg_put(ExenVM *vm, const EXimg *e)
{
    if (!e->self)
        return;
    uint8_t *b = imgt_bytes(vm, e->self);
    wr32(b + 0x00, e->data_ref);
    wr32(b + 0x04, (uint32_t)e->w);
    wr32(b + 0x08, (uint32_t)e->h);
    wr32(b + 0x0c, (uint32_t)e->size);
    wr32(b + 0x10, (uint32_t)e->depth);
    wr32(b + 0x14, (uint32_t)e->pitch);
    wr32(b + 0x18, (uint32_t)e->clipX);
    wr32(b + 0x1c, (uint32_t)e->clipY);
    wr32(b + 0x20, (uint32_t)e->clipW);
    wr32(b + 0x24, (uint32_t)e->clipH);
    wr32(b + 0x28, (uint32_t)e->ops);
    wr32(b + 0x2c, e->blitMode);
    wr32(b + 0x40, e->transpIdx);
    wr32(b + 0x44, e->pal);
    wr32(b + 0x48, e->palCount);
    wr32(b + 0x4c, e->palVersion);
    wr32(b + 0x50, e->alphaMode);
    wr32(b + 0x54, e->f54);
}

uint8_t *eximg_data(ExenVM *vm, const EXimg *e, uint32_t *len)
{
    *len = arr_len(vm, e->data_ref);
    return arr_data(vm, e->data_ref);
}

static uint32_t pal_get(ExenVM *vm, const EXimg *e, uint32_t i)
{
    if (e->pal == EXIMG_PAL_NATIVE)
        return i < 256 ? rd32(imgt_bytes(vm, e->self) + EXIMG_NATIVE_PAL + 4 * i) : 0;
    if (is_array_of(vm, e->pal, T_INT) && i < arr_len(vm, e->pal))
        return rd32(arr_data(vm, e->pal) + 4 * i);
    return 0;
}

static void pal_set_native(ExenVM *vm, EXimg *e, uint32_t i, uint32_t v)
{
    if (i < 256)
        wr32(imgt_bytes(vm, e->self) + EXIMG_NATIVE_PAL + 4 * i, v);
}

static uint8_t rgb332(uint32_t rgb)
{
    return (uint8_t)(((rgb >> 16) & 0xe0) | ((rgb >> 11) & 0x1c) | ((rgb >> 6) & 0x03));
}

/* Palette -> RGB332 lookup table, cached by palette version (0x42d279). */
static void lut_prime(ExenVM *vm, const EXimg *e)
{
    struct ExenPlatform *p = vm->plat;
    if (e->palCount == 0)
        return;
    int cp = plat_class(vm, PC_PALETTE);
    int have_ver = e->palVersion && cp >= 0 && heap_is_object(vm, e->palVersion) &&
                   !is_array(vm, e->palVersion);
    uint32_t ver = have_ver ? inst(vm, e->palVersion, cp)[PAL_VERSION] : 0;
    if (have_ver && p->lut_valid && ver == p->lut_version)
        return;
    uint32_t n = e->palCount > 256 ? 256 : e->palCount;
    for (uint32_t i = 0; i < n; i++)
        p->lut[i] = rgb332(pal_get(vm, e, i));
    if (have_ver) {
        p->lut_version = ver;
        p->lut_valid = 1;
    }
}

const uint8_t *eximg_lut(ExenVM *vm, const EXimg *e)
{
    lut_prime(vm, e);
    return vm->plat->lut;
}

/* setBlitMode (0x41a2a0 / 0x418f00): the mode is kept in the high nibble. */
static void set_blit_mode(EXimg *e, uint32_t mode) { e->blitMode = mode & 0xf0; }

/* ------------------------------------------------------------ pixel primitives */

static void put_px(uint8_t *d, uint32_t len, const EXimg *e, int x, int y, uint8_t c)
{
    if (x < 0 || y < 0)
        return;
    if (e->ops == 8) {
        uint32_t i = (uint32_t)y * (uint32_t)e->pitch + (uint32_t)x;
        if (i < len)
            d[i] = c;
    } else if (e->ops == 4) {
        uint32_t i = (uint32_t)y * (uint32_t)e->pitch + ((uint32_t)x >> 1);
        if (i < len)
            d[i] = (x & 1) ? (uint8_t)((d[i] & 0xf0) | (c & 0x0f))
                           : (uint8_t)((d[i] & 0x0f) | (c << 4));
    }
}

static int get_px(const uint8_t *d, uint32_t len, const EXimg *e, int x, int y)
{
    if (x < 0 || y < 0)
        return 0;
    if (e->ops == 4) {
        uint32_t i = (uint32_t)y * (uint32_t)e->pitch + ((uint32_t)x >> 1);
        if (i >= len)
            return 0;
        return (x & 1) ? d[i] & 0x0f : d[i] >> 4;
    }
    uint32_t i = (uint32_t)y * (uint32_t)e->pitch + (uint32_t)x;
    return i < len ? d[i] : 0;
}

static void hline(uint8_t *d, uint32_t len, const EXimg *e, int x, int y, int n, uint8_t c)
{
    if (n <= 0 || y < 0)
        return;
    if (e->ops == 8) {
        if (x < 0) {
            n += x;
            x = 0;
        }
        uint64_t i = (uint64_t)y * (uint32_t)e->pitch + (uint32_t)x;
        if (n <= 0 || i >= len)
            return;
        if (i + (uint32_t)n > len)
            n = (int)(len - i);
        memset(d + i, c, (size_t)n);
    } else {
        for (int k = 0; k < n; k++)
            put_px(d, len, e, x + k, y, c);
    }
}

/* ------------------------------------------------------------ Graphics helpers */

static uint32_t *gfields(ExenVM *vm, ref_t g) { return inst(vm, g, plat_class(vm, PC_GRAPHICS)); }

void eximg_sync_clip(ExenVM *vm, ref_t g, EXimg *e)
{
    const uint32_t *f = gfields(vm, g);
    e->clipX = (int32_t)f[G_CLIPX];
    e->clipY = (int32_t)f[G_CLIPY];
    e->clipW = (int32_t)f[G_CLIPW];
    e->clipH = (int32_t)f[G_CLIPH];
}

/* The target EXimg of a Graphics, clip synced. 0 if there is none (the original returns -2). */
static int gfx_target(ExenVM *vm, ref_t g, EXimg *e)
{
    if (!g || plat_class(vm, PC_GRAPHICS) < 0)
        return 0;
    if (!eximg_get(vm, gfields(vm, g)[G_PIXIMA], e))
        return 0;
    eximg_sync_clip(vm, g, e);
    eximg_put(vm, e);
    return e->ops != 0;
}

static uint8_t cur_color(ExenVM *vm, ref_t g) { return (uint8_t)gfields(vm, g)[G_COLOR]; }

typedef struct { int32_t x, y, w, h; } Rect;

/* rectIntersect (0x4177c6): half-open; the result w/h may be negative. */
static void rect_clip(Rect *r, int32_t cx, int32_t cy, int32_t cw, int32_t ch)
{
    int64_t r0 = (int64_t)r->x + r->w, r1 = (int64_t)r->y + r->h;
    int64_t c0 = (int64_t)cx + cw, c1 = (int64_t)cy + ch;
    if (r->x < cx) r->x = cx;
    if (r->y < cy) r->y = cy;
    r->w = (int32_t)((r0 < c0 ? r0 : c0) - r->x);
    r->h = (int32_t)((r1 < c1 ? r1 : c1) - r->y);
}

/* fillRectCore (0x417d92). */
static void fill_rect_core(ExenVM *vm, EXimg *e, int32_t x, int32_t y, int32_t w, int32_t h,
                           uint8_t c)
{
    Rect r = {x, y, w, h};
    rect_clip(&r, e->clipX, e->clipY, e->clipW, e->clipH);
    if (r.w <= 0 || r.h <= 0)
        return;
    uint32_t len;
    uint8_t *d = eximg_data(vm, e, &len);
    for (int32_t j = 0; j < r.h; j++)
        hline(d, len, e, r.x, r.y + j, r.w, c);
}

static int in_clip(const EXimg *e, int32_t x, int32_t y)
{
    return x >= e->clipX && (int64_t)x < (int64_t)e->clipX + e->clipW && y >= e->clipY &&
           (int64_t)y < (int64_t)e->clipY + e->clipH;
}

/* ------------------------------------------------------------ lines (0x4179d2) */

static int outcode(const EXimg *e, int32_t x, int32_t y)
{
    int c = 0;
    if (x < e->clipX) c |= 1;
    else if ((int64_t)x >= (int64_t)e->clipX + e->clipW) c |= 2;
    if (y < e->clipY) c |= 8;
    else if ((int64_t)y >= (int64_t)e->clipY + e->clipH) c |= 4;
    return c;
}

/* (a + (b * num) / den) truncated to int16, with 32-bit wrap-around like the original. */
static int32_t clip_coord(int32_t a, int32_t b, int32_t num, int32_t den)
{
    int32_t prod = (int32_t)((uint32_t)b * (uint32_t)num);
    int32_t q = (den == -1 && prod == INT32_MIN) ? prod : prod / den;
    return (int16_t)(uint16_t)(uint32_t)(a + q);
}

static void line_core(ExenVM *vm, EXimg *e, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      uint8_t c)
{
    uint32_t len;
    uint8_t *d = eximg_data(vm, e, &len);
    if (x0 == x1 && y0 == y1) {
        if (in_clip(e, x0, y0))
            put_px(d, len, e, x0, y0, c);
        return;
    }
    if (outcode(e, x0, y0) & outcode(e, x1, y1))
        return;
    int32_t xmax = e->clipX + e->clipW - 1, ymax = e->clipY + e->clipH - 1;
    while (outcode(e, x0, y0) || outcode(e, x1, y1)) {
        if (x0 < e->clipX && x1 != x0) {
            y0 = clip_coord(y0, y1 - y0, e->clipX - x0, x1 - x0);
            x0 = (int16_t)e->clipX;
        }
        if (y0 < e->clipY && y1 != y0) {
            x0 = clip_coord(x0, x1 - x0, e->clipY - y0, y1 - y0);
            y0 = (int16_t)e->clipY;
        }
        if (x0 > xmax && x1 != x0) {
            y0 = clip_coord(y0, y1 - y0, xmax - x0, x1 - x0);
            x0 = (int16_t)xmax;
        }
        if (y0 > ymax && y1 != y0) {
            x0 = clip_coord(x0, x1 - x0, ymax - y0, y1 - y0);
            y0 = (int16_t)ymax;
        }
        if (outcode(e, x0, y0))
            return;
        int32_t t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    int32_t dx = (int16_t)abs(x1 - x0), dy = (int16_t)abs(y1 - y0);
    int32_t n, incE, incNE, dd, sx1, sy1, sx2 = 1, sy2 = 1;
    if (dx >= dy) {
        n = dx + 1; incE = 2 * dy; incNE = 2 * (dy - dx); dd = 2 * dy - dx;
        sx1 = 1; sy1 = 0;
    } else {
        n = dy + 1; incE = 2 * dx; incNE = 2 * (dx - dy); dd = 2 * dx - dy;
        sx1 = 0; sy1 = 1;
    }
    if (x0 > x1) { sx1 = -sx1; sx2 = -sx2; }
    if (y0 > y1) { sy1 = -sy1; sy2 = -sy2; }
    int32_t x = x0, y = y0;
    while (n-- > 0) {
        if (e->ops == 8) {
            uint32_t i = (uint16_t)(uint32_t)(x + y * e->pitch); /* ops8 0x42c017 masks to 16 bits */
            if (i < len)
                d[i] = c;
        } else {
            put_px(d, len, e, x, y, c);
        }
        if (dd < 0) { dd += incE; x += sx1; y += sy1; }
        else        { dd += incNE; x += sx2; y += sy2; }
    }
}

/* polyline core 0x417e45: closed. */
static void polyline(ExenVM *vm, EXimg *e, int n, const int32_t *xs, const int32_t *ys, uint8_t c)
{
    for (int i = 0; i < n - 1; i++)
        line_core(vm, e, xs[i], ys[i], xs[i + 1], ys[i + 1], c);
    line_core(vm, e, xs[n - 1], ys[n - 1], xs[0], ys[0], c);
}

/* ------------------------------------------------------------ triangles */

#define SWAP3(a, b) do { int32_t t_; t_ = a##x; a##x = b##x; b##x = t_; \
                         t_ = a##y; a##y = b##y; b##y = t_; } while (0)

static int32_t sdiv(int32_t a, int32_t b)
{
    if (b == 0) return 0;
    if (b == -1 && a == INT32_MIN) return a;
    return a / b;
}
static int32_t wmul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static int32_t fx16(int32_t v) { return (int32_t)((uint32_t)v << 16); }

/* fillTriangleCore 0x41885c, top-clip bug included. */
static void fill_triangle(ExenVM *vm, EXimg *e, int32_t p0x, int32_t p0y, int32_t p1x,
                          int32_t p1y, int32_t p2x, int32_t p2y, uint8_t c)
{
    if (p1y < p0y) SWAP3(p0, p1);
    if (p2y < p0y) SWAP3(p0, p2);
    if (p2y < p1y) SWAP3(p1, p2);
    int32_t y0 = p0y, y1 = p1y, y2 = p2y;
    if ((int64_t)y0 >= (int64_t)e->clipY + e->clipH || y2 < e->clipY)
        return;
    int32_t x0 = fx16(p0x), x1 = fx16(p1x), x2 = fx16(p2x);
    int32_t xa = x0, xb = x0, xc = x1, s01 = 0, s02 = 0, s12 = 0;
    if (y1 - y0) s01 = sdiv(wsub(x1, x0), y1 - y0);
    if (y2 - y0) s02 = sdiv(wsub(x2, x0), y2 - y0);
    if (y2 - y1) s12 = sdiv(wsub(x2, x1), y2 - y1);
    if (y1 <= e->clipY) {
        xc = wsub(xc, wmul(y1, s12));
        xb = wsub(xb, wmul(y0, s02));
        y1 = y0 = e->clipY;
    } else if (y0 <= e->clipY) {
        xa = wsub(xa, wmul(y0, s01));
        xb = wsub(xb, wmul(y0, s02));
        y0 = e->clipY;
    }
    int32_t yend = e->clipY + e->clipH;
    if (y1 > yend) y2 = y1 = yend;
    else if (y2 > yend) y2 = yend;
    uint32_t len;
    uint8_t *d = eximg_data(vm, e, &len);
    int32_t cr = e->clipX + e->clipW;
    for (int half = 0; half < 2; half++) {
        for (int32_t y = y0; y < y1; y++) {
            int32_t l = xa >> 16, r = xb >> 16;
            if (r < l) { int32_t t = l; l = r; r = t; }
            if (l <= cr && r >= e->clipX) {
                if (l < e->clipX) l = e->clipX;
                if (r >= cr) r = cr - 1;
                if (l <= r)
                    hline(d, len, e, l, y, r - l + 1, c);
            }
            xa = wadd(xa, s01);
            xb = wadd(xb, s02);
        }
        y0 = y1;
        y1 = y2;
        xa = xc;
        s01 = s12;
    }
}

/* fillTextureTriangle core 0x4181e8 and ops8 texSpan 0x42c0be (medium confidence). */
typedef struct { int32_t x, y, u, v; } TexV;

static void tex_span(ExenVM *vm, EXimg *d, uint8_t *dd, uint32_t dlen, EXimg *s, int32_t u,
                     int32_t v, int32_t x, int32_t y, int32_t n, int32_t du, int32_t dv)
{
    uint32_t slen;
    const uint8_t *sd = eximg_data(vm, s, &slen);
    const uint8_t *lut = vm->plat->lut;
    u = wadd(u, 0x2000);
    v = wadd(v, 0x2000);
    if (s->depth == 4) {
        u >>= 1;
        du >>= 1;
    }
    for (int32_t k = 0; k < n; k++) {
        int32_t sx = u >> 16, sy = v >> 16;
        uint32_t si = (uint32_t)sy * (uint32_t)s->pitch + (uint32_t)sx;
        uint8_t texel = 0;
        if (sx >= 0 && sy >= 0 && si < slen) {
            texel = sd[si];
            if (s->depth == 4)
                texel = (uint8_t)(((sx & ~1) ? texel >> 4 : texel) & 0x0f); /* sic: &~1 */
        }
        put_px(dd, dlen, d, x + k, y, lut[texel]);
        u = wadd(u, du);
        v = wadd(v, dv);
    }
}

static void fill_texture_triangle(ExenVM *vm, EXimg *e, TexV p0, TexV p1, TexV p2, EXimg *s)
{
    if (s->depth > e->depth || e->palCount != 0 || e->ops != 8)
        return; /* ops8 texSpan fails; ops4 texSpan is a stub */
    if (s->palCount == 0) {
        plat_warn_once(vm, W_TEX, "fillTextureTriangle: texture without a palette (the original crashes)");
        return;
    }
    lut_prime(vm, s);
    TexV t;
    if (p1.y < p0.y) { t = p0; p0 = p1; p1 = t; }
    if (p2.y < p0.y) { t = p0; p0 = p2; p2 = t; }
    if (p2.y < p1.y) { t = p1; p1 = p2; p2 = t; }
    int32_t y0 = p0.y, y1 = p1.y, y2 = p2.y;
    if ((int64_t)y0 >= (int64_t)e->clipY + e->clipH || y2 < e->clipY)
        return;
    /* x, u, v as 16.16 with +0.5 */
    int32_t X[3], U[3], V[3];
    const TexV *pv[3] = {&p0, &p1, &p2};
    for (int i = 0; i < 3; i++) {
        X[i] = wadd(fx16(pv[i]->x), 0x8000);
        U[i] = wadd(fx16(pv[i]->u), 0x8000);
        V[i] = wadd(fx16(pv[i]->v), 0x8000);
    }
    int32_t xa = X[0], ua = U[0], va = V[0], xb = X[0], ub = U[0], vb = V[0];
    int32_t xc = X[1], uc = U[1], vc = V[1];
    int32_t sx01 = 0, su01 = 0, sv01 = 0, sx02 = 0, su02 = 0, sv02 = 0, sx12 = 0, su12 = 0, sv12 = 0;
    if (y1 - y0) { sx01 = sdiv(wsub(X[1], X[0]), y1 - y0); su01 = sdiv(wsub(U[1], U[0]), y1 - y0); sv01 = sdiv(wsub(V[1], V[0]), y1 - y0); }
    if (y2 - y0) { sx02 = sdiv(wsub(X[2], X[0]), y2 - y0); su02 = sdiv(wsub(U[2], U[0]), y2 - y0); sv02 = sdiv(wsub(V[2], V[0]), y2 - y0); }
    if (y2 - y1) { sx12 = sdiv(wsub(X[2], X[1]), y2 - y1); su12 = sdiv(wsub(U[2], U[1]), y2 - y1); sv12 = sdiv(wsub(V[2], V[1]), y2 - y1); }
    if (y1 <= e->clipY) {
        xc = wsub(xc, wmul(y1, sx12)); uc = wsub(uc, wmul(y1, su12)); vc = wsub(vc, wmul(y1, sv12));
        xb = wsub(xb, wmul(y0, sx02)); ub = wsub(ub, wmul(y0, su02)); vb = wsub(vb, wmul(y0, sv02));
        y1 = y0 = e->clipY;
    } else if (y0 <= e->clipY) {
        xa = wsub(xa, wmul(y0, sx01)); ua = wsub(ua, wmul(y0, su01)); va = wsub(va, wmul(y0, sv01));
        xb = wsub(xb, wmul(y0, sx02)); ub = wsub(ub, wmul(y0, su02)); vb = wsub(vb, wmul(y0, sv02));
        y0 = e->clipY;
    }
    int32_t yend = e->clipY + e->clipH;
    if (y1 > yend) y2 = y1 = yend;
    else if (y2 > yend) y2 = yend;
    uint32_t dlen;
    uint8_t *dd = eximg_data(vm, e, &dlen);
    int32_t cr = e->clipX + e->clipW;
    for (int half = 0; half < 2; half++) {
        for (int32_t y = y0; y < y1; y++) {
            int32_t l = xa >> 16, r = xb >> 16, ul = ua, vl = va, ur = ub, vr = vb;
            if (r < l) {
                int32_t t2 = l; l = r; r = t2;
                ul = ub; vl = vb; ur = ua; vr = va;
            }
            int32_t du = 0, dv = 0;
            if (r != l) {
                du = sdiv(wsub(ur, ul), r - l);
                dv = sdiv(wsub(vr, vl), r - l);
            }
            if (!(l >= cr || r < e->clipX)) {
                if (l < e->clipX) {
                    ul = wsub(ul, wmul(l, du)); /* sic: absolute coordinate, as the original */
                    vl = wsub(vl, wmul(l, dv));
                    l = e->clipX;
                }
                if (r >= cr) r = cr - 1;
                if (l <= r)
                    tex_span(vm, e, dd, dlen, s, ul, vl, l, y, r - l + 1, du, dv);
            }
            xa = wadd(xa, sx01); ua = wadd(ua, su01); va = wadd(va, sv01);
            xb = wadd(xb, sx02); ub = wadd(ub, su02); vb = wadd(vb, sv02);
        }
        y0 = y1; y1 = y2;
        xa = xc; ua = uc; va = vc;
        sx01 = sx12; su01 = su12; sv01 = sv12;
    }
}

/* ------------------------------------------------------------ blits */

/* One source pixel through the source's blit mode onto an 8bpp destination byte. */
typedef struct {
    uint32_t mode, key;
    const uint8_t *lut;
    uint8_t alpha[256];
} BlitOp;

static void blit_op_init(ExenVM *vm, BlitOp *op, EXimg *s)
{
    lut_prime(vm, s); /* primes the global LUT the row blitters read */
    op->lut = vm->plat->lut;
    op->mode = s->blitMode & 0xf0;
    op->key = s->transpIdx;
    if (op->mode < 0x10 || op->mode > 0x50)
        op->mode = 0x10;
    if (op->mode == 0x30)
        for (int i = 0; i < 256; i++)
            op->alpha[i] = (uint8_t)(pal_get(vm, s, (uint32_t)i) >> 24);
}

static void blend_px(const BlitOp *op, uint8_t *dst, uint8_t v)
{
    switch (op->mode) {
    case 0x10: *dst = v; break;
    case 0x20: if (v != op->key) *dst = v; break;
    case 0x40: *dst = op->lut[v]; break;
    case 0x50: if (v != op->key) *dst = op->lut[v]; break;
    case 0x30: {
        uint32_t a = op->alpha[v];
        uint8_t c = op->lut[v];
        if (a == 255) {
            *dst = c;
        } else if (a != 0) {
            static const uint32_t m[3] = {0xe0, 0x1c, 0x03};
            uint32_t out = 0, dv = *dst;
            for (int k = 0; k < 3; k++) {
                uint32_t f = ((((c & m[k]) - (dv & m[k])) * a) >> 8) + (dv & m[k]);
                out |= f & m[k];
            }
            *dst = (uint8_t)out;
        }
        break;
    }
    }
}

/* Source sample. 4bpp sources in modes 0x10/0x20 go through the 8bpp row blitters in the
 * original (0x418f00 calls setBlitMode8), which read whole bytes: reproduced. */
static int src_px(const uint8_t *sd, uint32_t slen, const EXimg *s, int raw4, int32_t x, int32_t y)
{
    if (s->depth == 4 && !raw4)
        return get_px(sd, slen, s, x, y);
    uint32_t i = (uint32_t)y * (uint32_t)s->pitch + (uint32_t)x;
    return i < slen ? sd[i] : -1;
}

int eximg_draw_image(ExenVM *vm, EXimg *d, int dx, int dy, int dw, int dh, EXimg *s, int sx,
                     int sy, int sw, int sh, int tr)
{
    if (!d->self || !s->self)
        return -2;
    if (d->depth < s->depth)
        return -5;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return -5;
    int64_t cr = (int64_t)d->clipX + d->clipW, cb = (int64_t)d->clipY + d->clipH;
    if (dx >= cr || dy >= cb)
        return -5;
    if (tr == 4 || tr == 5) {
        if ((int64_t)dx + dh < d->clipX || (int64_t)dy + dw < d->clipY)
            return -5;
    } else if ((int64_t)dx + dw < d->clipX || (int64_t)dy + dh < d->clipY) {
        return -5;
    }
    if (sx > s->w || sy > s->h)
        return -5;
    if ((int64_t)sx + sw < 0 || (int64_t)sy + sh < 0)
        return -5;
    if (d->ops != 8)
        return 1; /* ops4 blit/blitScaled are stubs */
    BlitOp op;
    blit_op_init(vm, &op, s);
    int raw4 = s->depth == 4 && (op.mode == 0x10 || op.mode == 0x20);
    if (raw4)
        plat_warn_once(vm, W_4BPP_RAW, "drawImage: 4bpp source in blit mode 0x%x (read as bytes, as the original)", op.mode);
    uint32_t dlen, slen;
    uint8_t *dd = eximg_data(vm, d, &dlen);
    const uint8_t *sd = eximg_data(vm, s, &slen);
    if (sw == dw && sh == dh) {
        int W = (tr == 4 || tr == 5) ? sh : sw, H = (tr == 4 || tr == 5) ? sw : sh;
        Rect r = {dx, dy, W, H};
        rect_clip(&r, d->clipX, d->clipY, d->clipW, d->clipH);
        for (int32_t Y = r.y; Y < r.y + r.h; Y++) {
            int32_t j = Y - dy;
            uint8_t *row = dd + (uint32_t)Y * (uint32_t)d->pitch;
            for (int32_t X = r.x; X < r.x + r.w; X++) {
                int32_t i = X - dx, px, py;
                switch (tr) {
                case 1: px = sx + i; py = sy + sh - 1 - j; break;
                case 2: px = sx + sw - 1 - i; py = sy + j; break;
                case 3: px = sx + sw - 1 - i; py = sy + sh - 1 - j; break;
                case 4: px = sx + sw - 1 - j; py = sy + i; break;
                case 5: px = sx + j; py = sy + sh - 1 - i; break;
                default: px = sx + i; py = sy + j; break;
                }
                if (px < 0 || py < 0 || px >= s->w || py >= s->h)
                    continue;
                int v = src_px(sd, slen, s, raw4, px, py);
                uint32_t di = (uint32_t)Y * (uint32_t)d->pitch + (uint32_t)X;
                if (v >= 0 && di < dlen)
                    blend_px(&op, row + X, (uint8_t)v);
            }
        }
        return 1;
    }
    /* scaled, nearest neighbour, transform ignored (ops8 0x42ca5b) */
    int32_t xs = (int32_t)(((int64_t)sw << 16) / dw), ys = (int32_t)(((int64_t)sh << 16) / dh);
    Rect r = {dx, dy, dw, dh};
    rect_clip(&r, d->clipX, d->clipY, d->clipW, d->clipH);
    for (int32_t Y = r.y; Y < r.y + r.h; Y++) {
        int32_t py = (int32_t)((((int64_t)sy << 16) + (int64_t)(Y - dy) * ys) >> 16);
        if (py < 0 || py >= s->h)
            continue;
        uint8_t *row = dd + (uint32_t)Y * (uint32_t)d->pitch;
        for (int32_t X = r.x; X < r.x + r.w; X++) {
            int32_t px = (int32_t)((((int64_t)sx << 16) + (int64_t)(X - dx) * xs) >> 16);
            if (px < 0 || px >= s->w)
                continue;
            int v = src_px(sd, slen, s, raw4, px, py);
            uint32_t di = (uint32_t)Y * (uint32_t)d->pitch + (uint32_t)X;
            if (v >= 0 && di < dlen)
                blend_px(&op, row + X, (uint8_t)v);
        }
    }
    return 1;
}

/* ------------------------------------------------------------ font (0x413b4f / 0x413df7) */

#define GLYPH_W 5
#define GLYPH_H 9
#define GLYPH_ADV (GLYPH_W + 1)

static void draw_glyph(ExenVM *vm, EXimg *e, uint8_t *d, uint32_t len, int32_t x, int32_t y,
                       int32_t bitX, uint8_t c)
{
    const uint8_t *font = vm->plat->font;
    int32_t r0 = 0, r1 = GLYPH_H - 1;
    if (y < e->clipY) {
        if (y < e->clipY - (GLYPH_H - 1))
            return;
        r0 = e->clipY - y;
        y = e->clipY;
    } else if (y > e->clipY + e->clipH - GLYPH_H) {
        if (y > e->clipY + e->clipH)
            return;
        r1 = e->clipY + e->clipH - y;
    }
    int32_t c0 = bitX, c1;
    if (x < e->clipX) {
        if (x < e->clipX - GLYPH_W)
            return;
        int32_t cut = e->clipX - x, vis = GLYPH_W - cut;
        c0 = bitX + cut;
        x = e->clipX;
        c1 = c0 + (vis < e->clipW ? vis : e->clipW);
    } else {
        c1 = bitX + GLYPH_W;
        if (x > e->clipX + e->clipW - GLYPH_W) {
            if (x > e->clipX + e->clipW)
                return;
            c1 = bitX + (e->clipX + e->clipW) - x;
        }
    }
    if (r0 >= r1)
        return;
    /* drawGlyph writes one byte per pixel whatever the depth (corrupts 4bpp images). */
    for (int32_t r = r0; r < r1; r++) {
        if (r < 0 || r >= 8)
            continue;
        const uint8_t *src = font + EXEN_FONT_STRIDE * r;
        int64_t base = (int64_t)x + (int64_t)(y + (r - r0)) * e->pitch;
        for (int32_t b = c0; b < c1; b++) {
            if (b < 0 || (b >> 3) >= EXEN_FONT_STRIDE)
                continue;
            if (!(src[b >> 3] & (1 << (7 - (b & 7))))) {
                int64_t i = base + (b - c0);
                if (i >= 0 && i < (int64_t)len)
                    d[i] = c;
            }
        }
    }
}

void eximg_draw_text(ExenVM *vm, EXimg *e, int32_t x, int32_t y, const uint8_t *s, int32_t len,
                     uint8_t c)
{
    if (!s || !vm->plat->have_font)
        return;
    if (y <= e->clipY - GLYPH_H || y > e->clipY + e->clipH - 1 || e->clipW == 0 || e->clipH == 0)
        return;
    int32_t L = e->clipX, R = e->clipX + e->clipW;
    if (x + len * GLYPH_ADV <= L || x > R || R <= 0)
        return;
    int32_t x0 = x;
    if (x < L) {
        int32_t skip = abs(L - abs(x)) / GLYPH_ADV; /* exact original formula (0x413c6d) */
        if (skip > 0) {
            x += skip * GLYPH_ADV;
            x0 = x;
        }
        s += skip;
        len -= skip;
    }
    if (x + len * GLYPH_ADV > R)
        len -= (x + len * GLYPH_ADV - R) / GLYPH_ADV;
    uint32_t dlen;
    uint8_t *d = eximg_data(vm, e, &dlen);
    for (x = x0; len > 0 && *s != 0xff; s++, len--, x += GLYPH_ADV)
        draw_glyph(vm, e, d, dlen, x, y, (int32_t)*s * GLYPH_W, c);
}

/* ------------------------------------------------------------ Graphics natives */

#define ARG(i) ((int32_t)a[i])

static int n_clearRect(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (gfx_target(vm, a[0], &e))
        fill_rect_core(vm, &e, ARG(1), ARG(2), ARG(3), ARG(4), 0xff);
    return 0;
}

static int n_fillRect(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (gfx_target(vm, a[0], &e))
        fill_rect_core(vm, &e, ARG(1), ARG(2), ARG(3), ARG(4), cur_color(vm, a[0]));
    return 0;
}

static int n_drawImage(ExenVM *vm, uint32_t *a)
{
    EXimg d, s;
    if (!gfx_target(vm, a[0], &d) || !eximg_get(vm, a[1], &s))
        return 0;
    eximg_draw_image(vm, &d, ARG(2), ARG(3), ARG(4), ARG(5), &s, ARG(7), ARG(8), ARG(9),
                     ARG(10), ARG(11));
    return 0;
}

static int n_drawLine(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (gfx_target(vm, a[0], &e))
        line_core(vm, &e, ARG(1), ARG(2), ARG(3), ARG(4), cur_color(vm, a[0]));
    return 0;
}

static int n_drawTriangle(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (gfx_target(vm, a[0], &e)) {
        int32_t xs[3] = {ARG(1), ARG(3), ARG(5)}, ys[3] = {ARG(2), ARG(4), ARG(6)};
        polyline(vm, &e, 3, xs, ys, cur_color(vm, a[0]));
    }
    return 0;
}

static int n_drawRect(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (gfx_target(vm, a[0], &e)) {
        int32_t x = ARG(1), y = ARG(2), w = ARG(3), h = ARG(4);
        int32_t xs[4] = {x, x + w, x + w, x}, ys[4] = {y, y, y + h, y + h};
        polyline(vm, &e, 4, xs, ys, cur_color(vm, a[0]));
    }
    return 0;
}

static int n_drawChars(ExenVM *vm, uint32_t *a)
{
    ref_t arr = a[1];
    int32_t off = ARG(2), len = ARG(3);
    if (!arr) {
        vm_halt(vm, "Graphics.drawChars: null array");
        return 0;
    }
    int32_t n = arr_len(vm, arr);
    if (n < off || n < len || n < off + len || (off | len) < 0) {
        vm_halt(vm, "Graphics.drawChars: offset/length out of range");
        return 0;
    }
    EXimg e;
    if (!gfx_target(vm, a[0], &e)) {
        if (!e.self)
            vm_halt(vm, "Graphics.drawChars: no target image");
        return 0;
    }
    /* copy: draw_text reads while it writes the image; arrays never alias images here but the
     * copy keeps the pointer valid whatever the GC does (it doesn't run here) */
    uint8_t buf[256];
    const uint8_t *src = arr_data(vm, arr) + off;
    uint8_t *tmp = len <= (int32_t)sizeof buf ? buf : malloc((size_t)len);
    if (!tmp)
        return 0;
    memcpy(tmp, src, (size_t)len);
    eximg_draw_text(vm, &e, ARG(4), ARG(5), tmp, len, cur_color(vm, a[0]));
    if (tmp != buf)
        free(tmp);
    return 0;
}

static int n_fillTriangle(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (gfx_target(vm, a[0], &e))
        fill_triangle(vm, &e, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), cur_color(vm, a[0]));
    return 0;
}

static int n_fillTextureTriangle(ExenVM *vm, uint32_t *a)
{
    EXimg e, s;
    ref_t tm = a[7];
    int ct = plat_class(vm, PC_TEXTUREMAP);
    if (!tm || ct < 0 || !gfx_target(vm, a[0], &e))
        return 0;
    const uint32_t *f = inst(vm, tm, ct);
    ref_t pu = f[TM_PTU], pv = f[TM_PTV];
    if (!is_array_of(vm, pu, T_INT) || !is_array_of(vm, pv, T_INT) ||
        arr_len(vm, pu) != arr_len(vm, pv) || arr_len(vm, pu) < 3)
        return 0;
    if (!eximg_get(vm, f[TM_PIXIMAGE], &s))
        return 0;
    const uint8_t *U = arr_data(vm, pu), *V = arr_data(vm, pv);
    TexV p[3];
    for (int i = 0; i < 3; i++) {
        p[i].x = ARG(1 + 2 * i);
        p[i].y = ARG(2 + 2 * i);
        p[i].u = (int32_t)rd32(U + 4 * i);
        p[i].v = (int32_t)rd32(V + 4 * i);
    }
    fill_texture_triangle(vm, &e, p[0], p[1], p[2], &s);
    return 0;
}

static int n_setPixel(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (gfx_target(vm, a[0], &e) && in_clip(&e, ARG(1), ARG(2))) {
        uint32_t len;
        uint8_t *d = eximg_data(vm, &e, &len);
        put_px(d, len, &e, ARG(1), ARG(2), (uint8_t)a[3]);
    }
    return 0;
}

static int n_getPixel(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    int32_t x = ARG(1), y = ARG(2);
    ref_t g = a[0];
    if (!gfx_target(vm, g, &e)) {
        a[0] = (uint32_t)-2;
        return 1;
    }
    if (!in_clip(&e, x, y)) {
        a[0] = (uint32_t)-5;
        return 1;
    }
    uint32_t len;
    const uint8_t *d = eximg_data(vm, &e, &len);
    a[0] = (uint32_t)get_px(d, len, &e, x, y);
    return 1;
}

static int n_setClip(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    ref_t g = a[0];
    if (!g || plat_class(vm, PC_GRAPHICS) < 0 || !eximg_get(vm, gfields(vm, g)[G_PIXIMA], &e))
        return 0;
    Rect r = {0, 0, e.w, e.h};
    rect_clip(&r, ARG(1), ARG(2), ARG(3), ARG(4));
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    uint32_t *f = gfields(vm, g);
    f[G_CLIPX] = (uint32_t)r.x;
    f[G_CLIPY] = (uint32_t)r.y;
    f[G_CLIPW] = (uint32_t)r.w;
    f[G_CLIPH] = (uint32_t)r.h;
    return 0;
}

static int n_setNormalVideo(ExenVM *vm, uint32_t *a) { (void)a; vm->plat->normal_video = 1; return 0; }
static int n_setInverseVideo(ExenVM *vm, uint32_t *a) { (void)a; vm->plat->normal_video = 0; return 0; }

static int n_setColor(ExenVM *vm, uint32_t *a)
{
    ref_t g = a[0];
    EXimg e;
    if (!g || plat_class(vm, PC_GRAPHICS) < 0 || !gfields(vm, g)[G_PIXIMA])
        return 0;
    if (!eximg_get(vm, gfields(vm, g)[G_PIXIMA], &e))
        return 0;
    uint32_t rgb = (a[1] << 16) | (a[2] << 8) | a[3]; /* not masked: components > 255 bleed */
    uint32_t idx;
    if (e.pal) {
        uint32_t best = 0xffffffffu;
        idx = 0;
        int r = (int)((rgb >> 16) & 0xff), gg = (int)((rgb >> 8) & 0xff), b = (int)(rgb & 0xff);
        for (uint32_t i = 0; i < e.palCount; i++) {
            uint32_t p = pal_get(vm, &e, i);
            int dr = r - (int)((p >> 16) & 0xff), dg = gg - (int)((p >> 8) & 0xff),
                db = b - (int)(p & 0xff);
            uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
            if (best == 0xffffffffu) {
                best = d;
            } else if (d < best) {
                best = d;
                idx = i;
            }
        }
    } else if (e.ops == 4) {
        idx = 0xffffffffu; /* ops4 rgbToColor */
    } else {
        idx = rgb332(rgb);
    }
    gfields(vm, g)[G_COLOR] = idx;
    return 0;
}

/* ------------------------------------------------------------ Image natives */

static uint32_t *ifields(ExenVM *vm, ref_t img) { return inst(vm, img, plat_class(vm, PC_IMAGE)); }

/* The Palette object of an image and its colorTabRGBA, or 0. */
static ref_t image_palette(ExenVM *vm, ref_t img, ref_t *tab)
{
    *tab = 0;
    int cp = plat_class(vm, PC_PALETTE);
    ref_t p = ifields(vm, img)[I_PALETTE];
    if (!p || cp < 0)
        return 0;
    ref_t t = inst(vm, p, cp)[PAL_TAB];
    if (is_array_of(vm, t, T_INT))
        *tab = t;
    return p;
}

static int n_updateNativePaletteFromJavaPalette(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (!eximg_get(vm, a[0], &e))
        return 0;
    ref_t tab, p = image_palette(vm, a[0], &tab);
    if (tab) {
        e.pal = tab;
        e.palCount = arr_len(vm, tab);
        e.palVersion = p;
        set_blit_mode(&e, 0x40);
    } else {
        e.pal = 0;
        e.palCount = 0;
        e.palVersion = 0;
        if (e.blitMode != 0x20)
            set_blit_mode(&e, 0x10);
    }
    eximg_put(vm, &e);
    return 0;
}

static int n_getNativePaletteSize(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    a[0] = eximg_get(vm, a[0], &e) ? e.palCount : 0;
    return 1;
}

static int n_updateJavaPaletteFromNativePalette(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (!eximg_get(vm, a[0], &e))
        return 0;
    ref_t tab, p = image_palette(vm, a[0], &tab);
    if (!tab)
        return 0; /* the original dereferences null here */
    uint16_t n = arr_len(vm, tab);
    for (uint16_t i = 0; i < n; i++) {
        uint32_t v = pal_get(vm, &e, i);
        wr32(arr_data(vm, tab) + 4u * i, v);
    }
    e.pal = tab;
    e.palCount = n;
    e.palVersion = p;
    eximg_put(vm, &e);
    return 0;
}

static int n_setPaletteAlpha(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (!eximg_get(vm, a[0], &e))
        return 0;
    e.alphaMode = a[1] & 0xff;
    set_blit_mode(&e, e.alphaMode ? 0x30 : (e.pal ? 0x40 : 0x10));
    eximg_put(vm, &e);
    return 0;
}

static int n_setTransparentColor(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (!eximg_get(vm, a[0], &e))
        return 0;
    e.transpIdx = a[1] & 0xff;
    set_blit_mode(&e, e.pal ? 0x50 : 0x20);
    eximg_put(vm, &e);
    return 0;
}

static int n_getTransparentColor(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    a[0] = eximg_get(vm, a[0], &e) ? e.transpIdx : 0;
    return 1;
}

static int n_removeTransparentColor(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (!eximg_get(vm, a[0], &e))
        return 0;
    set_blit_mode(&e, e.pal ? 0x40 : 0x10);
    eximg_put(vm, &e);
    return 0;
}

static int n_getSizeOfEXimgStruct(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = EXIMG_SIZE; return 1; }
static int n_getManufDisplayHeaderSize(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = 0; return 1; }

/* Image.Init(w,h,depth) -> 0x4176c7 and ops.init (0x42b36d / 0x42bb4d). */
static int n_Init(ExenVM *vm, uint32_t *a)
{
    int ci = plat_class(vm, PC_IMAGE);
    if (ci < 0 || !a[0])
        return 0;
    uint32_t *f = ifields(vm, a[0]);
    ref_t imgt = f[I_IMGT], data = f[I_DATA];
    if (!imgt || arr_len(vm, imgt) < EXIMG_SIZE)
        return 0;
    memset(arr_data(vm, imgt), 0, EXIMG_SIZE);
    EXimg e;
    memset(&e, 0, sizeof e);
    e.self = imgt;
    e.data_ref = data;
    int32_t w = ARG(1), h = ARG(2), depth = ARG(3);
    e.depth = depth;
    e.ops = (depth == 4 || depth == 8) ? depth : 0;
    if (e.ops) {
        e.w = w;
        e.h = h;
        e.pitch = depth == 8 ? w : (w + 1) / 2;
        e.size = e.pitch * h;
        e.clipW = w;
        e.clipH = h;
        set_blit_mode(&e, 0x10);
        e.f54 = 1;
        if (depth == 8 && data) {
            uint32_t n = arr_len(vm, data);
            if (e.size > 0)
                memset(arr_data(vm, data), 0xff, (uint32_t)e.size < n ? (size_t)e.size : n);
        }
    } else {
        plat_warn_once(vm, W_NOOPS, "Image.Init: depth %d has no ops table", depth);
    }
    eximg_put(vm, &e);
    return 0;
}

static int n_transformNativePaletteToRawRGB(ExenVM *vm, uint32_t *a)
{
    EXimg e;
    if (!eximg_get(vm, a[0], &e) || !e.palCount)
        return 0;
    uint32_t len;
    uint8_t *d = eximg_data(vm, &e, &len);
    uint32_t n = (uint32_t)e.size < len ? (uint32_t)e.size : len;
    for (uint32_t i = 0; i < n; i++)
        d[i] = (uint8_t)pal_get(vm, &e, d[i]); /* low byte = blue: reproduced literally */
    e.transpIdx = (uint8_t)pal_get(vm, &e, e.transpIdx);
    eximg_put(vm, &e);
    return 0;
}

/* ---- bitmap decode (0x418d0a) */

static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

static int is_png(const uint8_t *s, size_t len) { return len >= 8 && !memcmp(s, png_sig, 8); }

/* bitmapDepth 0x418cdf -> 0x41d987 */
static int bitmap_depth(const uint8_t *s, size_t len, int keep)
{
    if (!is_png(s, len))
        return 4; /* raw 1bpp is reported as 4 */
    size_t p = 8;
    while (p + 8 <= len) {
        uint32_t clen = (uint32_t)s[p] << 24 | (uint32_t)s[p + 1] << 16 | (uint32_t)s[p + 2] << 8 | s[p + 3];
        if (!memcmp(s + p + 4, "IHDR", 4)) {
            if (p + 8 + 10 > len)
                return 8;
            uint8_t bd = s[p + 16], ct = s[p + 17];
            return (ct == 3 && bd == 4 && keep) ? 4 : 8;
        }
        p += 8 + (size_t)clen;
    }
    return 8;
}

/* import1bpp: ops8 0x42bdb3, ops4 0x42b515. Rows are byte aligned, MSB first, bit 1 = white. */
static void import_1bpp(ExenVM *vm, EXimg *e, const uint8_t *src, size_t slen, int keep)
{
    uint32_t len;
    uint8_t *d = eximg_data(vm, e, &len);
    int32_t w = e->w, h = e->h;
    if (w <= 0 || h <= 0)
        return;
    size_t rowb = ((size_t)w + 7) >> 3;
    if (e->ops == 8) {
        e->pitch = w;
        e->size = w * h;
        for (int32_t y = 0; y < h; y++)
            for (int32_t x = 0; x < w; x++) {
                size_t si = (size_t)y * rowb + ((size_t)x >> 3);
                int bit = si < slen ? (src[si] >> (7 - (x & 7))) & 1 : 0;
                uint32_t di = (uint32_t)y * (uint32_t)w + (uint32_t)x;
                if (di < len)
                    d[di] = keep ? (uint8_t)bit : (bit ? 0xff : 0x00);
            }
    } else if (e->ops == 4) {
        static const uint8_t tk[4] = {0x00, 0x01, 0x10, 0x11}, tr[4] = {0x00, 0x0f, 0xf0, 0xff};
        const uint8_t *tab = keep ? tk : tr;
        e->pitch = (w + 1) >> 1;
        e->size = e->pitch * h;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x += 2) {
                size_t si = (size_t)y * rowb + ((size_t)x >> 3);
                int two = si < slen ? (src[si] >> (6 - (x & 7))) & 3 : 0;
                uint32_t di = (uint32_t)y * (uint32_t)e->pitch + ((uint32_t)x >> 1);
                if (di < len)
                    d[di] = tab[two];
            }
        }
    } else {
        return;
    }
    if (keep) {
        e->pal = EXIMG_PAL_NATIVE;
        e->palCount = 2;
        pal_set_native(vm, e, 0, 0x00000000u);
        pal_set_native(vm, e, 1, 0xffffffffu);
        set_blit_mode(e, 0x40);
    } else {
        set_blit_mode(e, 0x10);
    }
    e->f54 = 1;
}

/* decodePng 0x41dacd, on top of codec.c's container parser and codecs. */
static void decode_png(ExenVM *vm, EXimg *e, const uint8_t *png, size_t len, int keep)
{
    uint8_t *buf = malloc(len + 6);
    if (!buf)
        return;
    memset(buf, 0, 6);
    memcpy(buf + 6, png, len);
    ExenDecodedImage im;
    int rc = exen_image_decode(buf, len + 6, &im);
    free(buf);
    if (rc == EXEN_CODEC_ERR_UNSUPPORTED && im.color_type == 2) {
        static const uint8_t msg[] = "True color PNG not supported";
        eximg_draw_text(vm, e, 0, 0, msg, 23, 0);
        plat_warn_once(vm, W_TRUECOLOR, "image decode: truecolour PNG not supported");
        return;
    }
    if (rc != EXEN_CODEC_OK) {
        plat_warn_once(vm, W_CODEC, "image decode failed (codec error %d); image left blank", rc);
        return;
    }
    uint32_t w = im.width, h = im.height, bd = im.bit_depth;
    /* IHDR */
    e->depth = keep ? (int32_t)bd : 8;
    e->ops = (e->depth == 4 || e->depth == 8) ? e->depth : 0;
    e->pitch = (int32_t)((bd == 4 && keep) ? (w + 1) >> 1 : w);
    e->size = e->pitch * (int32_t)h;
    set_blit_mode(e, 0x10);
    /* PLTE */
    uint8_t lut332[256];
    int have_lut = 0;
    if (im.palette_count) {
        uint32_t n = im.palette_count;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t r = im.palette[3 * i], g = im.palette[3 * i + 1], b = im.palette[3 * i + 2];
            if (keep)
                pal_set_native(vm, e, i, 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | b);
            else
                lut332[i] = (uint8_t)((r & 0xe0) | ((g & 0xe0) >> 3) | ((b >> 6) & 3));
        }
        if (keep) {
            e->pal = EXIMG_PAL_NATIVE;
            e->palCount = n;
            set_blit_mode(e, 0x40);
        } else {
            have_lut = 1;
            set_blit_mode(e, 0x10);
        }
    }
    /* tRNS */
    if (im.has_trns) {
        uint32_t mode = 0x20;
        if (keep) {
            int zeros = 0;
            for (uint32_t i = 0; i < im.alpha_count; i++) {
                uint32_t av = im.alpha[i];
                if (av == 0xff)
                    continue;
                if (av == 0) {
                    e->transpIdx = i;
                    zeros++;
                }
                /* sic: the alpha is ORed into the blue byte and the alpha byte is cleared */
                pal_set_native(vm, e, i, (pal_get(vm, e, i) & 0x00ffffffu) | av);
            }
            e->alphaMode = zeros > 1;
            mode = e->alphaMode ? 0x30 : 0x50;
        } else {
            for (uint32_t i = 0; i < im.alpha_count; i++)
                if (im.alpha[i] == 0)
                    e->transpIdx = have_lut && i < im.palette_count ? lut332[i] : 0;
        }
        set_blit_mode(e, mode);
    }
    /* IDAT */
    uint32_t dlen;
    uint8_t *d = eximg_data(vm, e, &dlen);
    if (keep) {
        size_t n = im.raw_len < dlen ? im.raw_len : dlen;
        memcpy(d, im.raw, n);
    } else if (bd == 8 || bd == 4) {
        size_t n = (size_t)w * h;
        if (n > dlen)
            n = dlen;
        for (size_t i = 0; i < n; i++)
            d[i] = have_lut ? lut332[im.pixels[i]] : im.pixels[i];
    } else {
        size_t n = (size_t)w * h < dlen ? (size_t)w * h : dlen;
        memcpy(d, im.pixels, n);
    }
    exen_image_free(&im);
}

static void decode_bitmap(ExenVM *vm, ref_t image, const uint8_t *src, size_t len, int keep)
{
    EXimg e;
    if (!eximg_get(vm, image, &e))
        return;
    if (is_png(src, len)) {
        decode_png(vm, &e, src, len, keep);
    } else {
        plat_warn_once(vm, W_1BPP, "image decode: raw 1bpp bitmap");
        import_1bpp(vm, &e, src, len, keep);
    }
    eximg_put(vm, &e);
}

/* The bytes at a Resource's cursor, to the end of the file: the simulator reads straight from
 * the open .exn, so a decoder that runs past the resource reads into the next one. */
static const uint8_t *resource_cursor(ExenVM *vm, ref_t res, size_t *len)
{
    int cr = plat_class(vm, PC_RESOURCE);
    *len = 0;
    if (!res || cr < 0 || !vm->exn)
        return NULL;
    const uint32_t *f = inst(vm, res, cr);
    uint64_t pos = (uint64_t)f[R_START] + f[R_OFFSET];
    if (pos >= vm->exn_len)
        return NULL;
    *len = vm->exn_len - (size_t)pos;
    return vm->exn + pos;
}

static int n_TransformBitmapFromRes(ExenVM *vm, uint32_t *a)
{
    size_t len;
    const uint8_t *s = resource_cursor(vm, a[1], &len);
    if (!s)
        return 0;
    if (!is_png(s, len)) {
        /* raw 1bpp from the file: ((w+7)>>3)*h bytes at the cursor */
        EXimg e;
        if (eximg_get(vm, a[0], &e)) {
            size_t n = (size_t)(((int64_t)e.w + 7) >> 3) * (size_t)(e.h > 0 ? e.h : 0);
            if (n < len)
                len = n;
        }
    }
    decode_bitmap(vm, a[0], s, len, (int)(a[2] & 0xff));
    return 0;
}

static int n_TransformBitmapFromBytes(ExenVM *vm, uint32_t *a)
{
    ref_t arr = a[1];
    if (!arr)
        return 0;
    size_t len = arr_len(vm, arr);
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy)
        return 0;
    memcpy(copy, arr_data(vm, arr), len);
    decode_bitmap(vm, a[0], copy, len, (int)(a[2] & 0xff));
    free(copy);
    return 0;
}

static int n_GetBitmapDepthFromRes(ExenVM *vm, uint32_t *a)
{
    size_t len;
    const uint8_t *s = resource_cursor(vm, a[0], &len);
    a[0] = s ? (uint32_t)bitmap_depth(s, len, (int)(a[1] & 0xff)) : 4;
    return 1;
}

static int n_GetBitmapDepthFromBytes(ExenVM *vm, uint32_t *a)
{
    ref_t arr = a[0];
    a[0] = arr ? (uint32_t)bitmap_depth(arr_data(vm, arr), arr_len(vm, arr), (int)(a[1] & 0xff)) : 4;
    return 1;
}

/* ------------------------------------------------------------ registration */

void graphics_register(void)
{
    natives_register(0, n_clearRect);
    natives_register(1, n_drawImage);
    natives_register(2, n_drawLine);
    natives_register(3, n_drawTriangle);
    natives_register(4, n_drawRect);
    natives_register(5, n_drawChars);
    natives_register(6, n_fillTriangle);
    natives_register(7, n_fillRect);
    natives_register(8, n_fillTextureTriangle);
    natives_register(9, n_setPixel);
    natives_register(10, n_getPixel);
    natives_register(11, n_setClip);
    natives_register(12, n_setNormalVideo);
    natives_register(13, n_setInverseVideo);
    natives_register(14, n_setColor);
    natives_register(15, n_updateNativePaletteFromJavaPalette);
    natives_register(16, n_getNativePaletteSize);
    natives_register(17, n_updateJavaPaletteFromNativePalette);
    natives_register(18, n_setPaletteAlpha);
    natives_register(19, n_setTransparentColor);
    natives_register(20, n_getTransparentColor);
    natives_register(21, n_removeTransparentColor);
    natives_register(22, n_getSizeOfEXimgStruct);
    natives_register(23, n_getManufDisplayHeaderSize);
    natives_register(24, n_Init);
    natives_register(25, n_transformNativePaletteToRawRGB);
    natives_register(26, n_TransformBitmapFromRes);
    natives_register(27, n_GetBitmapDepthFromRes);
    natives_register(28, n_TransformBitmapFromBytes);
    natives_register(29, n_GetBitmapDepthFromBytes);
}
