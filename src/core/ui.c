/* exen.DialogBox (101-102) and exen.List (109): the framework's text box and menu list.
 *
 * Translated from gensimu.exe (docs/re/NATIVES.md §2.3): getNbLinesFormatted 0x424940 ->
 * 0x4231f6, drawBox 0x42497c -> 0x4233ed (title 0x423804, justification 0x4237a2, "more"
 * test 0x423751), drawList 0x426870 -> 0x41b950 (window 0x41bbea, branch rule 0x41bae8).
 * Everything is drawn with the built-in 5x8 font (advance 6, line height 9).
 *
 * Quirks kept:
 * - Both natives start by clearing the target: a white fill of (0, 0, clipW, clipH), the clip
 *   size but not its position (8bpp ops slot 10, 0x42c2e1).
 * - Word wrap (0x4232ee) breaks at spaces and '\n', counts at most width/6 characters per
 *   line, keeps the space that ends a line in the line, and splits a word longer than a line.
 *   The line count gets one extra line when the screen height is a multiple of 10 (0x4231f6
 *   tests the profile's maximum LCD height, 0x45c7c8).
 * - The text stops one line early when a title is shown (the bottom test adds the title
 *   height twice).
 * - The scroll arrow is the simulator's 8x4 1-bit image at 0x456d70 (a black down-pointing
 *   triangle on white), drawn opaque.
 * The wrap reads one byte past the end of the string, as the original does; we read 0 there.
 */
#include <string.h>

#include "extnatives.h"

enum { DB_TITLE, DB_TEXT, DB_SPRITE, DB_SCROLL, DB_MAXSCROLL, DB_STARTY, DB_CURSOR };
#define DB_JUSTIFY 13
enum { L_CAPTIONS, L_PROPS, L_CURRENT };

#define FONT_ADV 6   /* glyph width 5 + 1 */
#define FONT_H 9     /* charHeight */

typedef struct {
    const uint8_t *s;
    uint32_t n;
} Str;

static uint8_t sc(const Str *t, uint32_t i) { return t->s && i < t->n ? t->s[i] : 0; }

static Str jstring(ExenVM *vm, ref_t s)
{
    Str t = {NULL, 0};
    const uint8_t *c;
    int n = vm_string_get(vm, s, &c);
    if (c) {
        t.s = c;
        t.n = (uint32_t)n;
    }
    return t;
}

/* ------------------------------------------------------------ drawing helpers */

static void fill(ExenVM *vm, EXimg *e, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c)
{
    uint32_t len;
    uint8_t *d = eximg_data(vm, e, &len);
    for (int32_t j = y; j < y + h; j++)
        for (int32_t i = x; i < x + w; i++) {
            int64_t k = (int64_t)j * e->pitch + i;
            if (i >= 0 && j >= 0 && k >= 0 && k < len)
                d[k] = c;
        }
}

/* ops->clear (0x42c2e1): the rectangle (0, 0, clipW, clipH), white */
static void clear(ExenVM *vm, EXimg *e)
{
    if (e->ops == 8)
        fill(vm, e, 0, 0, e->clipW, e->clipH, 0xff);
}

/* fillRectCore (0x417d92) */
static void fill_rect(ExenVM *vm, EXimg *e, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t c)
{
    int64_t x0 = x > e->clipX ? x : e->clipX, y0 = y > e->clipY ? y : e->clipY;
    int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x1 > (int64_t)e->clipX + e->clipW) x1 = (int64_t)e->clipX + e->clipW;
    if (y1 > (int64_t)e->clipY + e->clipH) y1 = (int64_t)e->clipY + e->clipH;
    if (x1 - x0 <= 0 || y1 - y0 <= 0 || e->ops != 8)
        return;
    fill(vm, e, (int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0), c);
}

static void put_clipped(ExenVM *vm, EXimg *e, int32_t x, int32_t y, uint8_t c)
{
    if (x < e->clipX || y < e->clipY || (int64_t)x >= (int64_t)e->clipX + e->clipW ||
        (int64_t)y >= (int64_t)e->clipY + e->clipH || e->ops != 8)
        return;
    fill(vm, e, x, y, 1, 1, c);
}

static void text(ExenVM *vm, EXimg *e, int32_t x, int32_t y, const Str *t, uint32_t from,
                 int32_t len, uint8_t c)
{
    if (!t->s || len <= 0)
        return;
    /* the font reads `len` bytes; stay inside the string */
    if (from >= t->n)
        return;
    if ((uint32_t)len > t->n - from)
        len = (int32_t)(t->n - from);
    eximg_draw_text(vm, e, x, y, t->s + from, len, c);
}

/* ------------------------------------------------------------ word wrap */

/* 0x4232ee: characters of the line starting at p, at most max of them */
static uint32_t wrap(const Str *t, uint32_t p, uint32_t len, uint32_t max, int depth)
{
    uint32_t i = 0;
    while (sc(t, p) != ' ' && sc(t, p) != '\n' && i < len && i < max) {
        p++;
        i++;
    }
    if (sc(t, p) == '\n')
        return i + 1;
    if (sc(t, p) == ' ')
        p++;
    if (i < len && p > 0 && sc(t, p - 1) == ' ' && depth < 4096) {
        uint32_t r = wrap(t, p, len - i, max - i - 1, depth + 1);
        uint32_t tt = r + i + 1;
        if (tt < max)
            return tt > len ? len : tt;
        return i;
    }
    return i;
}

/* 0x4232b3: one line at a pixel width */
static uint32_t line_len(const Str *t, uint32_t p, uint32_t len, int32_t width)
{
    return wrap(t, p, len, (uint32_t)(width / FONT_ADV), 0);
}

/* 0x4231f6: number of lines, or (limit >= 0) the offset where line `limit` starts */
static int32_t count_lines(ExenVM *vm, const Str *t, uint32_t len, int32_t width, int32_t limit)
{
    uint32_t p = 0, lines = 0;
    if (!t->s)
        return -1;
    for (uint32_t guard = 0; guard <= 0x10000; guard++) { /* a zero-width line would hang */
        uint32_t n = line_len(t, p, len, width);
        if ((uint32_t)limit == lines)
            return (int32_t)p;
        lines++;
        p += n;
        len -= n;
        if (len == 0)
            break;
        if (sc(t, p) == ' ') {
            p++;
            len--;
            if (len == 0)
                break;
        }
    }
    if (vm->plat->height % 10 == 0)
        lines++;
    return (int32_t)lines;
}

/* 0x4237a2: x of a line of n characters: 1 centred, 2 right, else left */
static int32_t justify(int32_t mode, int32_t n, int32_t width)
{
    if (mode == 1)
        return (width - n * FONT_ADV) / 2;
    if (mode == 2)
        return width - n * FONT_ADV;
    return 0;
}

/* ------------------------------------------------------------ DialogBox */

static uint32_t *db_fields(ExenVM *vm, ref_t self) { return inst(vm, self, ext_class(vm, EC_DIALOGBOX)); }

static int n_getNbLinesFormatted(ExenVM *vm, uint32_t *a) /* 101 */
{
    if (!a[1]) {
        vm_throw_hash(vm, H_NPE);
        a[0] = 0;
        return 1;
    }
    Str t = jstring(vm, a[1]);
    a[0] = (uint32_t)count_lines(vm, &t, t.n, (int32_t)a[2], -1);
    return 1;
}

/* the 8x4 1-bit arrow at 0x456d70: bit 0 = black, bit 1 = white */
static const uint8_t arrow_bits[4] = {0x01, 0x83, 0xc7, 0xef};

/* 102 drawBox(Graphics g, int width, int height, int scrollLine, boolean cursor) */
static int n_drawBox(ExenVM *vm, uint32_t *a)
{
    int cg = plat_class(vm, PC_GRAPHICS);
    EXimg D;
    if (!a[1] || cg < 0 || !eximg_get(vm, inst(vm, a[1], cg)[0], &D))
        return 0;
    const uint32_t *f = db_fields(vm, a[0]);
    Str title = jstring(vm, f[DB_TITLE]), body = jstring(vm, f[DB_TEXT]);
    int32_t startY = (int32_t)f[DB_STARTY], just = (int32_t)f[DB_JUSTIFY];
    int32_t W = (int32_t)a[2], H = (int32_t)a[3], scroll = (int32_t)a[4];
    int cursor = a[5] != 0;
    eximg_sync_clip(vm, a[1], &D);
    eximg_put(vm, &D);
    clear(vm, &D);

    /* 0x4233ed */
    int32_t skip = scroll - startY / 10;
    if (skip < 0)
        skip = 0;
    int32_t off = count_lines(vm, &body, body.n, W, skip);
    int32_t total = count_lines(vm, &body, body.n, W, -1);
    int32_t y = 0, lineh = FONT_H + 1;
    if (!body.s)
        return 0;
    if (title.s && title.n) { /* 0x423804 */
        uint32_t fit = (uint32_t)(D.w / FONT_ADV);
        int32_t n = (int32_t)(fit >= title.n ? title.n : fit);
        int32_t x = justify(1, n, D.w);
        fill_rect(vm, &D, 0, 0, D.w, FONT_H, 0x03);
        text(vm, &D, x, 1, &title, 0, n, 0xff);
        text(vm, &D, x + 1, 1, &title, 0, n, 0xff);
        y = lineh;
    }
    int32_t ty = y + startY - scroll * 10;
    if (ty <= y)
        ty = y;
    uint32_t p = (uint32_t)off, len = body.n - (uint32_t)off;
    while (len != 0 && ty + y + 1 < H) {
        uint32_t n = line_len(&body, p, len, W);
        int32_t drawn = (int32_t)n;
        if (sc(&body, p + n - 1) == '\n')
            drawn--;
        int32_t x = justify(just, drawn, W);
        text(vm, &D, x, ty, &body, p, drawn, 0x00);
        p += n;
        len -= n;
        if (len == 0)
            break;
        if (sc(&body, p) == ' ') {
            p++;
            len--;
            if (len == 0)
                break;
        }
        ty += lineh;
    }
    int32_t lines = total + startY / 10;
    int32_t last = lines - (H / 10 - 1);
    if (last < 0)
        last = 0;
    if (last != scroll && cursor) { /* drawImageCore of the arrow, opaque */
        int32_t ax = justify(1, 1, W), ay = H - 4 - 1;
        for (int32_t j = 0; j < 4; j++)
            for (int32_t i = 0; i < 8; i++)
                put_clipped(vm, &D, ax + i, ay + j, (arrow_bits[j] >> (7 - i)) & 1 ? 0xff : 0x00);
    }
    return 0;
}

/* ------------------------------------------------------------ List */

/* 109 drawList(Graphics g, int width, int height) */
static int n_drawList(ExenVM *vm, uint32_t *a)
{
    int cg = plat_class(vm, PC_GRAPHICS), cl = ext_class(vm, EC_LIST);
    EXimg D;
    if (!a[1] || cg < 0 || cl < 0 || !eximg_get(vm, inst(vm, a[1], cg)[0], &D))
        return 0;
    const uint32_t *f = inst(vm, a[0], cl);
    ref_t caps = f[L_CAPTIONS], props = f[L_PROPS];
    if (!caps) {
        vm_throw_hash(vm, H_NPE);
        return 0;
    }
    uint32_t count = arr_len(vm, caps), first = 0, current = f[L_CURRENT];
    eximg_sync_clip(vm, a[1], &D);
    eximg_put(vm, &D);

    /* 0x41b950 */
    int32_t W = (int32_t)a[2], rowh = FONT_H + 2;
    uint32_t rows = (uint32_t)((int32_t)a[3] / rowh);
    int32_t yy = 0, barh = rowh - 1;
    if (current > 3) { /* 0x41bbea: keep the current item on the fourth row */
        first = current - 3;
        count -= first;
        current = 3;
    }
    uint32_t n = rows < count ? rows : count;
    clear(vm, &D);
    uint32_t ncaps = arr_len(vm, caps), nprops = props ? arr_len(vm, props) : 0;
    for (uint32_t i = 0; i < n && i < 0x10000; i++) {
        uint32_t k = first + i;
        Str t = {NULL, 0};
        int32_t prop = 0;
        if (k < ncaps)
            t = jstring(vm, rd32(arr_data(vm, caps) + 4 * k));
        if (k < nprops)
            prop = (int32_t)rd32(arr_data(vm, props) + 4 * k);
        int32_t xo = 0;
        uint8_t col = 2;
        if (prop == 1) { /* LEAF */
            xo = 5;
            col = 0;
        }
        if (current == i) {
            fill_rect(vm, &D, 0, yy, W, barh + 2, 0x10);
            col = 0xff;
        }
        text(vm, &D, xo, yy + 2, &t, 0, (int32_t)t.n, col);
        text(vm, &D, xo + 1, yy + 2, &t, 0, (int32_t)t.n, col);
        yy += rowh;
        if (prop == 0) { /* BRANCH: a rule that breaks up towards the right (0x41bae8) */
            int32_t y = yy - 1, x0 = 0, x1 = D.w - 25;
            static const int seg[5][2] = {{0, 0}, {2, 3}, {2, 2}, {2, 1}, {2, 0}};
            for (int s = 0; s < 5; s++) {
                if (s) {
                    x0 = x1 + seg[s][0];
                    x1 = x0 + seg[s][1];
                }
                for (int32_t x = x0 < x1 ? x0 : x1; x <= (x0 < x1 ? x1 : x0); x++)
                    put_clipped(vm, &D, x, y, 0x00);
            }
            yy++;
        }
    }
    return 0;
}

void ui_register(void)
{
    natives_register(101, n_getNbLinesFormatted);
    natives_register(102, n_drawBox);
    natives_register(109, n_drawList);
}
