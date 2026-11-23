/* exen.AnimBitmap (natives 43-45), exen.PlayField (46-53), exen.AnimFlash (54-64, stubs) and
 * exen.Displayable (65-66).
 *
 * Sources: docs/re/NATIVES.md §2.3 and docs/re/natives/{AnimBitmap, PlayField}.c.txt, rechecked
 * against the disassembly of gensimu.exe (addresses below). exen.Sprite has no natives: its
 * methods are bytecode, and PlayField reads and links its fields directly.
 *
 * Quirks of the original that are kept:
 *   - The blit "mode" codes (1, 3, 4) that AnimBitmap and PlayField pass to drawImageCore
 *     (0x418008) have no effect: they go to the source's setPaintMode slot, a stub in both
 *     image vtables. A WITHMASK mask is therefore drawn like any other image, through the
 *     source's own blit mode (transparent key, palette, alpha), before the image itself.
 *   - PlayField.draw writes the view-intersected clip into the Graphics object's clip fields
 *     while it draws, and puts the old values back at the end.
 Bugs of the original that are fixed (the project's decision; no corpus game depends on them):
 *   - The z sort in PlayField.draw (an in-place cocktail sort of the sprite list, 0x427b58)
 *     relinks the two swapped sprites but not their outer neighbours, so a swap in the middle of
 *     the list can drop a sprite from it. We sort with the same order (ascending z, stable: the
 *     original only swaps on a strictly smaller z) and relink every neighbour.
 *   - moveTiles (0x42712f) clips both rectangles and picks a loop direction, but its loop
 *     bodies never add the loop counters to the coordinates: it copies column x2, rows [0, h),
 *     to column x1, w times. We move the whole w x h block from (x2, y2) to (x1, y1), in the
 *     direction that is safe for overlapping rectangles.
 * Other quirks of the original that are kept:
 *   - Only sprites whose animation is exactly an exen.AnimBitmap are drawn; AnimFlash goes to its
 *     stub draw and any other Animation class is silently skipped.
 *   - haveDisplayableCommand is always true.
 * Where the original would crash the host (division by zero, null dereference, out-of-range
 * reads), we halt the VM or skip the access, and say so.
 */
#include <string.h>

#include "platform.h"

/* Field slots (tools/cvp.py dump of the vendored gensimu.exe). */
enum { G_PIXIMA, G_COLOR, G_PAINTMODE, G_CLIPX, G_CLIPY, G_CLIPW, G_CLIPH };
enum { I_WIDTH, I_HEIGHT };
enum { AB_IMAGE, AB_NBFRAME, AB_WIDTH, AB_HEIGHT, AB_STATE, AB_LISTCOORDS, AB_FRAMESEQ };
enum {
    PF_STATE, PF_CHARSET, PF_ANIMTILEINDEX, PF_ANIMTILEMAX, PF_CHARW, PF_CHARH, PF_BACKGROUND,
    PF_BGW, PF_BGH, PF_NBBITS, PF_VIEWX, PF_VIEWY, PF_VIEWW, PF_VIEWH, PF_FIRST, PF_LAST
};
enum { SP_ANIM, SP_FRAME, SP_X, SP_Y, SP_Z, SP_VISIBLE, SP_TRANSFORM, SP_NEXT, SP_PREV };
enum { AF_CTX, AF_TX, AF_TY, AF_WIDTH, AF_HEIGHT };

/* AnimBitmap.state / PlayField.state bits (the classes' <clinit>). */
#define AB_WITHMASK 1
#define AB_INVERSEVIDEO 2
#define PF_BACKGROUND_BIT 2
#define PF_CHARSET_BIT 4
#define PF_CHARSET_OPACITY 8
#define PF_CHARSET_WITHMASK 0x10
#define PF_CHARSET_INVERSEVIDEO 0x20

/* plat_warn_once bits used here (graphics.c and resource.c use 0..8). */
enum {
    W_PF_SORT = 16, W_PF_LIST, W_PF_ANIMTILE, W_PF_CELL, W_AB_COORDS, W_AB_ANIM, W_DC_LABEL,
    W_AB_GRID
};

#define ARG(i) ((int32_t)a[i])

/* 32-bit wrap-around arithmetic, as the original's int registers. */
static int32_t wadd(int32_t x, int32_t y) { return (int32_t)((uint32_t)x + (uint32_t)y); }
static int32_t wsub(int32_t x, int32_t y) { return (int32_t)((uint32_t)x - (uint32_t)y); }
static int32_t wmul(int32_t x, int32_t y) { return (int32_t)((uint32_t)x * (uint32_t)y); }

/* idiv: truncating, like C. The callers rule out a zero divisor; INT_MIN / -1 (a #DE fault in
 * the original) gives INT_MIN with remainder 0. */
static int32_t qdiv(int32_t x, int32_t y) { return (y == -1) ? wsub(0, x) : x / y; }
static int32_t qmod(int32_t x, int32_t y) { return (y == -1) ? 0 : x % y; }

static int is_arr(ExenVM *vm, ref_t r, uint16_t type)
{
    return r && heap_is_object(vm, r) && is_array(vm, r) && arr_type(vm, r) == type;
}

/* The fields of `obj` as an instance of framework class `which`, or NULL if the class is
 * missing or `obj` is not an object. */
static uint32_t *fields(ExenVM *vm, ref_t obj, int which)
{
    int c = plat_class(vm, which);
    if (!obj || c < 0 || !heap_is_object(vm, obj) || is_array(vm, obj))
        return NULL;
    return inst(vm, obj, c);
}

static uint32_t class_hash_of(ExenVM *vm, ref_t obj)
{
    uint16_t c = obj_cls(vm, obj);
    return c < vm->nclasses ? vm->classes[c].hash : 0;
}

/* ============================================================ AnimBitmap */

/* frameIndex 0x424580 (= native 45 getRealFrame): through frameSequence, then mod nbFrame.
 * Returns 0 and halts the VM where the original divides by zero. */
static int ab_frame_index(ExenVM *vm, const uint32_t *ab, int32_t f, int32_t *out)
{
    ref_t seq = ab[AB_FRAMESEQ];
    if (seq) {
        int32_t L = is_arr(vm, seq, T_INT) ? arr_len(vm, seq) : 0;
        if (L == 0) {
            vm_halt(vm, "AnimBitmap: empty frameSequence (division by zero in the original)");
            return 0;
        }
        f %= L;
        if (f < 0)
            f += L;
        f = (int32_t)rd32(arr_data(vm, seq) + 4 * (uint32_t)f);
    }
    int32_t n = (int32_t)ab[AB_NBFRAME];
    if (n == 0) {
        vm_halt(vm, "AnimBitmap: nbFrame is 0 (division by zero in the original)");
        return 0;
    }
    f = qmod(f, n);
    if (f < 0)
        f = wadd(f, n);
    *out = f;
    return 1;
}

/* frameRect 0x4243d0: r = {sx, sy, w, h} of frame f in the image. Uses 16-bit views of
 * nbFrame, width, height and Image.width, as the original. */
static int ab_frame_rect(ExenVM *vm, const uint32_t *ab, int32_t f, int32_t r[4])
{
    r[0] = (int32_t)ab[AB_WIDTH];
    r[1] = (int32_t)ab[AB_HEIGHT];
    r[2] = r[3] = 0;
    int16_t nb = (int16_t)ab[AB_NBFRAME], w = (int16_t)ab[AB_WIDTH], h = (int16_t)ab[AB_HEIGHT];
    if (nb == 0) {
        vm_halt(vm, "AnimBitmap: nbFrame & 0xffff is 0 (division by zero in the original)");
        return 0;
    }
    f %= nb;
    if (f < 0)
        f += nb;
    ref_t lc = ab[AB_LISTCOORDS];
    if (!lc) {
        if (!w || !h || !ab[AB_IMAGE])
            return 1; /* r stays {width, height, 0, 0}: the blits are rejected */
        const uint32_t *img = fields(vm, ab[AB_IMAGE], PC_IMAGE);
        int16_t iw = img ? (int16_t)img[I_WIDTH] : 0;
        if (!iw) {
            vm_log(vm, "exen_Animation_draw, Param nl=0, Division by zero rejected.");
            return 1;
        }
        if (ab[AB_STATE] & AB_WITHMASK)
            iw = (int16_t)(iw >> 1);
        int16_t cols = (int16_t)(iw / w);
        if (cols == 0) {
            /* frame wider than the (half) image: the original divides by zero */
            plat_warn_once(vm, W_AB_GRID, "AnimBitmap: frame width %d > image width %d "
                                          "(division by zero in the original); not drawn", w, iw);
            r[2] = r[3] = 0;
            return 1;
        }
        r[0] = (f % cols) * w;
        r[1] = (f / cols) * h;
        r[2] = w;
        r[3] = h;
        return 1;
    }
    /* 4 shorts per frame, no bounds check in the original */
    uint32_t k = 4u * (uint32_t)f;
    if (!is_arr(vm, lc, T_SHORT) || k + 3 >= arr_len(vm, lc)) {
        plat_warn_once(vm, W_AB_COORDS, "AnimBitmap: frame %d outside listCoords (the original "
                                        "reads past the array); not drawn", f);
        r[0] = r[1] = r[2] = r[3] = 0;
        return 1;
    }
    const uint8_t *c = arr_data(vm, lc) + 2 * k;
    for (int i = 0; i < 4; i++)
        r[i] = (int16_t)rd16(c + 2 * i);
    return 1;
}

/* AnimBitmap.draw 0x42469c, also called by PlayField.draw for AnimBitmap sprites. */
static void ab_draw(ExenVM *vm, ref_t self, ref_t g, int32_t x, int32_t y, int32_t frame,
                    int32_t tr)
{
    uint32_t *ab = fields(vm, self, PC_ANIMBITMAP);
    uint32_t *gf = fields(vm, g, PC_GRAPHICS);
    if (!ab || !gf)
        return;
    EXimg src, dst;
    if (!eximg_get(vm, ab[AB_IMAGE], &src) || !eximg_get(vm, gf[G_PIXIMA], &dst))
        return;
    eximg_sync_clip(vm, g, &dst);
    eximg_put(vm, &dst);
    const uint32_t *img = fields(vm, ab[AB_IMAGE], PC_IMAGE);
    if (!img || !ab[AB_NBFRAME] || !img[I_WIDTH] || !img[I_HEIGHT])
        return;
    int32_t f, r[4];
    if (!ab_frame_index(vm, ab, frame, &f) || !ab_frame_rect(vm, ab, f, r))
        return;
    int32_t state = (int16_t)ab[AB_STATE];
    int32_t maskx = wadd((int32_t)img[I_WIDTH] >> 1, r[0]);
    /* mode codes: normal video mask 3 / image 4, inverse video mask 4 / image 3 (no effect) */
    if (state & AB_WITHMASK)
        eximg_draw_image(vm, &dst, x, y, r[2], r[3], &src, maskx, r[1], r[2], r[3], tr);
    eximg_draw_image(vm, &dst, x, y, r[2], r[3], &src, r[0], r[1], r[2], r[3], tr);
}

static int n_AnimBitmap_draw(ExenVM *vm, uint32_t *a)
{
    if (a[1])
        ab_draw(vm, a[0], a[1], ARG(2), ARG(3), ARG(4), ARG(5));
    return 0;
}

/* moduloFrame 0x4245fe: positive f mod (frameSequence.length, or nbFrame). */
static int n_AnimBitmap_moduloFrame(ExenVM *vm, uint32_t *a)
{
    uint32_t *ab = fields(vm, a[0], PC_ANIMBITMAP);
    int32_t f = ARG(1), L = 0;
    if (ab)
        L = ab[AB_FRAMESEQ] ? (is_arr(vm, ab[AB_FRAMESEQ], T_INT) ? arr_len(vm, ab[AB_FRAMESEQ]) : 0)
                            : (int32_t)ab[AB_NBFRAME];
    if (L == 0) {
        vm_halt(vm, "AnimBitmap.moduloFrame: modulus 0 (division by zero in the original)");
        a[0] = 0;
        return 1;
    }
    f = qmod(f, L);
    if (f < 0)
        f = wadd(f, L);
    a[0] = (uint32_t)f;
    return 1;
}

static int n_AnimBitmap_getRealFrame(ExenVM *vm, uint32_t *a)
{
    uint32_t *ab = fields(vm, a[0], PC_ANIMBITMAP);
    int32_t f = 0;
    if (ab)
        ab_frame_index(vm, ab, ARG(1), &f);
    a[0] = (uint32_t)f;
    return 1;
}

/* ============================================================ PlayField cells */

/* The background byte[] holds one u8 per cell (nbBits == 8) or one little-endian u16, row-major.
 * Accesses outside the array (the original does not check) read 0 and write nothing. */
static uint32_t cell_raw(ExenVM *vm, const uint32_t *pf, int32_t cx, int32_t cy)
{
    ref_t bg = pf[PF_BACKGROUND];
    if (!is_arr(vm, bg, T_BYTE))
        return 0;
    int64_t i = (int64_t)cy * (int32_t)pf[PF_BGW] + cx;
    int wide = pf[PF_NBBITS] != 8;
    if (i < 0 || (i + 1) * (wide ? 2 : 1) > arr_len(vm, bg)) {
        plat_warn_once(vm, W_PF_CELL, "PlayField: cell (%d,%d) outside the background array",
                       cx, cy);
        return 0;
    }
    const uint8_t *d = arr_data(vm, bg);
    return wide ? rd16(d + 2 * i) : d[i];
}

/* cellGet 0x426e9b: values >= (1 << nbBits) - animTileMaxIndex (unsigned) are animated tiles
 * and come back negative. */
static int32_t cell_get(ExenVM *vm, const uint32_t *pf, int32_t cx, int32_t cy)
{
    uint32_t v = cell_raw(vm, pf, cx, cy);
    uint32_t one = 1u << (pf[PF_NBBITS] & 31);
    return v >= one - pf[PF_ANIMTILEMAX] ? (int32_t)(v - one) : (int32_t)v;
}

/* cellSet 0x426e30: negative values get 1 << nbBits added, then the low 8 or 16 bits are stored. */
static void cell_set(ExenVM *vm, const uint32_t *pf, int32_t cx, int32_t cy, int32_t v)
{
    ref_t bg = pf[PF_BACKGROUND];
    if (!is_arr(vm, bg, T_BYTE))
        return;
    if (v < 0)
        v = wadd(v, (int32_t)(1u << (pf[PF_NBBITS] & 31)));
    int64_t i = (int64_t)cy * (int32_t)pf[PF_BGW] + cx;
    int wide = pf[PF_NBBITS] != 8;
    if (i < 0 || (i + 1) * (wide ? 2 : 1) > arr_len(vm, bg)) {
        plat_warn_once(vm, W_PF_CELL, "PlayField: cell (%d,%d) outside the background array",
                       cx, cy);
        return;
    }
    uint8_t *d = arr_data(vm, bg);
    if (wide)
        wr16(d + 2 * i, (uint16_t)v);
    else
        d[i] = (uint8_t)v;
}

static int in_map(const uint32_t *pf, int32_t cx, int32_t cy)
{
    return cx >= 0 && cy >= 0 && cx < (int32_t)pf[PF_BGW] && cy < (int32_t)pf[PF_BGH] &&
           pf[PF_BACKGROUND];
}

/* getCellTile 0x426f37: 0 outside the map. */
static int n_PlayField_getCellTile(ExenVM *vm, uint32_t *a)
{
    uint32_t *pf = fields(vm, a[0], PC_PLAYFIELD);
    int32_t v = 0;
    if (pf && in_map(pf, ARG(1), ARG(2)))
        v = cell_get(vm, pf, ARG(1), ARG(2));
    a[0] = (uint32_t)v;
    return 1;
}

/* setCellTile 0x426fa8: ignored outside the map. */
static int n_PlayField_setCellTile(ExenVM *vm, uint32_t *a)
{
    uint32_t *pf = fields(vm, a[0], PC_PLAYFIELD);
    if (pf && in_map(pf, ARG(1), ARG(2)))
        cell_set(vm, pf, ARG(1), ARG(2), ARG(3));
    return 0;
}

/* fillCells(cx, cy, w, h, v) 0x427013: the rect is clipped to the map, v is truncated to 16 bits. */
static int n_PlayField_fillCells(ExenVM *vm, uint32_t *a)
{
    uint32_t *pf = fields(vm, a[0], PC_PLAYFIELD);
    if (!pf || !pf[PF_BACKGROUND])
        return 0;
    int32_t cx = ARG(1), cy = ARG(2), w = ARG(3), h = ARG(4);
    int32_t v = (int16_t)a[5];
    if (cx < 0) {
        w = wadd(w, cx);
        cx = 0;
    }
    if (cy < 0) {
        h = wadd(h, cy);
        cy = 0;
    }
    int32_t W = (int32_t)pf[PF_BGW], H = (int32_t)pf[PF_BGH];
    if (wsub(W, cx) < w)
        w = wsub(W, cx);
    if (wsub(H, cy) < h)
        h = wsub(H, cy);
    for (int32_t j = 0; j < h; j++)
        for (int32_t i = 0; i < w; i++)
            cell_set(vm, pf, cx + i, cy + j, v);
    return 0;
}

/* moveTiles(x1, y1, x2, y2, w, h) 0x42712f: move the w x h block of cells at (x2, y2) to
 * (x1, y1). The clipping is the original's. The original then picks one of four loop
 * directions from the relative position but never adds its loop counters to the coordinates
 * (it copies column x2, rows [0, h), to column x1, w times); we do the block move it intended,
 * iterating away from the overlap so an overlapping move copies each cell before overwriting
 * it. */
static int n_PlayField_moveTiles(ExenVM *vm, uint32_t *a)
{
    uint32_t *pf = fields(vm, a[0], PC_PLAYFIELD);
    if (!pf || !pf[PF_BACKGROUND])
        return 0;
    int32_t x1 = ARG(1), y1 = ARG(2), x2 = ARG(3), y2 = ARG(4), w = ARG(5), h = ARG(6);
    int32_t W = (int32_t)pf[PF_BGW], H = (int32_t)pf[PF_BGH];
    if (x1 >= W || x2 >= W || y2 >= H || y1 >= H)
        return 0;
    if (x1 < 0) { x2 = wsub(x2, x1); w = wadd(w, x1); x1 = 0; }
    if (y1 < 0) { y2 = wsub(y2, y1); h = wadd(h, y1); y1 = 0; }
    if (x2 < 0) { x1 = wsub(x1, x2); w = wadd(w, x2); x2 = 0; }
    if (y2 < 0) { y1 = wsub(y1, y2); h = wadd(h, y2); y2 = 0; }
    int32_t t = w < wsub(W, x2) ? w : wsub(W, x2);
    w = t < wsub(W, x1) ? t : wsub(W, x1);
    t = h < wsub(H, y2) ? h : wsub(H, y2);
    h = t < wsub(H, y1) ? t : wsub(H, y1);
    int dx = x1 > x2 ? -1 : 1, dy = y1 > y2 ? -1 : 1;
    for (int32_t jj = 0; jj < h; jj++) {
        int32_t j = dy > 0 ? jj : h - 1 - jj;
        for (int32_t ii = 0; ii < w; ii++) {
            int32_t i = dx > 0 ? ii : w - 1 - ii;
            cell_set(vm, pf, x1 + i, y1 + j, cell_get(vm, pf, x2 + i, y2 + j));
        }
    }
    return 0;
}

/* ============================================================ PlayField sprites */

/* Bound on list walks. The original has none; a corrupt list (e.g. from a bad save state) would hang. */
#define LIST_LIMIT 100000

/* The z sort of PlayField.draw (0x427b58..0x427df0): ascending z, stable (the original's
 * cocktail sort only swaps on a strictly smaller z). Unlike the original, which relinks only the
 * swapped pair and can drop a sprite from the list, every neighbour is relinked. Exposed for the
 * unit tests. */
void playfield_sort_sprites(ExenVM *vm, uint32_t *pf)
{
    int cs = plat_class(vm, PC_SPRITE);
    if (cs < 0 || !pf[PF_FIRST])
        return;
#define SPR(r) inst(vm, (r), cs)
    enum { MAXS = 1024 };
    ref_t list[MAXS];
    int n = 0;
    for (ref_t cur = pf[PF_FIRST]; cur; cur = SPR(cur)[SP_NEXT]) {
        if (n == MAXS || !heap_is_object(vm, cur)) {
            plat_warn_once(vm, W_PF_LIST, "PlayField.draw: sprite list too long or corrupt; "
                                          "not sorted");
            return;
        }
        list[n++] = cur;
    }
    /* insertion sort: stable, and nearly free on the already-sorted lists games keep */
    for (int i = 1; i < n; i++) {
        ref_t r = list[i];
        int32_t z = (int32_t)SPR(r)[SP_Z];
        int j = i;
        while (j > 0 && (int32_t)SPR(list[j - 1])[SP_Z] > z) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = r;
    }
    for (int i = 0; i < n; i++) {
        uint32_t *sp = SPR(list[i]);
        sp[SP_PREV] = i > 0 ? list[i - 1] : 0;
        sp[SP_NEXT] = i + 1 < n ? list[i + 1] : 0;
    }
    pf[PF_FIRST] = list[0];
    pf[PF_LAST] = list[n - 1];
#undef SPR
}

/* addSprite 0x427f95: appends at the tail. A sprite with a next or prev link is "already added"
 * (a sprite alone in a list has neither, and is relinked). */
static int n_PlayField_addSprite(ExenVM *vm, uint32_t *a)
{
    uint32_t *pf = fields(vm, a[0], PC_PLAYFIELD);
    ref_t s = a[1];
    if (!s) {
        vm_log(vm, "Playfield addSprite: sprite ptr is null.");
        vm_halt(vm, "PlayField.addSprite(null) (non catchable interrupt 0)");
        return 0;
    }
    uint32_t *sp = fields(vm, s, PC_SPRITE);
    if (!pf || !sp)
        return 0;
    if (sp[SP_NEXT] || sp[SP_PREV]) {
        vm_log(vm, "Sprite ptr has already been added");
        return 0;
    }
    if (!pf[PF_FIRST] || pf[PF_FIRST] == s) {
        pf[PF_FIRST] = s;
        sp[SP_PREV] = 0;
    } else {
        uint32_t *lp = fields(vm, pf[PF_LAST], PC_SPRITE);
        if (!lp) {
            /* first set but last null: the original dereferences null */
            plat_warn_once(vm, W_PF_LIST, "PlayField.addSprite: list has a head but no tail");
            return 0;
        }
        lp[SP_NEXT] = s;
        sp[SP_PREV] = pf[PF_LAST];
    }
    pf[PF_LAST] = s;
    sp[SP_NEXT] = 0;
    return 0;
}

/* removeSprite 0x4280bd. */
static int n_PlayField_removeSprite(ExenVM *vm, uint32_t *a)
{
    uint32_t *pf = fields(vm, a[0], PC_PLAYFIELD);
    ref_t s = a[1];
    if (!s) {
        vm_log(vm, "Playfield removeSprite: Sprite ptr is null");
        vm_halt(vm, "PlayField.removeSprite(null) (non catchable interrupt 0)");
        return 0;
    }
    uint32_t *sp = fields(vm, s, PC_SPRITE);
    int cs = plat_class(vm, PC_SPRITE);
    if (!pf || !sp)
        return 0;
    ref_t p = sp[SP_PREV], n = sp[SP_NEXT], it = pf[PF_FIRST];
    long guard = 0;
    while (it && it != s && ++guard < LIST_LIMIT)
        it = inst(vm, it, cs)[SP_NEXT];
    if (it != s) {
        vm_log(vm, "Sprite ptr has already been removed");
        return 0;
    }
    if (!p)
        pf[PF_FIRST] = n;
    else
        inst(vm, p, cs)[SP_NEXT] = n;
    if (n)
        inst(vm, n, cs)[SP_PREV] = p;
    if (pf[PF_LAST] == s)
        pf[PF_LAST] = p;
    sp[SP_NEXT] = 0;
    sp[SP_PREV] = 0;
    return 0;
}

/* removeAllSprite 0x428208. */
static int n_PlayField_removeAllSprite(ExenVM *vm, uint32_t *a)
{
    uint32_t *pf = fields(vm, a[0], PC_PLAYFIELD);
    int cs = plat_class(vm, PC_SPRITE);
    if (!pf)
        return 0;
    ref_t it = pf[PF_FIRST];
    long guard = 0;
    while (it && ++guard < LIST_LIMIT) {
        uint32_t *sp = inst(vm, it, cs);
        ref_t nx = sp[SP_NEXT];
        sp[SP_PREV] = 0;
        sp[SP_NEXT] = 0;
        it = nx;
    }
    pf[PF_FIRST] = 0;
    pf[PF_LAST] = 0;
    return 0;
}

/* ============================================================ PlayField.draw */

/* rectIntersect 0x4177c6: half-open, the result w/h may be negative. */
static void rect_intersect(int32_t r[4], const int32_t c[4])
{
    int64_t r0 = (int64_t)r[0] + r[2], r1 = (int64_t)r[1] + r[3];
    int64_t c0 = (int64_t)c[0] + c[2], c1 = (int64_t)c[1] + c[3];
    if (r[0] < c[0]) r[0] = c[0];
    if (r[1] < c[1]) r[1] = c[1];
    r[2] = (int32_t)((r0 < c0 ? r0 : c0) - r[0]);
    r[3] = (int32_t)((r1 < c1 ? r1 : c1) - r[1]);
}

/* The tile pass of PlayField.draw. Column-major, from the first visible cell to the edge of the
 * destination image or of the map (the clip trims the view). Returns 1 if the tile sheet is
 * narrower than a tile (the original logs and returns without restoring anything else). */
static int draw_tiles(ExenVM *vm, uint32_t *pf, EXimg *dst, EXimg *cs, int32_t ox, int32_t oy)
{
    int32_t state = (int32_t)pf[PF_STATE];
    int32_t charW = (int32_t)pf[PF_CHARW], charH = (int32_t)pf[PF_CHARH];
    int32_t W = (int32_t)pf[PF_BGW], H = (int32_t)pf[PF_BGH];
    int32_t col0, px0, row0, py0;
    if (ox < 0) {
        col0 = qdiv(wsub(0, ox), charW);
        px0 = qmod(ox, charW);
    } else {
        col0 = 0;
        px0 = ox;
    }
    if (oy < 0) {
        row0 = qdiv(wsub(0, oy), charH);
        py0 = qmod(oy, charH);
    } else {
        row0 = 0;
        py0 = oy;
    }
    int32_t dw = dst->w, dh = dst->h;
    if ((state & 6) != 6 || col0 >= W || row0 >= H || px0 >= dw || py0 >= dh)
        return 0;
    /* tiles per sheet row, from the Java Image.width field */
    const uint32_t *ci = fields(vm, pf[PF_CHARSET], PC_IMAGE);
    int32_t tpr = qdiv(ci ? (int32_t)ci[I_WIDTH] : 0, charW);
    if (tpr == 0)
        return 1;
    int32_t px = px0, cx = col0;
    do {
        int32_t py = py0, cy = row0;
        do {
            int32_t t = cell_get(vm, pf, cx, cy);
            if (t < 0) {
                int32_t m = wadd((int32_t)pf[PF_ANIMTILEMAX], 1);
                ref_t ati = pf[PF_ANIMTILEINDEX];
                if (m == 0) {
                    vm_halt(vm, "PlayField.draw: animTileMaxIndex is -1 (division by zero in the "
                                "original)");
                    return 0;
                }
                t = qmod(wsub(0, t), m);
                if (is_arr(vm, ati, T_SHORT) && t >= 0 && t < arr_len(vm, ati)) {
                    t = (int16_t)rd16(arr_data(vm, ati) + 2 * (uint32_t)t);
                } else {
                    plat_warn_once(vm, W_PF_ANIMTILE, "PlayField.draw: animated tile %d outside "
                                                      "animTileIndex (the original reads past "
                                                      "it); drawn as empty", t);
                    t = 0;
                }
            }
            if (t != 0) {
                t -= 1;
                if (!(state & PF_CHARSET_OPACITY)) {
                    /* mode 1 */
                    eximg_draw_image(vm, dst, px, py, charW, charH, cs, wmul(qmod(t, tpr), charW),
                                     wmul(qdiv(t, tpr), charH), charW, charH, 0);
                } else if (state & PF_CHARSET_WITHMASK) {
                    /* tile 2t is the image, 2t+1 its mask; mask first (modes 4 then 3, or 3
                       then 4 with CHARSET_INVERSEVIDEO; no effect) */
                    int32_t t2 = wmul(t, 2);
                    int32_t sx = wmul(qmod(t2, tpr), charW), sy = wmul(qdiv(t2, tpr), charH);
                    eximg_draw_image(vm, dst, px, py, charW, charH, cs, wadd(sx, charW), sy, charW,
                                     charH, 0);
                    eximg_draw_image(vm, dst, px, py, charW, charH, cs, sx, sy, charW, charH, 0);
                } else {
                    /* mode 3, or 4 with CHARSET_INVERSEVIDEO (no effect) */
                    eximg_draw_image(vm, dst, px, py, charW, charH, cs, wmul(qmod(t, tpr), charW),
                                     wmul(qdiv(t, tpr), charH), charW, charH, 0);
                }
            }
            py = wadd(py, charH);
            cy++;
        } while (py < dh && cy < H);
        px = wadd(px, charW);
        cx++;
    } while (px < dw && cx < W);
    return 0;
}

/* draw(Graphics g, int x, int y) 0x427531. Field pixel (fx, fy) lands at screen
 * (fx - x + viewX, fy - y + viewY). */
static int n_PlayField_draw(ExenVM *vm, uint32_t *a)
{
    uint32_t *pf = fields(vm, a[0], PC_PLAYFIELD);
    ref_t g = a[1];
    uint32_t *gf = fields(vm, g, PC_GRAPHICS);
    if (!g || !pf || !gf)
        return 0;
    EXimg dst, cs;
    int have_dst = eximg_get(vm, gf[G_PIXIMA], &dst);
    int have_cs = eximg_get(vm, pf[PF_CHARSET], &cs);
    int32_t ox = wsub((int32_t)pf[PF_VIEWX], ARG(2)), oy = wsub((int32_t)pf[PF_VIEWY], ARG(3));

    /* clip the Graphics to the view window for the duration of the call */
    uint32_t saved[4];
    memcpy(saved, gf + G_CLIPX, sizeof saved);
    int32_t clip[4], view[4];
    for (int i = 0; i < 4; i++) {
        clip[i] = (int32_t)saved[i];
        view[i] = (int32_t)pf[PF_VIEWX + i];
    }
    rect_intersect(clip, view);
    for (int i = 0; i < 4; i++)
        gf[G_CLIPX + i] = (uint32_t)clip[i];

    if (pf[PF_BACKGROUND] && pf[PF_CHARW] && pf[PF_CHARH]) {
        if (!have_cs || !have_dst) {
            /* fatal in the original (0x407a13), the clip is not restored */
            vm_log(vm, "Playfield Draw: image ptr is null.");
            vm_halt(vm, "PlayField.draw: image ptr is null (non catchable interrupt 0)");
            return 0;
        }
        eximg_sync_clip(vm, g, &dst);
        eximg_put(vm, &dst);
        if (draw_tiles(vm, pf, &dst, &cs, ox, oy)) {
            vm_log(vm, "exen_PlayField_draw, Param charNW=0, Division by zero rejected. "
                       "exiting function");
            memcpy(gf + G_CLIPX, saved, sizeof saved);
            return 0;
        }
        if (vm->halted)
            return 0;
    }

    if (pf[PF_FIRST]) {
        int csp = plat_class(vm, PC_SPRITE);
        playfield_sort_sprites(vm, pf);
        long guard = 0;
        for (ref_t s = pf[PF_FIRST]; s && ++guard < LIST_LIMIT && !vm->halted;
             s = inst(vm, s, csp)[SP_NEXT]) {
            const uint32_t *sp = inst(vm, s, csp);
            if (!sp[SP_VISIBLE])
                continue;
            ref_t an = sp[SP_ANIM];
            if (!an || !heap_is_object(vm, an)) {
                plat_warn_once(vm, W_AB_ANIM, "PlayField.draw: visible sprite without an "
                                              "animation (the original dereferences null)");
                continue;
            }
            uint32_t h = class_hash_of(vm, an);
            if (h == 0xe7167d52u) /* exen.AnimBitmap */
                ab_draw(vm, an, g, wadd(ox, (int32_t)sp[SP_X]), wadd(oy, (int32_t)sp[SP_Y]),
                        (int32_t)sp[SP_FRAME], (int32_t)sp[SP_TRANSFORM]);
            /* exen.AnimFlash (0xd414954a) goes to the no-op native 62; other classes are not
               drawn */
        }
    }
    memcpy(gf + G_CLIPX, saved, sizeof saved);
    return 0;
}

/* ============================================================ AnimFlash (stubs in gensimu) */

static int n_ret0(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = 0; return 1; }
static int n_ret1(ExenVM *vm, uint32_t *a) { (void)vm; a[0] = 1; return 1; }
static int n_void(ExenVM *vm, uint32_t *a) { (void)vm; (void)a; return 0; }

static int n_AnimFlash_getWidth(ExenVM *vm, uint32_t *a)
{
    const uint32_t *f = fields(vm, a[0], PC_ANIMFLASH);
    a[0] = f ? f[AF_WIDTH] : 0;
    return 1;
}

static int n_AnimFlash_getHeight(ExenVM *vm, uint32_t *a)
{
    const uint32_t *f = fields(vm, a[0], PC_ANIMFLASH);
    a[0] = f ? f[AF_HEIGHT] : 0;
    return 1;
}

/* ============================================================ Displayable */

/* Command position mapping 0x403f56 (an EXManuf query in a real port): 0 -> 2, 1 -> 1, else 0. */
static int cmd_pos(int32_t pos) { return pos == 0 ? 2 : pos == 1 ? 1 : 0; }

/* haveDisplayableCommand 0x424a60: cmdpos(0) || cmdpos(1), always true in gensimu. */
static int n_Displayable_haveDisplayableCommand(ExenVM *vm, uint32_t *a)
{
    a[0] = (cmd_pos(0) || cmd_pos(1)) ? 1 : 0;
    (void)vm;
    return 1;
}

/* drawCommand(Graphics g, String label, int pos) 0x424a9d -> 0x4238f0. The target image's
 * clip is set to the whole image for the call (and restored; the Graphics clip is not used).
 * The label is drawn in the built-in font, colour 0, on the bottom row y = h - 9, cut to the
 * characters that fit in the width (w / 6). Alignment from cmdpos(pos): 1 -> x = 1 (pos 1),
 * 2 -> right-aligned x = w - n*6 (pos 0), 3 -> centred (w - n*6) / 2 (not reachable), 0 -> not
 * drawn. */
static int n_Displayable_drawCommand(ExenVM *vm, uint32_t *a)
{
    uint32_t *gf = fields(vm, a[1], PC_GRAPHICS);
    EXimg e;
    if (!gf || !eximg_get(vm, gf[G_PIXIMA], &e))
        return 0;
    if (!a[2]) {
        plat_warn_once(vm, W_DC_LABEL, "Displayable.drawCommand: null label (the original "
                                       "dereferences null)");
        return 0;
    }
    const uint8_t *chars;
    int32_t len = vm_string_get(vm, a[2], &chars);
    if (!chars)
        return 0;
    int align = cmd_pos(ARG(3));
    e.clipX = 0;
    e.clipY = 0;
    e.clipW = e.w;
    e.clipH = e.h;
    int32_t y = e.h - 9, W = e.w;
    int32_t fit = W / 6;
    int32_t n = (uint32_t)fit < (uint32_t)len ? fit : len;
    if (n < 1 || y < 0)
        return 0;
    int32_t x;
    switch (align) {
    case 1: x = 1; break;
    case 2: x = W - n * 6; break;
    case 3: x = (W - n * 6) / 2; break;
    default: return 0;
    }
    eximg_draw_text(vm, &e, x, y, chars, n, 0);
    return 0;
}

/* ============================================================ registration */

void playfield_register(void)
{
    natives_register(43, n_AnimBitmap_draw);
    natives_register(44, n_AnimBitmap_moduloFrame);
    natives_register(45, n_AnimBitmap_getRealFrame);
    natives_register(46, n_PlayField_fillCells);
    natives_register(47, n_PlayField_moveTiles);
    natives_register(48, n_PlayField_draw);
    natives_register(49, n_PlayField_setCellTile);
    natives_register(50, n_PlayField_getCellTile);
    natives_register(51, n_PlayField_addSprite);
    natives_register(52, n_PlayField_removeSprite);
    natives_register(53, n_PlayField_removeAllSprite);
    natives_register(54, n_ret0);  /* AnimFlash.initAnimFlash([B)I */
    natives_register(55, n_void);  /* draw(Graphics,Z)V */
    natives_register(56, n_void);  /* delete()V */
    natives_register(57, n_void);  /* setFrame(I)V */
    natives_register(58, n_ret1);  /* getNbFrames()I */
    natives_register(59, n_ret0);  /* getNbLoops()I */
    natives_register(60, n_void);  /* setPosition(II)V */
    natives_register(61, n_void);  /* setSize(II)V */
    natives_register(62, n_void);  /* draw(Graphics,IIII)V */
    natives_register(63, n_AnimFlash_getWidth);
    natives_register(64, n_AnimFlash_getHeight);
    natives_register(65, n_Displayable_haveDisplayableCommand);
    natives_register(66, n_Displayable_drawCommand);
}
