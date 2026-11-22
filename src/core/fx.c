/* exen.FX natives (103-108): rotozoom, mosaic, shutters and per-line shifts.
 *
 * Translated from gensimu.exe (docs/re/natives/FX.c.txt, docs/re/NATIVES.md §2.3). Each native
 * resolves dst = EXimg(g.pixIma) and src = EXimg(image) and calls entry k of the *source*
 * image's ops->fx table as fx[k](src, dst, ...). Only the 8bpp ops table has real effects
 * (0x456018: 0x415b6a, 0x415fc6, 0x4162d0, 0x416715, 0x41688d, 0x416b75); the 4bpp table points
 * at empty stubs (0x457f48), so FX on a 4bpp source does nothing. A null image (or a null
 * offsets array for the shifts) is logged and is a fatal interrupt 0.
 *
 * The effects copy raw bytes: no palette LUT, no transparency, no paint mode. Rotazoom and
 * mosaic ignore the Graphics clip and write the whole destination; the shutters and shifts
 * call syncClip first and use parts of the destination clip (and, quirk, the *source* image's
 * stored clip size in the shifts). Pointer arithmetic is kept as in the original, including
 * rows that run past the right edge into the next row; only accesses outside the pixel arrays
 * are dropped (reads give 0), where the original would touch neighbouring memory.
 */
#include <string.h>

#include "extnatives.h"

enum { G_PIXIMA = 0 };

typedef struct {
    uint8_t *p;
    uint32_t len;
} Buf;

static inline uint8_t bget(const Buf *b, int64_t i) { return i >= 0 && i < b->len ? b->p[i] : 0; }
static inline void bput(Buf *b, int64_t i, uint8_t v)
{
    if (i >= 0 && i < b->len)
        b->p[i] = v;
}
static void bcopy_(Buf *d, int64_t di, const Buf *s, int64_t si, int32_t n)
{
    for (int32_t k = 0; k < n; k++)
        bput(d, di + k, bget(s, si + k));
}

static int32_t wmul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static int32_t iabs(int32_t v) { return v < 0 ? (int32_t)(0u - (uint32_t)v) : v; }
/* C (and x86 idiv) truncating division; callers never pass 0 */
static int32_t idiv(int32_t a, int32_t b)
{
    if (b == -1)
        return (int32_t)(0u - (uint32_t)a);
    return a / b;
}

/* Resolve the two images like the wrappers 0x424b70.. do. Returns 0 (after halting) when either
 * image is missing, -1 when the effect is a no-op (source without the 8bpp ops table). */
static int fx_images(ExenVM *vm, ref_t g, ref_t img, EXimg *d, EXimg *s, const char *what)
{
    int cg = plat_class(vm, PC_GRAPHICS);
    ref_t target = g && cg >= 0 ? inst(vm, g, cg)[G_PIXIMA] : 0;
    if (!eximg_get(vm, img, s) || !eximg_get(vm, target, d)) {
        vm_log(vm, "FX.%s: null image", what);
        vm_halt(vm, "FX.%s: null image (non catchable interrupt 0)", what);
        return 0;
    }
    return s->ops == 8 ? 1 : -1;
}

static void fx_buf(ExenVM *vm, const EXimg *e, Buf *b) { b->p = eximg_data(vm, e, &b->len); }

/* ------------------------------------------------------------ rotozoom (fx[0] 0x415b6a) */

static void rotozoom(ExenVM *vm, EXimg *S, EXimg *D, int32_t angle, int32_t zoom, int32_t cx,
                     int32_t cy)
{
    int32_t c = exen_cos(vm, angle), s = exen_sin(vm, angle);
    int32_t z = wadd(zoom, 0x10000) >> 8;
    c = wmul(c >> 8, z);
    s = wmul(s >> 8, z);
    int32_t hh = idiv(-D->h, 2), hw = idiv(-D->w, 2);
    /* 0xffff, not 0x10000: the centre lands one source pixel up-left of (cx, cy) */
    int32_t U = wmul(wadd(wadd(cx, S->w >> 1), wmul(S->w, 2)), 0xffff);
    int32_t V = wmul(wadd(wadd(cy, S->h >> 1), wmul(S->h, 2)), 0xffff);
    int32_t u0 = wadd(wadd(wmul(hh, s), wmul(hw, c)), U);
    int32_t v0 = wsub(wmul(hh, c), wsub(wmul(hw, s), V));
    Buf sb, db;
    fx_buf(vm, S, &sb);
    fx_buf(vm, D, &db);
    int64_t di = 0;
    int32_t sw = S->w, sh = S->h;
    int pow2 = 0;
    for (uint32_t m = 1; m; m <<= 1) /* 0x41c928 */
        if ((uint32_t)sw == m) {
            for (uint32_t k = 1; k; k <<= 1)
                if ((uint32_t)sh == k)
                    pow2 = 1;
            break;
        }
    if (!pow2 && (sw <= 0 || sh <= 0))
        return; /* the original's wrap loops would never end */
    for (int32_t row = D->h; row > 0; row--) {
        int32_t u = u0, v = v0;
        u0 = wadd(u0, s);
        v0 = wadd(v0, c);
        for (int32_t n = D->pitch; n > 0; n--) {
            int32_t x = u >> 16, y = v >> 16;
            int64_t si;
            if (pow2) { /* 0x415d19: wrap with masks, row stride w */
                x &= sw - 1;
                y &= sh - 1;
                si = (int64_t)y * sw + x;
            } else { /* 0x415e5d: wrap by repeated subtraction, row stride w */
                x %= sw;
                if (x < 0) x += sw;
                y %= sh;
                if (y < 0) y += sh;
                si = (int64_t)y * sw + x;
            }
            u = wadd(u, c);
            v = wsub(v, s);
            bput(&db, di++, bget(&sb, si));
        }
    }
}

static int n_doRotazoom(ExenVM *vm, uint32_t *a)
{
    EXimg d, s;
    if (fx_images(vm, a[0], a[1], &d, &s, "doRotazoom") > 0)
        rotozoom(vm, &s, &d, (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], (int32_t)a[5]);
    return 0;
}

/* ------------------------------------------------------------ mosaic (fx[1] 0x415fc6) */

static void mosaic(ExenVM *vm, EXimg *S, EXimg *D, int32_t x, int32_t y, int32_t level)
{
    int32_t W = S->w - iabs(x) < D->w ? S->w - iabs(x) : D->w;
    int32_t sw = S->w, sh = S->h;
    int32_t dskip = D->pitch - W;
    if (level < 16 || iabs(x) > D->w || iabs(y) > D->h)
        return;
    if (sw == 0 || sh == 0)
        return; /* divide by zero in the original */
    int32_t du = idiv(idiv(wmul(sw, 0x10000), level), sw);
    int32_t dv = idiv(idiv(wmul(sh, 0x10000), level), sh);
    int32_t U = wmul(du, sw) >> 12, V = wmul(dv, sh) >> 12;
    int32_t ex = idiv(wmul(wmul(sw, 0x10000), level - 16), sw) >> 8;
    int32_t ey = idiv(wmul(wmul(sh, 0x10000), level - 16), sh) >> 8;
    U = wadd(U, wmul(U, ex) >> 12);
    U = (int32_t)((uint32_t)wsub(sw, U) << 11);
    V = wadd(V, wmul(V, ey) >> 12);
    V = (int32_t)((uint32_t)wsub(sh, V) << 11);
    int32_t H = S->h - iabs(y) < D->h ? S->h - iabs(y) : D->h;
    if (y < 0) y = 0;
    if (x < 0) x = 0;
    Buf sb, db;
    fx_buf(vm, S, &sb);
    fx_buf(vm, D, &db);
    int64_t di = (int64_t)x + (int64_t)y * D->pitch;
    int32_t ay = 0;
    while (H-- > 0) {
        int32_t u = U, acc = 0;
        int64_t row = (int64_t)(V >> 12) * S->pitch;
        for (int32_t n = W; n > 0; n--) {
            bput(&db, di++, bget(&sb, row + (u >> 12)));
            u = wadd(u, du);
            acc = wadd(acc, du);
            if (acc >> 12) {
                u = wadd(u, ex);
                acc &= 0xfff;
            }
        }
        di += dskip;
        V = wadd(V, dv);
        ay = wadd(ay, dv);
        if (ay >> 12) {
            V = wadd(V, ey);
            ay &= 0xfff;
        }
    }
}

static int n_doMosaic(ExenVM *vm, uint32_t *a)
{
    EXimg d, s;
    if (fx_images(vm, a[0], a[1], &d, &s, "doMosaic") > 0)
        mosaic(vm, &s, &d, (int32_t)a[2], (int32_t)a[3], (int32_t)a[4]);
    return 0;
}

/* ------------------------------------------------------------ shutters */

/* 0x4165f5: how many lines of each group of 8 are revealed, from `step` (an angle in the
 * 2048-unit circle, decreasing by 0x40 per group). */
static void shutter_table(ExenVM *vm, int32_t *tab, uint32_t n, int32_t step, int32_t dir)
{
    int32_t a = step;
    if (dir == 0) {
        a = wadd(a, 0x200);
        for (; n; n--, a = wsub(a, 0x40)) {
            if (a < 0x200) {
                *tab++ = 8;
            } else if (a < 0x600) {
                int32_t s = exen_sin(vm, a);
                if (s < 0) s = 0;
                *tab++ = wmul(s, 9) >> 16;
            } else {
                *tab++ = 0;
            }
        }
    } else {
        for (; n; n--, a = wsub(a, 0x40)) {
            if (a < 0x200) {
                int32_t s = exen_sin(vm, a);
                if (s < 0) s = 0;
                *tab++ = wmul(s, 9) >> 16;
            } else {
                *tab++ = 8;
            }
        }
    }
}

/* The original keeps the table in a 64-entry stack array and indexes it & 63; wider images
 * would overrun it. We size it for the image and index it the same way. */
#define SHUTTER_MAX_GROUPS 4096

/* fx[2] 0x4162d0: columns in groups of 8, from the data origin (the clip only bounds the size). */
static void shutter_vertical(ExenVM *vm, EXimg *S, EXimg *D, int32_t step, int32_t dir)
{
    static int32_t tab[SHUTTER_MAX_GROUPS];
    int32_t W = S->w < D->clipW ? S->w : D->clipW;
    int32_t H = S->h < D->clipH ? S->h : D->clipH;
    uint32_t groups = (uint32_t)W >> 3;
    uint32_t rem = (uint32_t)W - (groups << 3);
    int32_t sback = wmul(H, S->pitch) - 1, dback = wmul(H, D->pitch) - 1;
    Buf sb, db;
    fx_buf(vm, S, &sb);
    fx_buf(vm, D, &db);
    uint32_t nt = groups < SHUTTER_MAX_GROUPS ? groups : SHUTTER_MAX_GROUPS;
    memset(tab, 0, sizeof tab);
    shutter_table(vm, tab, nt, step, dir);
    int64_t sp = 0, dp = 0, s = 0, d = 0;
    int have = 0; /* s/d start out as null pointers in the original */
    uint32_t idx = 0;
    int32_t g = (int32_t)groups;
    while (g > 0) {
        s = sp;
        d = dp;
        have = 1;
        sp += 8;
        dp += 8;
        int32_t k = tab[idx];
        idx = (idx + 1) & 0x3f;
        g--;
        for (; k > 0; k--) {
            for (int32_t r = H; r > 0; r--) {
                bput(&db, d, bget(&sb, s));
                s += S->pitch;
                d += D->pitch;
            }
            s -= sback;
            d -= dback;
        }
    }
    if (rem && have) {
        /* quirk: continues from where the last group stopped, with tab[idx] - rem columns */
        for (int32_t k = tab[idx] - (int32_t)rem; k > 0; k--) {
            for (int32_t r = H; r > 0; r--) {
                bput(&db, d, bget(&sb, s));
                s += S->pitch;
                d += D->pitch;
            }
            s -= sback;
            d -= dback;
        }
    }
}

/* fx[3] 0x416715: rows in groups of 8, whole source rows, no clipping at all. */
static void shutter_horizontal(ExenVM *vm, EXimg *S, EXimg *D, int32_t step, int32_t dir)
{
    static int32_t tab[SHUTTER_MAX_GROUPS];
    int32_t groups = S->h / 8;
    Buf sb, db;
    fx_buf(vm, S, &sb);
    fx_buf(vm, D, &db);
    uint32_t ng = groups > 0 ? (uint32_t)groups : 0;
    if (ng > SHUTTER_MAX_GROUPS)
        ng = SHUTTER_MAX_GROUPS;
    memset(tab, 0, sizeof tab);
    shutter_table(vm, tab, ng, step, dir);
    int64_t sp = 0, dp = 0;
    for (uint32_t gi = 0; gi < ng; gi++) {
        uint32_t k = (uint32_t)tab[gi];
        int64_t s = sp, d = dp;
        for (; k; k--) {
            bcopy_(&db, d, &sb, s, S->pitch);
            s += S->pitch;
            d += D->pitch;
        }
        sp += (int64_t)S->pitch * 8;
        dp += (int64_t)D->pitch * 8;
    }
}

static int shutter(ExenVM *vm, uint32_t *a, int vertical)
{
    EXimg d, s;
    int r = fx_images(vm, a[0], a[1], &d, &s, vertical ? "doShutterVertical" : "doShutterHorizontal");
    if (!r)
        return 0;
    eximg_sync_clip(vm, a[0], &d);
    eximg_put(vm, &d);
    if (r < 0)
        return 0;
    if (vertical)
        shutter_vertical(vm, &s, &d, (int32_t)a[2], (int32_t)a[3]);
    else
        shutter_horizontal(vm, &s, &d, (int32_t)a[2], (int32_t)a[3]);
    return 0;
}

static int n_doShutterVertical(ExenVM *vm, uint32_t *a) { return shutter(vm, a, 1); }
static int n_doShutterHorizontal(ExenVM *vm, uint32_t *a) { return shutter(vm, a, 0); }

/* ------------------------------------------------------------ shifts */

/* The offsets array is read one signed byte per line without a length check; past its end we
 * read 0 (the original reads whatever follows it in memory). */
static int32_t off_at(const Buf *o, uint32_t i) { return (int8_t)bget(o, i); }

/* fx[4] 0x41688d: each row r of the source moves by offs[r] + x. */
static void shift_horizontal(ExenVM *vm, EXimg *S, EXimg *D, int32_t x, int32_t y, const Buf *offs)
{
    Buf sb, db;
    fx_buf(vm, S, &sb);
    fx_buf(vm, D, &db);
    int64_t sp = 0, dp = (int64_t)iabs(y) * D->pitch;
    int32_t W = S->w < D->clipW ? S->w : D->clipW;
    int32_t H = S->h < D->clipH ? S->h : D->clipH;
    if (wadd(y, D->clipY) < D->clipY) {
        if (y < D->clipY)
            y = -y;
        sp = (int64_t)y * S->pitch;
        dp = (int64_t)D->clipY * D->pitch;
        H = S->clipH - y < D->clipH ? S->clipH - y : D->clipH;
    } else {
        sp = 0;
        dp = (int64_t)wadd(D->clipY, y) * D->pitch;
        H = S->clipH < D->clipH - y ? S->clipH : D->clipH - y;
    }
    if (iabs(x) > D->w || y > D->h)
        return;
    if (H > S->h)
        H = S->h;
    dp += D->clipX;
    for (int32_t r = 0; r < H; r++) {
        int32_t o = wadd(off_at(offs, (uint32_t)r), x);
        int32_t n = W;
        if (o > 0) {
            int32_t t = D->w - (o + n);
            if (t < 0)
                n += t;
            if (o + n > D->clipW)
                n = D->clipW - o;
            if (n > 0)
                bcopy_(&db, dp + o, &sb, sp, n);
        } else {
            n += o;
            if (n > D->clipW)
                n = D->clipW;
            if (n > 0)
                bcopy_(&db, dp, &sb, sp - o, n);
        }
        sp += S->pitch;
        dp += D->pitch;
    }
}

/* fx[5] 0x416b75: each column c moves by offs[c] + y. */
static void shift_vertical(ExenVM *vm, EXimg *S, EXimg *D, int32_t x, int32_t y, const Buf *offs)
{
    Buf sb, db;
    fx_buf(vm, S, &sb);
    fx_buf(vm, D, &db);
    int64_t s0, d0 = 0;
    int32_t N, dx, start;
    if (x < 0) {
        x = -x;
        s0 = x;
        N = S->clipW - x < D->clipW ? S->clipW - x : D->clipW;
        dx = D->clipX;
        start = x; /* quirk: the source then starts at 2*|x| */
    } else {
        s0 = 0;
        N = S->clipW < D->clipW - x ? S->clipW : D->clipW - x;
        dx = D->clipX + x;
        start = 0;
    }
    uint32_t oi = 0;
    for (int32_t c = start; c < N + start; c++) {
        int32_t o = wadd(wadd(off_at(offs, oi++), D->clipY), y);
        int64_t s = s0 + c, d = d0 + c + dx;
        int32_t n;
        if (o > D->clipY) {
            d += (int64_t)o * D->pitch;
            int32_t lim = D->clipH - o + D->clipY;
            n = S->h < lim ? S->h : lim;
        } else {
            d += (int64_t)D->clipY * D->pitch;
            s += -(int64_t)(o - D->clipY) * S->pitch;
            int32_t lim = S->h + (o - D->clipY);
            n = lim < D->clipH ? lim : D->clipH;
        }
        for (int32_t k = 0; k < n; k++) {
            bput(&db, d, bget(&sb, s));
            s += S->pitch;
            d += D->pitch;
        }
    }
}

static int shift(ExenVM *vm, uint32_t *a, int vertical)
{
    const char *what = vertical ? "doShiftVertical" : "doShiftHorizontal";
    EXimg d, s;
    int r = fx_images(vm, a[0], a[1], &d, &s, what);
    if (!r)
        return 0;
    if (!a[4]) {
        vm_log(vm, "FX.%s: pointer array of offset is null", what);
        vm_halt(vm, "FX.%s: null offsets (non catchable interrupt 0)", what);
        return 0;
    }
    eximg_sync_clip(vm, a[0], &d);
    eximg_put(vm, &d);
    if (r < 0)
        return 0;
    Buf o = {arr_data(vm, a[4]), arr_len(vm, a[4])};
    if (vertical)
        shift_vertical(vm, &s, &d, (int32_t)a[2], (int32_t)a[3], &o);
    else
        shift_horizontal(vm, &s, &d, (int32_t)a[2], (int32_t)a[3], &o);
    return 0;
}

static int n_doShiftHorizontal(ExenVM *vm, uint32_t *a) { return shift(vm, a, 0); }
static int n_doShiftVertical(ExenVM *vm, uint32_t *a) { return shift(vm, a, 1); }

void fx_register(void)
{
    natives_register(103, n_doRotazoom);
    natives_register(104, n_doMosaic);
    natives_register(105, n_doShiftHorizontal);
    natives_register(106, n_doShiftVertical);
    natives_register(107, n_doShutterVertical);
    natives_register(108, n_doShutterHorizontal);
}
