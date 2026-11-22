/* exen.RayCast natives (137-146): the "exrayc" grid ray caster.
 *
 * A faithful translation of gensimu.exe's engine (0x41f0da..0x420ca8), its native wrappers
 * (0x4282b0..0x428a9a) and the 8bpp textured column renderer (ops slot 17, 0x42ccc4); see
 * docs/re/NATIVES.md §2.3 for the walkthrough. The quirks are kept: integer-only fixed point
 * with its truncations, the 0x7fff bias on the first grid intersection, the fisheye correction
 * against the angle of the last draw(), castRay using whatever sine period exen.Math currently
 * has, changeInternalValues changing exen.Math's period as a side effect, the wall nibbles
 * (high nibble = wall met on a horizontal grid line, low nibble = on a vertical one), and
 * sprites drawn in column runs in front of the walls with the mask image never used.
 *
 * Engine state. The original keeps it in RayCast.internal_rayc_values (a byte[452]) and
 * reaches it through a global pointer refreshed from the Java fields by every native
 * (0x4282b0). Layout (u32 little-endian):
 *   +0x00 max grid steps  +0x04 sine period (6*viewW)  +0x08 wall height  +0x0c view width
 *   +0x10 view height  +0x14 column step (1 or 2)  +0x18 rays  +0x1c full circle (= period)
 *   +0x20 view width  +0x24 half FOV (W/2)  +0x28 W/4  +0x2c 90 deg (3W/2)  +0x30 180 deg
 *   +0x34 270 deg  +0x38 0  +0x3c W/12  +0x40 W/6  +0x44 horizon  +0x48 projection distance
 *   +0x4c horizon offset  +0x50 x  +0x54 y (16.16, 64 units per cell)  +0x58 view angle
 *   +0x5c map*  +0x60 map width  +0x64 map height  +0x68 wall EXimg*[16]  +0xa8 wall log2
 *   widths*  +0xac slices*  +0xb0 rays  +0xb4 max sprites  +0xb8/+0xbc/+0xc0 -> the three
 *   0x54-byte nodes at +0xc8 (sprite blit), +0x11c (mask blit), +0x170 (list head)
 *   +0xc4 sprite list*
 * We keep the integer fields in the byte[] at the same offsets. The pointer fields are not
 * stored: the buffers are resolved from the Java fields on every native call, as the original
 * re-resolves them.
 *
 * A sprite (RayCast.internal_sprite_list, 84 bytes each):
 *   +0x00 active  +0x04 x +0x08 y +0x0c z  +0x10 w +0x14 h (world size)
 *   +0x18 sx +0x1c sy +0x20 sw +0x24 sh (rect in the sprite image) +0x28 mx +0x2c my (mask)
 *   +0x30 sprite image  +0x34 mask image  +0x38 screen x  +0x3c screen y  +0x40 depth
 *   +0x44 screen w  +0x48 screen h  +0x4c prev  +0x50 next (depth-sorted draw list)
 * The image slots hold exen.Image references (the original: EXimg pointers), refreshed from
 * spriteImages/maskImages before every use. The list links hold node ids (0 = none, i+1 =
 * sprite i, LIST_HEAD = the head node) instead of pointers; they only live during a draw.
 *
 * Buffers are bounds checked: reads outside give 0 and writes are dropped, where the original
 * would touch neighbouring memory. Divisions the original would fault on are skipped.
 */
#include <string.h>

#include "extnatives.h"

/* exen.RayCast instance slots */
enum { RC_WALLS, RC_WALLPOW2, RC_MAXSPR, RC_SPRIMG, RC_MASKIMG, RC_VALUES, RC_SPRLIST, RC_SLICES,
       RC_MAP, RC_MAPW, RC_MAPH };

#define NODE_A 0xc8u    /* sprite blit parameters */
#define NODE_B 0x11cu   /* mask blit parameters (computed, never drawn) */
#define NODE_HEAD 0x170u
#define SPR_SIZE 0x54u
#define LIST_HEAD 0x7ffffff0u

typedef struct {
    uint8_t *p;
    uint32_t len;
} Mem;

typedef struct {
    ExenVM *vm;
    ref_t self;
    Mem r, map, slices, spr, pow2;
    EXimg walls[16];
    int wall_ok[16];
} RC;

static int32_t m32(const Mem *m, uint32_t off)
{
    return m->p && off <= m->len && m->len - off >= 4 ? (int32_t)rd32(m->p + off) : 0;
}
static void s32(Mem *m, uint32_t off, int32_t v)
{
    if (m->p && off <= m->len && m->len - off >= 4)
        wr32(m->p + off, (uint32_t)v);
}
static uint8_t m8(const Mem *m, int64_t off) { return m->p && off >= 0 && off < m->len ? m->p[off] : 0; }
static void s8(Mem *m, int64_t off, uint8_t v)
{
    if (m->p && off >= 0 && off < m->len)
        m->p[off] = v;
}

static int32_t wadd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t wsub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static int32_t wmul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static int32_t wneg(int32_t a) { return (int32_t)(0u - (uint32_t)a); }
static int32_t shl16(int32_t a) { return (int32_t)((uint32_t)a << 16); }
static int32_t idiv(int32_t a, int32_t b)
{
    if (b == 0)
        return 0; /* a divide fault in the original; every caller excludes it first */
    if (b == -1)
        return wneg(a);
    return a / b;
}

#define R(o) m32(&rc->r, (o))
static int32_t R_max(RC *rc) { return m32(&rc->r, 0xb4); }
#define RS(o, v) s32(&rc->r, (o), (v))

/* exen.Math getSinOfPeriod / getCosOfPeriod (0x41cb91 / 0x41cbb2) */
static int32_t psin(RC *rc, int32_t x)
{
    ExenVM *vm = rc->vm;
    return exen_sin(vm, (int32_t)((uint32_t)x << 11) / vm->math.period);
}
static int32_t pcos(RC *rc, int32_t x)
{
    ExenVM *vm = rc->vm;
    return exen_cos(vm, (int32_t)((uint32_t)x << 11) / vm->math.period);
}
static void set_period(ExenVM *vm, int32_t p) /* 0x41cb68 */
{
    if (p > 0)
        vm->math.period = p;
}

static Mem arr_mem(ExenVM *vm, ref_t a)
{
    Mem m = {NULL, 0};
    if (a && heap_is_object(vm, a) && is_array(vm, a)) {
        m.p = arr_data(vm, a);
        m.len = (uint32_t)arr_len(vm, a) * (uint32_t)elem_size(arr_type(vm, a));
    }
    return m;
}

static uint32_t *rc_fields(ExenVM *vm, ref_t self) { return inst(vm, self, ext_class(vm, EC_RAYCAST)); }

/* 0x4282b0: point the engine at this RayCast's buffers. 0 on failure (already halted). */
static int refresh(ExenVM *vm, ref_t self, RC *rc)
{
    memset(rc, 0, sizeof *rc);
    rc->vm = vm;
    rc->self = self;
    if (!self || ext_class(vm, EC_RAYCAST) < 0) {
        vm_halt(vm, "RayCast: no instance");
        return 0;
    }
    const uint32_t *f = rc_fields(vm, self);
    rc->r = arr_mem(vm, f[RC_VALUES]);
    if (!rc->r.p) {
        vm_log(vm, "RayCast: internal values are null");
        vm_halt(vm, "RayCast: internal values are null (non catchable interrupt 0x27)");
        return 0;
    }
    rc->spr = arr_mem(vm, f[RC_SPRLIST]);
    RS(0xb4, (int32_t)f[RC_MAXSPR]);
    RS(0x60, (int32_t)f[RC_MAPW]);
    RS(0x64, (int32_t)f[RC_MAPH]);
    rc->map = arr_mem(vm, f[RC_MAP]);
    rc->slices = arr_mem(vm, f[RC_SLICES]);
    return 1;
}

/* images[i] (the original takes its EXimg, 0x42846c); 0 if the array or the element is missing */
static ref_t image_at(ExenVM *vm, ref_t arr, int32_t i)
{
    if (!arr || i < 0 || i >= (int32_t)arr_len(vm, arr))
        return 0;
    return rd32(arr_data(vm, arr) + 4u * (uint32_t)i);
}

/* ------------------------------------------------------------ sprites */

static uint32_t node_off(RC *rc, uint32_t id, Mem **m)
{
    if (id == LIST_HEAD) {
        *m = &rc->r;
        return NODE_HEAD;
    }
    *m = &rc->spr;
    return (id - 1) * SPR_SIZE;
}
static int32_t N(RC *rc, uint32_t id, uint32_t off)
{
    Mem *m;
    uint32_t b = node_off(rc, id, &m);
    return m32(m, b + off);
}
static void NS(RC *rc, uint32_t id, uint32_t off, int32_t v)
{
    Mem *m;
    uint32_t b = node_off(rc, id, &m);
    s32(m, b + off, v);
}

/* 0x420150: the sprite entry, or 0 (logged) */
static uint32_t sprite_id(RC *rc, int32_t i)
{
    if (i < 0 || i >= R(0xb4)) {
        vm_log(rc->vm, "RayCast: sprite index out of range");
        return 0;
    }
    if (!rc->spr.p) {
        vm_log(rc->vm, "RayCast: no sprite list");
        return 0;
    }
    return (uint32_t)i + 1;
}

static void insert_before(RC *rc, uint32_t n, uint32_t p) /* 0x420471 */
{
    uint32_t t = (uint32_t)N(rc, p, 0x4c);
    if (t)
        NS(rc, t, 0x50, (int32_t)n);
    NS(rc, n, 0x4c, (int32_t)t);
    NS(rc, n, 0x50, (int32_t)p);
    NS(rc, p, 0x4c, (int32_t)n);
}

static void insert_after(RC *rc, uint32_t n, uint32_t p) /* 0x4204c1 */
{
    uint32_t t = (uint32_t)N(rc, p, 0x50);
    NS(rc, p, 0x50, (int32_t)n);
    NS(rc, n, 0x4c, (int32_t)p);
    NS(rc, n, 0x50, (int32_t)t);
    if (t)
        NS(rc, t, 0x4c, (int32_t)n);
}

static int valid_node(RC *rc, uint32_t id)
{
    return id == LIST_HEAD || (id >= 1 && (uint64_t)id * SPR_SIZE <= rc->spr.len);
}

/* 0x42055c: project the active sprites and sort the visible ones far to near */
static void project_sprites(RC *rc)
{
    if (!rc->spr.p)
        return;
    int32_t nmax = R(0xb4);
    for (int32_t i = 0; i < nmax; i++) {
        uint32_t s = (uint32_t)i + 1;
        if ((uint64_t)s * SPR_SIZE > rc->spr.len)
            break;
        if (!N(rc, s, 0))
            continue;
        int32_t dx = wsub(N(rc, s, 4), R(0x50)) >> 8;
        int32_t dy = wsub(N(rc, s, 8), R(0x54)) >> 8;
        int32_t c = pcos(rc, wneg(R(0x58))) >> 8;
        int32_t sn = psin(rc, wneg(R(0x58))) >> 8;
        int32_t depth = wsub(wmul(dx, c), wmul(dy, sn)) >> 16;
        int32_t lat = wadd(wmul(dx, sn), wmul(dy, c)) >> 16;
        int32_t z = depth;
        if (z <= 0)
            continue;
        uint32_t cur = (uint32_t)N(rc, LIST_HEAD, 0x50), prev = LIST_HEAD;
        int32_t scale = idiv(shl16(R(0x48)), z);
        NS(rc, s, 0x44, wmul(N(rc, s, 0x10), scale) >> 16);
        NS(rc, s, 0x48, wmul(N(rc, s, 0x14), scale) >> 16);
        NS(rc, s, 0x38, wadd(wmul(lat, scale) >> 16, wsub(R(0xc), N(rc, s, 0x44)) / 2));
        NS(rc, s, 0x3c, wsub(R(0x44), wadd(wmul(N(rc, s, 0xc), scale) >> 16, N(rc, s, 0x48) / 2)));
        NS(rc, s, 0x40, z);
        if (N(rc, s, 0x3c) >= R(0x10) || wadd(N(rc, s, 0x3c), N(rc, s, 0x48)) < 0 ||
            N(rc, s, 0x38) >= R(0xc) || wadd(N(rc, s, 0x38), N(rc, s, 0x44)) < 0 ||
            N(rc, s, 0x44) == 0 || N(rc, s, 0x48) == 0)
            continue;
        for (int guard = 0; cur && valid_node(rc, cur) && guard <= nmax &&
                            N(rc, s, 0x40) < N(rc, cur, 0x40); guard++) {
            prev = cur;
            cur = (uint32_t)N(rc, cur, 0x50);
        }
        if (cur && !valid_node(rc, cur))
            cur = 0;
        if (!cur)
            insert_after(rc, s, prev);
        else
            insert_before(rc, s, cur);
    }
}

static int32_t slice_dist(RC *rc, int64_t col) { return col >= 0 && col < (1LL << 28) ? m32(&rc->slices, (uint32_t)col * 8) : 0; }

/* drawImageCore (0x418008) of the sprite image */
static void sprite_blit(RC *rc, EXimg *D, uint32_t s, int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                        int32_t sx, int32_t sy, int32_t sw, int32_t sh)
{
    EXimg S;
    if (!eximg_get(rc->vm, (ref_t)N(rc, s, 0x30), &S))
        return; /* drawImageCore returns -2 for a null image */
    eximg_draw_image(rc->vm, D, dx, dy, dw, dh, &S, sx, sy, sw, sh, 0);
}

/* 0x42080d: draw the sorted sprites in column runs where they are nearer than the walls */
static void draw_sprites(RC *rc, EXimg *D)
{
    if (!rc->slices.p) {
        vm_log(rc->vm, "RayCast: draw sprites: null pointer");
        return;
    }
    int32_t step = R(0x14);
    uint32_t n = (uint32_t)N(rc, LIST_HEAD, 0x50);
    for (int guard = 0; n && valid_node(rc, n) && n != LIST_HEAD && guard <= R(0xb4); guard++) {
        int32_t w = N(rc, n, 0x44);
        RS(NODE_A + 0x04, N(rc, n, 0x18));
        RS(NODE_A + 0x08, N(rc, n, 0x1c));
        RS(NODE_A + 0x0c, N(rc, n, 0x20));
        RS(NODE_A + 0x10, N(rc, n, 0x24));
        RS(NODE_A + 0x24, idiv(shl16(N(rc, n, 0x20)), N(rc, n, 0x44)));
        RS(NODE_A + 0x28, idiv(shl16(N(rc, n, 0x24)), N(rc, n, 0x48)));
        RS(NODE_A + 0x2c, shl16(N(rc, n, 0x18)));
        RS(NODE_A + 0x00, N(rc, n, 0x30));
        RS(NODE_B + 0x00, N(rc, n, 0x34));
        RS(NODE_B + 0x04, N(rc, n, 0x28));
        RS(NODE_B + 0x08, N(rc, n, 0x2c));
        RS(NODE_B + 0x0c, N(rc, n, 0x20));
        RS(NODE_B + 0x10, N(rc, n, 0x24));
        RS(NODE_B + 0x24, R(NODE_A + 0x24));
        RS(NODE_B + 0x28, R(NODE_A + 0x28));
        RS(NODE_B + 0x2c, shl16(N(rc, n, 0x28)));
        if (N(rc, n, 0x38) < D->clipX) {
            int32_t cut = wsub(D->clipX, N(rc, n, 0x38));
            w = wadd(w, wsub(N(rc, n, 0x38), D->clipX));
            RS(NODE_A + 0x2c, wadd(R(NODE_A + 0x2c), wmul(cut, R(NODE_A + 0x24))));
            RS(NODE_B + 0x2c, wadd(R(NODE_B + 0x2c), wmul(cut, R(NODE_B + 0x24))));
            NS(rc, n, 0x38, D->clipX);
        }
        int32_t e = wsub(wadd(N(rc, n, 0x38), w), wmul(R(0x18), step));
        if (e > 0)
            w -= e;
        e = wsub(wadd(N(rc, n, 0x38), w), wadd(D->clipX, D->clipW));
        if (e > 0)
            w -= e;
        RS(NODE_A + 0x18, N(rc, n, 0x3c));
        RS(NODE_B + 0x18, R(NODE_A + 0x18));
        RS(NODE_A + 0x20, N(rc, n, 0x48));
        RS(NODE_B + 0x20, R(NODE_A + 0x20));
        RS(NODE_A + 0x1c, step);
        RS(NODE_B + 0x1c, R(NODE_A + 0x1c));
        RS(NODE_A + 0x14, N(rc, n, 0x38));
        RS(NODE_B + 0x14, R(NODE_A + 0x14));
        if (w > 0 && step > 0) {
            int32_t col = idiv(N(rc, n, 0x38), step);
            RS(NODE_B + 0x24, wmul(R(NODE_B + 0x24), step));
            RS(NODE_A + 0x24, wmul(R(NODE_A + 0x24), step));
            int32_t start = -1, uA = 0, end, uA2;
            for (int32_t k = 0; k < w; k += step) {
                if (N(rc, n, 0x40) < slice_dist(rc, col)) {
                    if (start < 0) {
                        start = N(rc, n, 0x38);
                        uA = R(NODE_A + 0x2c);
                    }
                } else if (start >= 0) {
                    end = N(rc, n, 0x38);
                    uA2 = R(NODE_A + 0x2c);
                    sprite_blit(rc, D, n, start, R(NODE_A + 0x18), end - start, R(NODE_A + 0x20),
                                uA >> 16, N(rc, n, 0x1c), wsub(uA2, uA) >> 16, N(rc, n, 0x24));
                    start = -1;
                }
                RS(NODE_A + 0x2c, wadd(R(NODE_A + 0x2c), R(NODE_A + 0x24)));
                RS(NODE_B + 0x2c, wadd(R(NODE_B + 0x2c), R(NODE_B + 0x24)));
                NS(rc, n, 0x38, wadd(N(rc, n, 0x38), step));
                col++;
            }
            if (start >= 0) {
                end = N(rc, n, 0x38);
                uA2 = R(NODE_A + 0x2c);
                sprite_blit(rc, D, n, start, R(NODE_A + 0x18), end - start, R(NODE_A + 0x20),
                            uA >> 16, N(rc, n, 0x1c), wsub(uA2, uA) >> 16, N(rc, n, 0x24));
            }
        }
        n = (uint32_t)N(rc, n, 0x50);
    }
}

/* ------------------------------------------------------------ rays */

static void clamp_pos(RC *rc) /* 0x41f3fe */
{
    if (R(0x50) < 0)
        RS(0x50, 0);
    int32_t mx = shl16((int32_t)((uint32_t)wsub(R(0x60), 1) << 6));
    if (R(0x50) > mx)
        RS(0x50, mx);
    if (R(0x54) < 0)
        RS(0x54, 0);
    int32_t my = shl16((int32_t)((uint32_t)wsub(R(0x64), 1) << 6));
    if (R(0x54) > my)
        RS(0x54, my);
}

static uint8_t map_at(RC *rc, int32_t cx, int32_t cy, int32_t w) { return m8(&rc->map, (int64_t)cy * w + cx); }

/* 0x41fea6 / 0x41fe65: distance along the ray from a delta and sin/cos */
static int32_t dist_by(int32_t trig, int32_t delta)
{
    if (trig == 0)
        return INT32_MAX;
    int32_t d = idiv(delta, trig);
    return d < 0 ? wneg(d) : d;
}

typedef struct { int32_t x, y, dist; } Hit;

/* 0x41f7f2: cast one ray at angle a. out8 is the 8-byte slice record {dist, wall, texcol}. */
static int32_t cast_ray(RC *rc, int32_t a, Mem *om, uint32_t ooff, Hit *hit)
{
    const int32_t MASK = (int32_t)0xffc00000;
    int32_t hitsV = 0, hitsH = 0, stepXv = 0, stepYh = 0;
    int32_t W = R(0x60), H = R(0x64);
    uint8_t hwall = 0, vwall = 0;
    int32_t q1 = R(0x2c), q2 = R(0x30), q3 = R(0x34), full = R(0x1c);
    int32_t hx, hy, vx, vy;

    /* crossings of horizontal grid lines */
    if (a == q2 || a == full) {
        hitsH = R(0);
        hwall = 0;
        hx = hy = INT32_MAX;
    } else {
        if (a > q2 && a < full) {
            hy = R(0x54) & MASK;
            stepYh = (int32_t)0xffc00000;
        } else {
            hy = wadd(R(0x54) & MASK, 0x400000);
            stepYh = 0x400000;
        }
        int32_t dy = wsub(hy, R(0x54));
        int32_t c = pcos(rc, a) >> 1, s = psin(rc, a) >> 1;
        c = shl16(c);
        int32_t k = s != 0 ? idiv(c, s) : 0xc80;
        int32_t stepXh = wmul(k, stepYh >> 16);
        hx = wadd(wmul(k, dy >> 16), R(0x50));
        hx = stepXh < 0 ? wsub(hx, 0x7fff) : wadd(hx, 0x7fff);
        for (hitsH = 0; hitsH < R(0); hitsH++) {
            int32_t cx = (hx >> 16) >> 6, cy = (hy >> 16) >> 6;
            if (cx < 0 || cy < 0 || cx >= W || cy >= H)
                break;
            uint8_t m = map_at(rc, cx, cy, W);
            if (m & 0xf0) {
                hwall = (uint8_t)(m >> 4);
                break;
            }
            hx = wadd(hx, stepXh);
            hy = wadd(hy, stepYh);
        }
    }

    /* crossings of vertical grid lines (here the halving truncates toward zero) */
    if (a == q1 || a == q3) {
        hitsV = R(0);
        vwall = 0;
        vx = vy = INT32_MAX;
    } else {
        if (a > q1 && a < q3) {
            vx = R(0x50) & MASK;
            stepXv = (int32_t)0xffc00000;
        } else {
            vx = wadd(R(0x50) & MASK, 0x400000);
            stepXv = 0x400000;
        }
        int32_t dx = wsub(vx, R(0x50));
        int32_t c = pcos(rc, a) / 2, s = psin(rc, a) / 2;
        s = shl16(s);
        int32_t k = c != 0 ? idiv(s, c) : 0xc80;
        int32_t stepYv = wmul(k, stepXv >> 16);
        vy = wadd(wmul(k, dx >> 16), R(0x54));
        vy = stepYv < 0 ? wsub(vy, 0x7fff) : wadd(vy, 0x7fff);
        for (hitsV = 0; hitsV < R(0); hitsV++) {
            int32_t cx = (vx >> 16) >> 6, cy = (vy >> 16) >> 6;
            if (cx < 0 || cy < 0 || cx >= W || cy >= H)
                break;
            uint8_t m = map_at(rc, cx, cy, W);
            if (m & 0x0f) {
                vwall = (uint8_t)(m & 0x0f);
                break;
            }
            vx = wadd(vx, stepXv);
            vy = wadd(vy, stepYv);
        }
    }

    int32_t distH = dist_by(psin(rc, a), wsub(hy, R(0x54)));
    int32_t distV = dist_by(pcos(rc, a), wsub(vx, R(0x50)));
    int32_t d;
    if (distH < distV) {
        d = distH;
        hit->x = hx;
        hit->y = hy;
        hit->dist = distH;
    } else {
        hit->x = vx;
        hit->y = vy;
        hit->dist = distV;
        d = distV;
    }
    int32_t rel = wsub(a, R(0x58));
    if (rel < 0)
        rel = wadd(rel, full);
    if (rel < 0)
        rel = wadd(rel, full);
    d = wmul(d, pcos(rc, rel)) >> 16; /* fisheye correction against the last draw's angle */
    if (d < 0)
        d = wneg(d);
    else if (d == 0)
        d = 1;
    int32_t hits = hitsV < hitsH ? hitsV : hitsH;
    s32(om, ooff, d);
    if (distH < distV) {
        s8(om, (int64_t)ooff + 4, hwall);
        s8(om, (int64_t)ooff + 5, (uint8_t)(stepYh < 0 ? (hx >> 16) & 0x3f : 0x3f - ((hx >> 16) & 0x3f)));
    } else {
        s8(om, (int64_t)ooff + 4, vwall);
        s8(om, (int64_t)ooff + 5, (uint8_t)(stepXv < 0 ? 0x3f - ((vy >> 16) & 0x3f) : (vy >> 16) & 0x3f));
    }
    return hits;
}

/* 0x41f6c0: one ray per column step */
static void cast_all(RC *rc)
{
    if (!rc->map.p || !rc->slices.p) {
        vm_log(rc->vm, "RayCast: cast: null pointer");
        return;
    }
    int32_t a = wsub(R(0x58), R(0x24));
    if (a < 0)
        a = wadd(R(0x1c), a);
    int32_t step = R(0x14);
    if (step <= 0)
        return; /* the original would loop forever */
    uint32_t i = 0;
    for (int32_t x = 0; x < R(0xc); x += step) {
        Hit h;
        cast_ray(rc, a, &rc->slices, i * 8, &h);
        i++;
        a = wadd(a, step);
        if (a >= R(0x1c))
            a = wsub(a, R(0x1c));
    }
}

/* ------------------------------------------------------------ walls */

/* ops slot 17 (8bpp 0x42ccc4): one textured wall column, 1 or 2 pixels wide */
static void draw_slice(RC *rc, EXimg *D, EXimg *T, int32_t x, int32_t top, int32_t width,
                       int32_t height, int32_t texcol, uint8_t shift)
{
    ExenVM *vm = rc->vm;
    int32_t skip = 0;
    if (width == 2)
        x &= ~1;
    else
        width = 0;
    if (!T) {
        vm_log(vm, "RayCast: raycDrawVerticalSlice: null pointer");
        return;
    }
    Mem src, dst;
    src.p = eximg_data(vm, T, &src.len);
    dst.p = eximg_data(vm, D, &dst.len);
    int32_t col = wmul(texcol & 0x3f, T->w) >> 6;
    int32_t pitch = D->pitch;
    if (height == 0)
        return;
    int32_t dv = idiv(shl16(T->h), height);
    if (top < D->clipY) {
        skip = wsub(D->clipY, top);
        height = wsub(height, skip);
        top = D->clipY;
    }
    if (wadd(top, height) >= wadd(D->clipY, D->clipH)) {
        height = wsub(wadd(D->clipY, D->clipH), top);
        if (height <= 0)
            return;
    }
    int32_t v = wmul(dv, skip);
    int64_t d = (int64_t)top * pitch + x;
    pitch -= 1;
    const uint8_t *lut = NULL;
    int nibbles = 0;
    int64_t base;
    if (T->palCount == 0) {
        base = col;
        shift = (uint8_t)(shift + 3);
    } else if (T->depth == 8) {
        lut = eximg_lut(vm, T);
        base = col;
        shift = (uint8_t)(shift + 3);
    } else if (T->depth == 4) {
        lut = eximg_lut(vm, T);
        base = col >> 1;
        shift = (uint8_t)(shift + 2);
        nibbles = (col & 1) ? 1 : 2; /* 1: low nibble, 2: high nibble */
    } else {
        return;
    }
    for (int32_t k = 0; k < height; k++) {
        int64_t si = base + (int64_t)(int32_t)((uint32_t)(v >> 16) << (shift & 31));
        v = wadd(v, dv);
        uint8_t c = m8(&src, si);
        if (nibbles == 1)
            c &= 0x0f;
        else if (nibbles == 2)
            c = (uint8_t)(c >> 4);
        if (lut)
            c = lut[c];
        s8(&dst, d, c);
        d += 1;
        if (width)
            s8(&dst, d, c);
        d += pitch;
    }
}

/* 0x41fee7 */
static void draw_walls(RC *rc, EXimg *D)
{
    int32_t step = R(0x14);
    int32_t xpix = 0, first = 0;
    if (D->clipX > 0) {
        first = D->clipX;
        xpix = wmul(first, step);
    }
    int32_t last = wsub(wmul(R(0x18), step), first);
    if (last > wadd(D->clipW, D->clipX))
        last = wadd(D->clipW, D->clipX);
    if (last < first) {
        vm_log(rc->vm, "RayCast: draw: nothing to draw in the clip");
        return;
    }
    if (step == 2)
        last >>= 1;
    if (D->ops != 8)
        return; /* the slice op is a stub on other depths */
    for (int32_t i = first; i < last; i++) {
        int32_t d = slice_dist(rc, i);
        if (d != 0) {
            int32_t hgt = idiv(wmul(R(8), R(0x48)), d);
            int32_t bot = wadd(R(0x44), hgt >> 1);
            int32_t top = wsub(R(0x44), hgt >> 1);
            int32_t tex = m8(&rc->slices, (int64_t)i * 8 + 5);
            int w = m8(&rc->slices, (int64_t)i * 8 + 4) & 0x0f;
            draw_slice(rc, D, rc->wall_ok[w] ? &rc->walls[w] : NULL, xpix, top, step,
                       wsub(bot, top), tex, m8(&rc->pow2, w));
        }
        xpix = wadd(xpix, step);
    }
}

/* 0x41f4db */
static void render(RC *rc, EXimg *D, int32_t x, int32_t y, int32_t angle)
{
    ExenVM *vm = rc->vm;
    RS(0x50, x);
    RS(0x54, y);
    RS(0x58, angle);
    clamp_pos(rc);
    int32_t saved = vm->math.period;
    set_period(vm, R(4));
    cast_all(rc);
    RS(NODE_HEAD + 0x50, 0);
    project_sprites(rc);
    draw_walls(rc, D);
    draw_sprites(rc, D);
    set_period(vm, saved);
}

/* ------------------------------------------------------------ natives */

static int n_draw(ExenVM *vm, uint32_t *a) /* 137 draw(Graphics, x, y, angle) 0x4284c9 */
{
    RC rc;
    if (!refresh(vm, a[0], &rc))
        return 0;
    if (!rc.map.p) {
        vm_log(vm, "RayCast.draw: no map");
        return 0;
    }
    const uint32_t *f = rc_fields(vm, a[0]);
    for (int i = 1; i < 16; i++)
        rc.wall_ok[i] = eximg_get(vm, image_at(vm, f[RC_WALLS], i), &rc.walls[i]);
    rc.pow2 = arr_mem(vm, f[RC_WALLPOW2]);
    if (!rc.pow2.p) {
        vm_log(vm, "RayCast.draw: no wall widths");
        return 0;
    }
    if (rc.spr.p) {
        for (int32_t i = 0; i < (int32_t)f[RC_MAXSPR] && (uint64_t)(i + 1) * SPR_SIZE <= rc.spr.len; i++) {
            NS(&rc, (uint32_t)i + 1, 0x34, (int32_t)image_at(vm, f[RC_MASKIMG], i));
            NS(&rc, (uint32_t)i + 1, 0x30, (int32_t)image_at(vm, f[RC_SPRIMG], i));
        }
    }
    int cg = plat_class(vm, PC_GRAPHICS);
    EXimg D;
    if (!a[1] || cg < 0 || !eximg_get(vm, inst(vm, a[1], cg)[0], &D)) {
        vm_log(vm, "RayCast.draw: null image");
        return 0;
    }
    eximg_sync_clip(vm, a[1], &D);
    eximg_put(vm, &D);
    render(&rc, &D, (int32_t)a[2], (int32_t)a[3], (int32_t)a[4]);
    return 0;
}

static int n_isThereAWall(ExenVM *vm, uint32_t *a) /* 138, 0x41f5b9 */
{
    RC rc, *p = &rc;
    if (!refresh(vm, a[0], p)) {
        a[0] = 0;
        return 1;
    }
    int32_t cx = ((int32_t)a[1] >> 16) / 64, cy = ((int32_t)a[2] >> 16) / 64;
    int32_t W = m32(&p->r, 0x60), H = m32(&p->r, 0x64);
    int32_t idx = wadd(wmul(cy, W), cx);
    uint32_t r = 1;
    if (!p->map.p || cx < 1 || cy < 1 || cx >= W || idx < W || idx >= wmul(wsub(H, 1), W)) {
        r = 0;
    } else if ((m8(&p->map, (int64_t)idx + W) & 0xf0) && (m8(&p->map, idx) & 0xf0) &&
               (m8(&p->map, (int64_t)idx + 1) & 0x0f) && (m8(&p->map, idx) & 0x0f)) {
        r = 0;
    }
    a[0] = r;
    return 1;
}

static int n_addMonster(ExenVM *vm, uint32_t *a) /* 139, 0x42022a */
{
    RC rc;
    if (!refresh(vm, a[0], &rc)) {
        a[0] = 0;
        return 1;
    }
    uint32_t s = sprite_id(&rc, (int32_t)a[1]);
    if (!s) {
        a[0] = (uint32_t)-5;
        return 1;
    }
    int32_t w = (int32_t)a[2], h = (int32_t)a[3];
    NS(&rc, s, 0x00, 1);
    NS(&rc, s, 0x30, 0);
    NS(&rc, s, 0x34, 0);
    NS(&rc, s, 0x10, w);
    NS(&rc, s, 0x14, h);
    NS(&rc, s, 0x18, 0);
    NS(&rc, s, 0x1c, 0);
    NS(&rc, s, 0x20, w);
    NS(&rc, s, 0x24, h);
    NS(&rc, s, 0x28, 0);
    NS(&rc, s, 0x2c, 0);
    a[0] = 1;
    return 1;
}

static int n_findFirstSpriteFreeID(ExenVM *vm, uint32_t *a) /* 140, 0x4201bf */
{
    RC rc;
    if (!refresh(vm, a[0], &rc)) {
        a[0] = 0;
        return 1;
    }
    if (!rc.spr.p) {
        a[0] = 0;
        return 1;
    }
    int32_t n = R_max(&rc);
    a[0] = (uint32_t)-1;
    for (int32_t i = 0; i < n; i++)
        if (!N(&rc, (uint32_t)i + 1, 0)) {
            a[0] = (uint32_t)i;
            break;
        }
    return 1;
}

static int n_removeSprite(ExenVM *vm, uint32_t *a) /* 141, 0x4202c9 */
{
    RC rc;
    if (!refresh(vm, a[0], &rc))
        return 0;
    uint32_t s = sprite_id(&rc, (int32_t)a[1]);
    if (s)
        NS(&rc, s, 0, 0);
    return 0;
}

static int n_moveSprite(ExenVM *vm, uint32_t *a) /* 142, 0x420511 */
{
    RC rc;
    if (!refresh(vm, a[0], &rc))
        return 0;
    uint32_t s = sprite_id(&rc, (int32_t)a[1]);
    if (s) {
        NS(&rc, s, 0x04, (int32_t)a[2]);
        NS(&rc, s, 0x08, (int32_t)a[3]);
        NS(&rc, s, 0x0c, (int32_t)a[4]);
    }
    return 0;
}

static int n_setSpritePos(ExenVM *vm, uint32_t *a) /* 143, 0x4287be -> 0x420302 */
{
    RC rc;
    if (!refresh(vm, a[0], &rc))
        return 0;
    int32_t id = (int32_t)a[1];
    if (id < 0 || id >= R_max(&rc)) {
        vm_log(vm, "RayCast.setSpritePos: sprite index out of range");
        return 0;
    }
    const uint32_t *f = rc_fields(vm, a[0]);
    if (rc.spr.p) {
        NS(&rc, (uint32_t)id + 1, 0x34, (int32_t)image_at(vm, f[RC_MASKIMG], id));
        NS(&rc, (uint32_t)id + 1, 0x30, (int32_t)image_at(vm, f[RC_SPRIMG], id));
    }
    uint32_t s = sprite_id(&rc, id);
    if (!s)
        return 0;
    int32_t sx = (int32_t)a[2], sy = (int32_t)a[3], sw = (int32_t)a[4], sh = (int32_t)a[5];
    int32_t mx = (int32_t)a[6], my = (int32_t)a[7];
    if (sx < 0 || sy < 0 || mx < 0 || my < 0) {
        vm_log(vm, "RayCast.setSpritePos: negative position");
        return 0;
    }
    EXimg S, M;
    if (!eximg_get(vm, (ref_t)N(&rc, s, 0x30), &S) || !eximg_get(vm, (ref_t)N(&rc, s, 0x34), &M)) {
        vm_log(vm, "RayCast.setSpritePos: sprite or mask image missing");
        return 0;
    }
    if (wadd(sx, sw) > S.w || wadd(sy, sh) > S.h || wadd(mx, sw) > M.w || wadd(my, sh) > M.h) {
        vm_log(vm, "RayCast.setSpritePos: rectangle outside the image");
        return 0;
    }
    NS(&rc, s, 0x18, sx);
    NS(&rc, s, 0x1c, sy);
    NS(&rc, s, 0x20, sw);
    NS(&rc, s, 0x24, sh);
    NS(&rc, s, 0x28, mx);
    NS(&rc, s, 0x2c, my);
    return 0;
}

static int n_setSpriteSize(ExenVM *vm, uint32_t *a) /* 144, 0x420412 */
{
    RC rc;
    if (!refresh(vm, a[0], &rc))
        return 0;
    uint32_t s = sprite_id(&rc, (int32_t)a[1]);
    if (!s)
        return 0;
    if ((int32_t)a[2] <= 0 || (int32_t)a[3] <= 0) {
        vm_log(vm, "RayCast.setSpriteSize: bad size");
        return 0;
    }
    NS(&rc, s, 0x10, (int32_t)a[2]);
    NS(&rc, s, 0x14, (int32_t)a[3]);
    return 0;
}

/* 145 changeInternalValues_internal(viewW, viewH, wallH, colStep, horizonOffset, maxSteps),
 * 0x41f0da */
static int n_changeInternalValues(ExenVM *vm, uint32_t *a)
{
    RC rcs, *rc = &rcs;
    if (!refresh(vm, a[0], rc))
        return 0;
    int32_t w = (int32_t)a[1], h = (int32_t)a[2], wallh = (int32_t)a[3], step = (int32_t)a[4];
    int32_t hoff = (int32_t)a[5], steps = (int32_t)a[6];
    if (wallh > 0)
        RS(8, wallh);
    if (step > 0 && step < 3)
        RS(0x14, step);
    if (w > 0) {
        if (R(0x14) == 2)
            w &= 0xfffe;
        RS(0xc, w);
        RS(0x18, idiv(w, R(0x14)));
        RS(0xb0, R(0x18));
        RS(0x20, R(0xc));
        RS(0x24, R(0x20) / 2);
        RS(0x28, R(0x20) / 4);
        RS(0x2c, wmul(R(0x24), 3));
        RS(0x30, wmul(R(0x2c), 2));
        RS(0x34, wmul(R(0x2c), 3));
        RS(0x1c, wmul(R(0x20), 6));
        RS(0x38, 0);
        RS(0x3c, R(0x24) / 6);
        RS(0x40, wmul(R(0x3c), 2));
        set_period(vm, R(0x1c)); /* exen.Math's period changes for everyone */
        RS(4, R(0x1c));
        int32_t s = psin(rc, R(0x20) / 2);
        int32_t c = pcos(rc, R(0x20) / 2);
        int32_t v = wmul(R(0xc) / 2, c);
        v = s == 0 ? 1 : idiv(v, s);
        if (v < 0)
            v = wneg(v);
        RS(0x48, v);
    }
    if (h > 0)
        RS(0x10, h);
    RS(0x44, wadd(h / 2, hoff));
    if (hoff > 0)
        RS(0x4c, hoff);
    if (steps > 0)
        RS(0, steps);
    return 0;
}

static int n_castRay(ExenVM *vm, uint32_t *a) /* 146 castRay(x, y, angle, int[] out) 0x428962 */
{
    RC rc;
    if (!refresh(vm, a[0], &rc)) {
        a[0] = 0;
        return 1;
    }
    ref_t out = a[4];
    if (!out || arr_len(vm, out) < 6) {
        a[0] = 0;
        return 1;
    }
    s32(&rc.r, 0x50, (int32_t)a[1]);
    s32(&rc.r, 0x54, (int32_t)a[2]);
    clamp_pos(&rc);
    uint8_t rec[8] = {0};
    Mem rm = {rec, 8};
    Hit h;
    cast_ray(&rc, (int32_t)a[3], &rm, 0, &h);
    uint8_t *o = arr_data(vm, out);
    wr32(o + 0, (uint32_t)h.x);
    wr32(o + 4, (uint32_t)h.y);
    wr32(o + 8, (uint32_t)h.dist);
    wr32(o + 12, rd32(rec));
    wr32(o + 16, rec[4]);
    wr32(o + 20, rec[5]);
    a[0] = 1;
    return 1;
}

void raycast_register(void)
{
    natives_register(137, n_draw);
    natives_register(138, n_isThereAWall);
    natives_register(139, n_addMonster);
    natives_register(140, n_findFirstSpriteFreeID);
    natives_register(141, n_removeSprite);
    natives_register(142, n_moveSprite);
    natives_register(143, n_setSpritePos);
    natives_register(144, n_setSpriteSize);
    natives_register(145, n_changeInternalValues);
    natives_register(146, n_castRay);
}
