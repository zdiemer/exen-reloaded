/* In-window UI: microui rendered with the SDL renderer (ui.h). */
#include "ui.h"

#include "exen.h"
#include "font8x8.h"
#include "microui.h"
#include "persist.h"
#include "rgb332.h"

/* ---------------------------------------------------------------- font */

/* Glyphs beyond ASCII, for the few non-ASCII characters the menus and messages use. In the
 * atlas they follow the 95 ASCII glyphs. Bit 0 of each row is the leftmost pixel. */
enum { G_ASCII = FONT8X8_LAST - FONT8X8_FIRST + 1, G_ELLIPSIS = G_ASCII, G_TIMES, G_TRIANGLE,
       G_EMDASH, G_CHECK, G_BULLET, G_DOWN, G_COUNT };
static const uint8_t extra_glyphs[G_COUNT - G_ASCII][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0x00}, /* U+2026 ellipsis */
    {0x00, 0x22, 0x14, 0x08, 0x14, 0x22, 0x00, 0x00}, /* U+00D7 multiplication sign */
    {0x00, 0x04, 0x0C, 0x1C, 0x3C, 0x1C, 0x0C, 0x04}, /* U+25B8 small right triangle */
    {0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00}, /* U+2014 em dash (and U+2013) */
    {0x00, 0xC0, 0x60, 0x31, 0x1B, 0x0E, 0x04, 0x00}, /* U+2713 check mark */
    {0x00, 0x00, 0x18, 0x3C, 0x3C, 0x18, 0x00, 0x00}, /* U+2022 bullet */
    {0x00, 0x00, 0x7E, 0x3C, 0x18, 0x00, 0x00, 0x00}, /* U+25BE small down triangle */
};

/* Decode one UTF-8 character of [*s, end) into a glyph index. */
static int next_glyph(const char **s, const char *end)
{
    unsigned char c = (unsigned char)**s;
    (*s)++;
    if (c < 0x80)
        return c >= FONT8X8_FIRST && c <= FONT8X8_LAST ? c - FONT8X8_FIRST : '?' - FONT8X8_FIRST;
    int n = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (n < 0)
        return '?' - FONT8X8_FIRST;
    unsigned cp = c & (0x3f >> n);
    while (n-- > 0 && *s < end && ((unsigned char)**s & 0xc0) == 0x80)
        cp = cp << 6 | ((unsigned char)*(*s)++ & 0x3f);
    switch (cp) {
    case 0x2026: return G_ELLIPSIS;
    case 0x00d7: return G_TIMES;
    case 0x25b8: case 0x25b6: return G_TRIANGLE;
    case 0x2014: case 0x2013: return G_EMDASH;
    case 0x2713: return G_CHECK;
    case 0x2022: return G_BULLET;
    case 0x25be: return G_DOWN;
    case 0x00a0: return ' ' - FONT8X8_FIRST;
    default: return '?' - FONT8X8_FIRST;
    }
}

static int text_len(const char *s, int len)
{
    const char *end = s + (len < 0 ? (int)SDL_strlen(s) : len);
    int n = 0;
    while (s < end) {
        next_glyph(&s, end);
        n++;
    }
    return n;
}

static int text_width(mu_Font font, const char *s, int len)
{
    (void)font;
    return 8 * text_len(s, len);
}

static int text_height(mu_Font font)
{
    (void)font;
    return 8;
}

/* ---------------------------------------------------------------- state */

#define UI_ICON_RADIO MU_ICON_MAX
#define UI_ICON_ARROW (MU_ICON_MAX + 1)
#define UI_ICON_THUMB0 100 /* + slot: the slot's thumbnail */

#define MENU_DEPTH 3
#define MENU_PAD 4

struct Ui {
    SDL_Renderer *ren;
    UiHost host;
    mu_Context *mu;
    SDL_Texture *font;
    int s;          /* output pixels per microui unit */
    int W, H;       /* output size in microui units */
    int panel;
    int opening;    /* first frame of a panel: (re)open its window */

    /* menu strip */
    int strip, pinned, shown;
    int top;                         /* highlighted top-level menu, -1 none */
    int depth;                       /* drop-downs open, 0..MENU_DEPTH */
    const MenuItem *items[MENU_DEPTH];
    int sel[MENU_DEPTH];             /* selected item at each level, -1 none */
    int sel_y[MENU_DEPTH];           /* its y, where a submenu opens */
    mu_Rect top_rect[8];
    mu_Rect drop_rect[MENU_DEPTH];
    int alt_armed;

    /* panels */
    int row, col, capture, scroll_to;
    char msg[200];
    int scroll_by;
    PersistSlotInfo info[PERSIST_NSLOTS];
    SDL_Texture *thumb[PERSIST_NSLOTS];
    int confirm_delete;
    uint32_t lut[256];
};

static mu_Color rgba(int r, int g, int b, int a)
{
    return mu_color(r, g, b, a);
}

Ui *ui_create(SDL_Renderer *ren, const UiHost *host)
{
    Ui *ui = SDL_calloc(1, sizeof *ui);
    if (!ui)
        return NULL;
    ui->mu = SDL_calloc(1, sizeof *ui->mu);
    if (!ui->mu) {
        SDL_free(ui);
        return NULL;
    }
    ui->ren = ren;
    ui->host = *host;
    ui->top = -1;
    ui->confirm_delete = -1;
    exen_rgb332_lut(ui->lut);
    mu_init(ui->mu);
    ui->mu->text_width = text_width;
    ui->mu->text_height = text_height;
    mu_Style *st = ui->mu->style;
    st->size = mu_vec2(60, 8);
    st->padding = 3;
    st->spacing = 2;
    st->indent = 12;
    st->title_height = 14;
    st->scrollbar_size = 6;
    st->thumb_size = 6;
    st->colors[MU_COLOR_TEXT] = rgba(232, 228, 230, 255);
    st->colors[MU_COLOR_BORDER] = rgba(18, 14, 16, 255);
    st->colors[MU_COLOR_WINDOWBG] = rgba(40, 34, 38, 246);
    st->colors[MU_COLOR_TITLEBG] = rgba(150, 30, 60, 255);
    st->colors[MU_COLOR_TITLETEXT] = rgba(255, 255, 255, 255);
    st->colors[MU_COLOR_PANELBG] = rgba(30, 26, 29, 255);
    st->colors[MU_COLOR_BUTTON] = rgba(62, 54, 59, 255);
    st->colors[MU_COLOR_BUTTONHOVER] = rgba(84, 72, 79, 255);
    st->colors[MU_COLOR_BUTTONFOCUS] = rgba(200, 40, 75, 255);
    st->colors[MU_COLOR_BASE] = rgba(26, 22, 25, 255);
    st->colors[MU_COLOR_BASEHOVER] = rgba(36, 31, 34, 255);
    st->colors[MU_COLOR_BASEFOCUS] = rgba(44, 38, 42, 255);
    st->colors[MU_COLOR_SCROLLBASE] = rgba(30, 26, 29, 255);
    st->colors[MU_COLOR_SCROLLTHUMB] = rgba(110, 96, 104, 255);

    /* font atlas: G_COUNT glyphs of 8x8, white on transparent, tinted per text command */
    uint32_t *px = SDL_calloc((size_t)G_COUNT * 64, sizeof *px);
    if (px) {
        for (int g = 0; g < G_COUNT; g++) {
            const uint8_t *rows = g < G_ASCII ? font8x8[g] : extra_glyphs[g - G_ASCII];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    if (rows[y] >> x & 1)
                        px[y * G_COUNT * 8 + g * 8 + x] = 0xffffffffu;
        }
        ui->font = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, G_COUNT * 8, 8);
        if (ui->font) {
            SDL_UpdateTexture(ui->font, NULL, px, G_COUNT * 8 * 4);
            SDL_SetTextureBlendMode(ui->font, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(ui->font, SDL_SCALEMODE_NEAREST);
        }
        SDL_free(px);
    }
    return ui;
}

static void free_thumbs(Ui *ui)
{
    for (int i = 0; i < PERSIST_NSLOTS; i++) {
        if (ui->thumb[i])
            SDL_DestroyTexture(ui->thumb[i]);
        ui->thumb[i] = NULL;
    }
}

void ui_destroy(Ui *ui)
{
    if (!ui)
        return;
    free_thumbs(ui);
    if (ui->font)
        SDL_DestroyTexture(ui->font);
    SDL_free(ui->mu);
    SDL_free(ui);
}

/* ---------------------------------------------------------------- drawing */

static void draw_text(Ui *ui, int x, int y, int s, mu_Color c, const char *str)
{
    if (!ui->font)
        return;
    SDL_SetTextureColorMod(ui->font, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(ui->font, c.a);
    const char *end = str + SDL_strlen(str);
    while (str < end) {
        int g = next_glyph(&str, end);
        SDL_FRect src = {(float)(g * 8), 0, 8, 8};
        SDL_FRect dst = {(float)x, (float)y, (float)(8 * s), (float)(8 * s)};
        SDL_RenderTexture(ui->ren, ui->font, &src, &dst);
        x += 8 * s;
    }
}

static void draw_glyph_in(Ui *ui, int glyph, mu_Rect r, mu_Color c)
{
    char buf[4];
    /* glyph index -> a string draw_text decodes back to it */
    static const char *const extra[] = {"\xE2\x80\xA6", "\xC3\x97", "\xE2\x96\xB8", "\xE2\x80\x94",
                                        "\xE2\x9C\x93", "\xE2\x80\xA2", "\xE2\x96\xBE"};
    const char *s = buf;
    if (glyph < G_ASCII) {
        buf[0] = (char)(glyph + FONT8X8_FIRST);
        buf[1] = 0;
    } else {
        s = extra[glyph - G_ASCII];
    }
    int x = (r.x + (r.w - 8) / 2) * ui->s, y = (r.y + (r.h - 8) / 2) * ui->s;
    draw_text(ui, x, y, ui->s, c, s);
}

static void draw_commands(Ui *ui)
{
    SDL_Renderer *ren = ui->ren;
    int s = ui->s;
    mu_Command *cmd = NULL;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    while (mu_next_command(ui->mu, &cmd)) {
        switch (cmd->type) {
        case MU_COMMAND_RECT: {
            mu_Color c = cmd->rect.color;
            SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
            SDL_FRect r = {(float)(cmd->rect.rect.x * s), (float)(cmd->rect.rect.y * s),
                           (float)(cmd->rect.rect.w * s), (float)(cmd->rect.rect.h * s)};
            SDL_RenderFillRect(ren, &r);
            break;
        }
        case MU_COMMAND_TEXT:
            draw_text(ui, cmd->text.pos.x * s, cmd->text.pos.y * s, s, cmd->text.color, cmd->text.str);
            break;
        case MU_COMMAND_ICON: {
            mu_Rect r = cmd->icon.rect;
            int id = cmd->icon.id;
            if (id >= UI_ICON_THUMB0 && id < UI_ICON_THUMB0 + PERSIST_NSLOTS) {
                SDL_Texture *t = ui->thumb[id - UI_ICON_THUMB0];
                SDL_FRect d = {(float)(r.x * s), (float)(r.y * s), (float)(r.w * s), (float)(r.h * s)};
                if (t)
                    SDL_RenderTexture(ren, t, NULL, &d);
            } else {
                int g = id == MU_ICON_CLOSE       ? 'x' - FONT8X8_FIRST
                        : id == MU_ICON_CHECK     ? G_CHECK
                        : id == MU_ICON_COLLAPSED ? G_TRIANGLE
                        : id == MU_ICON_EXPANDED  ? G_DOWN
                        : id == UI_ICON_RADIO     ? G_BULLET
                                                  : G_TRIANGLE;
                draw_glyph_in(ui, g, r, cmd->icon.color);
            }
            break;
        }
        case MU_COMMAND_CLIP: {
            mu_Rect r = cmd->clip.rect;
            if (r.w >= 0x100000) {
                SDL_SetRenderClipRect(ren, NULL);
            } else {
                SDL_Rect c = {r.x * s, r.y * s, r.w * s, r.h * s};
                SDL_SetRenderClipRect(ren, &c);
            }
            break;
        }
        }
    }
    SDL_SetRenderClipRect(ren, NULL);
}

/* Output pixels per UI unit: 1 below 800x600, so that panels get at least 400 units across, then
 * one more per 400 x 300. */
static void update_scale(Ui *ui)
{
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(ui->ren, &w, &h);
    int s = SDL_min(w / 400, h / 300);
    ui->s = s < 1 ? 1 : s > 4 ? 4 : s;
    ui->W = w / ui->s;
    ui->H = h / ui->s;
}

void ui_draw_hint(Ui *ui, const char *text)
{
    update_scale(ui);
    int cols = ui->W / 8 - 2;
    if (cols < 8)
        cols = 8;
    /* wrap into lines of at most `cols` characters, at spaces */
    char lines[16][128];
    int n = 0;
    const char *p = text;
    while (*p && n < 16) {
        const char *eol = SDL_strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : SDL_strlen(p);
        while (len && n < 16) {
            size_t take = len, chars = 0, cut = 0;
            const char *q = p, *end = p + len;
            while (q < end) {
                const char *before = q;
                next_glyph(&q, end);
                if ((int)++chars > cols) {
                    take = cut ? cut : (size_t)(before - p);
                    break;
                }
                if (*before == ' ')
                    cut = (size_t)(before - p);
            }
            size_t copy = take < sizeof lines[0] ? take : sizeof lines[0] - 1;
            SDL_memcpy(lines[n], p, copy);
            lines[n][copy] = 0;
            n++;
            p += take;
            len -= take;
            while (len && *p == ' ') {
                p++;
                len--;
            }
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    int lh = 12;
    int y0 = (ui->H - n * lh) / 2;
    for (int i = 0; i < n; i++) {
        int w = text_width(NULL, lines[i], -1);
        draw_text(ui, (ui->W - w) / 2 * ui->s, (y0 + i * lh) * ui->s, ui->s, rgba(170, 160, 166, 255), lines[i]);
    }
}

/* ---------------------------------------------------------------- menu strip */

static int top_count(void)
{
    int n = 0;
    for (const MenuItem *it = menu_bar(); it->kind != MENU_END; it++)
        n++;
    return n;
}

/* Can item i of `items` be selected? Fills st. */
static int selectable(Ui *ui, const MenuItem *it, MenuItemState *st)
{
    if (it->kind == MENU_SEP)
        return 0;
    menu_item_state(it, ui->host.query, ui->host.user, st);
    return !st->hidden && st->enabled;
}

static int step_sel(Ui *ui, const MenuItem *items, int from, int dir)
{
    int n = 0;
    while (items[n].kind != MENU_END)
        n++;
    MenuItemState st;
    for (int k = 1; k <= n; k++) {
        int i = ((from < 0 ? (dir > 0 ? -1 : 0) : from) + dir * k + n * 4) % n;
        if (selectable(ui, &items[i], &st))
            return i;
    }
    return -1;
}

static void open_top(Ui *ui, int top, int select_first)
{
    ui->top = top;
    ui->depth = 1;
    ui->items[0] = menu_bar()[top].sub;
    ui->sel[0] = select_first ? step_sel(ui, ui->items[0], -1, 1) : -1;
}

void ui_show_strip(Ui *ui, int show)
{
    if (!ui->strip)
        show = 0;
    if (ui->pinned)
        show = 1;
    ui->shown = show;
    ui->depth = 0;
    ui->top = show && !ui->pinned ? 0 : -1;
    ui->alt_armed = 0;
}

void ui_set_strip(Ui *ui, int enabled, int pinned)
{
    ui->strip = enabled;
    ui->pinned = enabled && pinned;
    ui_show_strip(ui, ui->pinned);
}

int ui_strip_enabled(const Ui *ui)
{
    return ui->strip;
}

/* Choose item i at level l. */
static void activate(Ui *ui, int l, int i)
{
    const MenuItem *it = &ui->items[l][i];
    MenuItemState st;
    if (!selectable(ui, it, &st))
        return;
    if (it->kind == MENU_SUB) {
        if (l + 1 < MENU_DEPTH) {
            ui->depth = l + 2;
            ui->items[l + 1] = it->sub;
            ui->sel[l + 1] = step_sel(ui, it->sub, -1, 1);
        }
        return;
    }
    ui_show_strip(ui, 0);
    menu_post(it->cmd);
}

static int key_to_access(SDL_Keycode k)
{
    return (k >= 'a' && k <= 'z') || (k >= '0' && k <= '9') ? (int)k : 0;
}

/* Keyboard navigation of the strip. Returns 1 if handled. */
static int strip_nav(Ui *ui, SDL_Keycode k)
{
    int ntop = top_count();
    int deep = ui->depth - 1;
    if (ui->top < 0)
        ui->top = 0;
    switch (k) {
    case SDLK_ESCAPE:
    case SDLK_F10:
        if (ui->depth > 0 && k == SDLK_ESCAPE)
            ui->depth--;
        else
            ui_show_strip(ui, 0);
        return 1;
    case SDLK_LEFT:
        if (ui->depth > 1)
            ui->depth--;
        else {
            ui->top = (ui->top + ntop - 1) % ntop;
            if (ui->depth)
                open_top(ui, ui->top, 1);
        }
        return 1;
    case SDLK_RIGHT:
        if (ui->depth >= 1 && ui->sel[deep] >= 0 && ui->items[deep][ui->sel[deep]].kind == MENU_SUB) {
            activate(ui, deep, ui->sel[deep]);
        } else {
            ui->top = (ui->top + 1) % ntop;
            if (ui->depth)
                open_top(ui, ui->top, 1);
        }
        return 1;
    case SDLK_DOWN:
    case SDLK_UP:
        if (!ui->depth)
            open_top(ui, ui->top, 1);
        else
            ui->sel[deep] = step_sel(ui, ui->items[deep], ui->sel[deep], k == SDLK_DOWN ? 1 : -1);
        return 1;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        if (!ui->depth)
            open_top(ui, ui->top, 1);
        else if (ui->sel[deep] >= 0)
            activate(ui, deep, ui->sel[deep]);
        return 1;
    default:
        break;
    }
    int ak = key_to_access(k);
    if (!ak)
        return 0;
    char plain[256];
    if (!ui->depth) {
        int i = 0;
        for (const MenuItem *it = menu_bar(); it->kind != MENU_END; it++, i++)
            if (menu_plain_label(it->label, plain, sizeof plain) == ak) {
                open_top(ui, i, 1);
                return 1;
            }
        return 1;
    }
    MenuItemState st;
    for (int i = 0; ui->items[deep][i].kind != MENU_END; i++) {
        const MenuItem *it = &ui->items[deep][i];
        if (selectable(ui, it, &st) && menu_plain_label(st.text, plain, sizeof plain) == ak) {
            ui->sel[deep] = i;
            activate(ui, deep, i);
            return 1;
        }
    }
    return 1;
}

static int bar_height(void)
{
    return 8 + 2 * MENU_PAD;
}

static void strip_bar(Ui *ui)
{
    mu_Context *ctx = ui->mu;
    int bh = bar_height();
    mu_Container *c = mu_get_container(ctx, "##menubar");
    c->rect = mu_rect(0, 0, ui->W, bh);
    c->open = 1;
    if (!mu_begin_window_ex(ctx, "##menubar", c->rect,
                            MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOSCROLL | MU_OPT_NOCLOSE | MU_OPT_NOFRAME))
        return;
    mu_bring_to_front(ctx, c);
    mu_draw_rect(ctx, c->rect, ctx->style->colors[MU_COLOR_TITLEBG]);
    int moved = ctx->mouse_delta.x || ctx->mouse_delta.y;
    int x = 2, i = 0;
    char plain[64];
    for (const MenuItem *it = menu_bar(); it->kind != MENU_END && i < 8; it++, i++) {
        menu_plain_label(it->label, plain, sizeof plain);
        mu_Rect r = mu_rect(x, 0, text_width(NULL, plain, -1) + 2 * MENU_PAD + 4, bh);
        mu_Id id = mu_get_id(ctx, &i, sizeof i);
        mu_update_control(ctx, id, r, 0);
        if ((ctx->mouse_pressed & MU_MOUSE_LEFT) && ctx->focus == id) {
            if (ui->depth && ui->top == i)
                ui->depth = 0;
            else
                open_top(ui, i, 0);
        } else if (ctx->hover == id && moved && ui->depth && ui->top != i) {
            open_top(ui, i, 0);
        }
        int hi = ui->top == i;
        if (hi || ctx->hover == id)
            mu_draw_rect(ctx, r, ctx->style->colors[hi ? MU_COLOR_BUTTONFOCUS : MU_COLOR_BUTTONHOVER]);
        mu_draw_text(ctx, NULL, plain, -1, mu_vec2(r.x + MENU_PAD + 2, MENU_PAD), ctx->style->colors[MU_COLOR_TITLETEXT]);
        ui->top_rect[i] = r;
        x += r.w;
    }
    mu_end_window(ctx);
}

static void strip_drop(Ui *ui, int l)
{
    static const char *const names[MENU_DEPTH] = {"##menu0", "##menu1", "##menu2"};
    mu_Context *ctx = ui->mu;
    const MenuItem *items = ui->items[l];
    int rh = 8 + 2 * 3, w = 0, h = 4;
    MenuItemState st;
    char plain[256], sc[64];
    int last_sep = 1;
    for (const MenuItem *it = items; it->kind != MENU_END; it++) {
        if (it->kind == MENU_SEP) {
            if (!last_sep)
                h += 5;
            last_sep = 1;
            continue;
        }
        menu_item_state(it, ui->host.query, ui->host.user, &st);
        if (st.hidden)
            continue;
        menu_plain_label(st.text, plain, sizeof plain);
        menu_shortcut_text(it, sc, sizeof sc);
        int iw = text_width(NULL, plain, -1) + (sc[0] ? text_width(NULL, sc, -1) + 16 : 0);
        w = SDL_max(w, iw);
        h += rh;
        last_sep = 0;
    }
    w += 12 + 12 + 8;
    int x, y;
    if (l == 0) {
        x = ui->top_rect[ui->top].x;
        y = bar_height();
    } else {
        x = ui->drop_rect[l - 1].x + ui->drop_rect[l - 1].w - 2;
        y = ui->sel_y[l - 1] - 2;
        if (x + w > ui->W) /* no room on the right: open to the left */
            x = SDL_max(0, ui->drop_rect[l - 1].x - w + 2);
    }
    if (x + w > ui->W)
        x = SDL_max(0, ui->W - w);
    if (y + h > ui->H)
        y = SDL_max(0, ui->H - h);
    mu_Container *c = mu_get_container(ctx, names[l]);
    c->rect = mu_rect(x, y, w, h);
    c->open = 1;
    ui->drop_rect[l] = c->rect;
    if (!mu_begin_window_ex(ctx, names[l], c->rect, MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOSCROLL | MU_OPT_NOCLOSE))
        return;
    mu_bring_to_front(ctx, c);
    int moved = ctx->mouse_delta.x || ctx->mouse_delta.y;
    int yy = y + 2;
    last_sep = 1;
    mu_Color dim = rgba(130, 120, 126, 255);
    for (int i = 0; items[i].kind != MENU_END; i++) {
        const MenuItem *it = &items[i];
        if (it->kind == MENU_SEP) {
            if (!last_sep) {
                mu_draw_rect(ctx, mu_rect(x + 4, yy + 2, w - 8, 1), ctx->style->colors[MU_COLOR_BUTTONHOVER]);
                yy += 5;
            }
            last_sep = 1;
            continue;
        }
        menu_item_state(it, ui->host.query, ui->host.user, &st);
        if (st.hidden)
            continue;
        last_sep = 0;
        mu_Rect r = mu_rect(x + 2, yy, w - 4, rh);
        mu_Id id = mu_get_id(ctx, &it, sizeof it);
        mu_update_control(ctx, id, r, 0);
        if (ctx->hover == id && moved && st.enabled) {
            ui->sel[l] = i;
            ui->depth = l + 1;
            if (it->kind == MENU_SUB)
                activate(ui, l, i);
            if (ui->depth > l + 1)
                ui->sel[l + 1] = -1;
        }
        if ((ctx->mouse_pressed & MU_MOUSE_LEFT) && ctx->focus == id && st.enabled) {
            ui->sel[l] = i;
            activate(ui, l, i);
            if (!ui->shown || ui->depth == 0) {
                mu_end_window(ctx);
                return;
            }
        }
        int sel = ui->sel[l] == i;
        if (sel) {
            mu_draw_rect(ctx, r, ctx->style->colors[l == ui->depth - 1 ? MU_COLOR_BUTTONFOCUS : MU_COLOR_BUTTONHOVER]);
            ui->sel_y[l] = yy;
        }
        mu_Color tc = st.enabled ? ctx->style->colors[MU_COLOR_TEXT] : dim;
        if (st.checked)
            mu_draw_icon(ctx, it->kind == MENU_RADIO ? UI_ICON_RADIO : MU_ICON_CHECK, mu_rect(x + 2, yy, 12, rh), tc);
        menu_plain_label(st.text, plain, sizeof plain);
        mu_draw_text(ctx, NULL, plain, -1, mu_vec2(x + 14, yy + 3), tc);
        menu_shortcut_text(it, sc, sizeof sc);
        if (sc[0])
            mu_draw_text(ctx, NULL, sc, -1, mu_vec2(x + w - 12 - text_width(NULL, sc, -1), yy + 3), tc);
        if (it->kind == MENU_SUB)
            mu_draw_icon(ctx, UI_ICON_ARROW, mu_rect(x + w - 12, yy, 10, rh), tc);
        yy += rh;
    }
    mu_end_window(ctx);
}

static void strip_frame(Ui *ui)
{
    strip_bar(ui);
    for (int l = 0; l < ui->depth && ui->shown; l++)
        strip_drop(ui, l);
}

static int in_rect(mu_Rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

int ui_strip_key(Ui *ui, const SDL_Event *ev)
{
    if (!ui->strip || ui->panel)
        return 0;
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.key == SDLK_LALT || ev->key.key == SDLK_RALT) {
            if (!ev->key.repeat)
                ui->alt_armed = 1;
            return 0;
        }
        ui->alt_armed = 0;
        if (ev->key.key == SDLK_F10 && !ev->key.repeat && !ui->shown) {
            ui_show_strip(ui, 1);
            return 1;
        }
        return 0;
    case SDL_EVENT_KEY_UP:
        if ((ev->key.key == SDLK_LALT || ev->key.key == SDLK_RALT) && ui->alt_armed) {
            ui->alt_armed = 0;
            ui_show_strip(ui, !ui->shown || ui->pinned);
            if (ui->pinned)
                ui->top = 0;
            return 1;
        }
        return 0;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ui->alt_armed = 0;
        return 0;
    }
    return 0;
}

/* ---------------------------------------------------------------- panels */

void ui_states_changed(Ui *ui)
{
    free_thumbs(ui);
    for (int i = 0; i < PERSIST_NSLOTS; i++) {
        persist_slot_info(i, &ui->info[i]);
        int w, h;
        uint8_t *px = persist_load_thumb(i, &w, &h);
        if (!px)
            continue;
        uint32_t *rgb = SDL_malloc((size_t)w * (size_t)h * 4);
        if (rgb) {
            for (int k = 0; k < w * h; k++)
                rgb[k] = ui->lut[px[k]];
            ui->thumb[i] = SDL_CreateTexture(ui->ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
            if (ui->thumb[i]) {
                SDL_UpdateTexture(ui->thumb[i], NULL, rgb, w * 4);
                SDL_SetTextureScaleMode(ui->thumb[i], SDL_SCALEMODE_NEAREST);
            }
            SDL_free(rgb);
        }
        SDL_free(px);
    }
}

void ui_open(Ui *ui, int panel)
{
    ui->shown = ui->pinned;
    ui->depth = 0;
    ui->panel = panel;
    ui->opening = 1;
    ui->row = ui->col = ui->capture = 0;
    ui->scroll_by = 0;
    ui->scroll_to = 1;
    ui->msg[0] = 0;
    ui->confirm_delete = -1;
    if (panel == UI_PANEL_STATES) {
        ui->row = persist_slot();
        ui_states_changed(ui);
    }
}

void ui_close(Ui *ui)
{
    ui->panel = UI_PANEL_NONE;
    ui->capture = 0;
    free_thumbs(ui);
}

int ui_panel(const Ui *ui)
{
    return ui->panel;
}

int ui_active(const Ui *ui)
{
    return ui->panel != UI_PANEL_NONE || ui->shown;
}

static mu_Rect panel_rect(Ui *ui, int maxw, int maxh)
{
    int top = ui->shown ? bar_height() : 0;
    int w = SDL_min(ui->W - 8, maxw), h = SDL_min(ui->H - top - 8, maxh);
    return mu_rect((ui->W - w) / 2, top + (ui->H - top - h) / 2, w, h);
}

static int begin_panel(Ui *ui, const char *title, mu_Rect r, int opt)
{
    mu_Container *c = mu_get_container(ui->mu, title);
    if (ui->opening) {
        c->open = 1;
        c->scroll = mu_vec2(0, 0);
    } else if (!c->open) { /* closed with its x */
        ui_close(ui);
        return 0;
    }
    c->rect = r;
    if (!mu_begin_window_ex(ui->mu, title, r, MU_OPT_NORESIZE | opt))
        return 0;
    mu_bring_to_front(ui->mu, c);
    return 1;
}

/* A selectable cell: returns 1 when clicked. */
static int cell(Ui *ui, int key, const char *text, int selected, mu_Rect *out)
{
    mu_Context *ctx = ui->mu;
    mu_Rect r = mu_layout_next(ctx);
    mu_Id id = mu_get_id(ctx, &key, sizeof key);
    mu_update_control(ctx, id, r, 0);
    int clicked = (ctx->mouse_pressed & MU_MOUSE_LEFT) && ctx->focus == id;
    int color = selected ? MU_COLOR_BUTTONFOCUS : ctx->hover == id ? MU_COLOR_BUTTONHOVER : MU_COLOR_BUTTON;
    mu_draw_rect(ctx, r, ctx->style->colors[color]);
    mu_draw_control_text(ctx, text, r, MU_COLOR_TEXT, 0);
    if (selected && out)
        *out = r;
    return clicked;
}

/* Scroll the current container so that r is visible. */
static void scroll_into_view(Ui *ui, mu_Rect r)
{
    mu_Container *c = mu_get_current_container(ui->mu);
    if (r.y < c->body.y)
        c->scroll.y -= c->body.y - r.y;
    else if (r.y + r.h > c->body.y + c->body.h)
        c->scroll.y += r.y + r.h - (c->body.y + c->body.h);
    if (c->scroll.y < 0)
        c->scroll.y = 0;
}

/* --- Controls --- */

#define CONTROLS_ROWS (INPUT_NPHONE + 1) /* the phone keys, then Reset / Close */

static void controls_save(Ui *ui)
{
    if (ui->host.keys_path && input_save_config(ui->host.input, ui->host.keys_path) < 0)
        SDL_snprintf(ui->msg, sizeof ui->msg, "Cannot write %s (see the log).", ui->host.keys_path);
}

static void controls_activate(Ui *ui)
{
    if (ui->row < INPUT_NPHONE) {
        ui->capture = 1;
        int code = input_phone_at(ui->row);
        SDL_snprintf(ui->msg, sizeof ui->msg, ui->col ? "Press a gamepad button or move a stick or trigger for %s. Esc cancels."
                                                      : "Press a key for %s. Esc cancels.",
                     input_phone_label(code));
    } else if (ui->col == 0) {
        input_reset_defaults(ui->host.input);
        controls_save(ui);
        if (!ui->msg[0])
            SDL_strlcpy(ui->msg, "Back to the default controls.", sizeof ui->msg);
    } else {
        ui_close(ui);
    }
}

static void controls_bound(Ui *ui, int kind, int code, int dir)
{
    int ph = input_phone_at(ui->row);
    char what[64];
    input_describe_source(kind, code, dir, what, sizeof what);
    ui->capture = 0;
    if (input_bind(ui->host.input, kind, code, dir, ph)) {
        SDL_snprintf(ui->msg, sizeof ui->msg, "%s now also presses %s.", what, input_phone_label(ph));
        controls_save(ui);
    } else {
        SDL_snprintf(ui->msg, sizeof ui->msg, "%s already presses %s.", what, input_phone_label(ph));
    }
}

static void controls_frame(Ui *ui)
{
    mu_Context *ctx = ui->mu;
    if (!begin_panel(ui, "Controls", panel_rect(ui, 520, 100000), MU_OPT_NOSCROLL))
        return;
    mu_Container *win = mu_get_current_container(ctx);
    int bw = win->body.w - 2 * ctx->style->padding;
    int lw = 13 * 8 + 6, cw = (bw - lw - 2 * ctx->style->spacing) / 2;
    mu_layout_row(ctx, 1, (int[]){-1}, 0);
    mu_label(ctx, ui->msg[0] ? ui->msg : "Select a cell, press Enter, then the new key.");
    mu_layout_row(ctx, 3, (int[]){lw, cw, -1}, 0);
    mu_label(ctx, "Phone key");
    mu_label(ctx, "Keyboard");
    mu_label(ctx, "Gamepad");
    int foot = 2 * (8 + 2 * ctx->style->padding) + 3 * ctx->style->spacing;
    mu_layout_row(ctx, 1, (int[]){-1}, -foot);
    mu_begin_panel_ex(ctx, "rows", 0);
    mu_Rect selr = {0, 0, 0, 0};
    char buf[256];
    for (int r = 0; r < INPUT_NPHONE; r++) {
        int code = input_phone_at(r);
        mu_layout_row(ctx, 3, (int[]){lw - 4, cw, -1}, 0);
        mu_label(ctx, input_phone_label(code));
        for (int c = 0; c < 2; c++) {
            input_describe(ui->host.input, code, c, buf, sizeof buf);
            int sel = ui->row == r && ui->col == c;
            const char *t = sel && ui->capture ? "press..." : buf[0] ? buf : "-";
            if (cell(ui, 1000 + r * 2 + c, t, sel, &selr)) {
                ui->row = r;
                ui->col = c;
                controls_activate(ui);
            }
        }
    }
    if (ui->scroll_to && ui->row < INPUT_NPHONE && selr.w) {
        scroll_into_view(ui, selr);
        ui->scroll_to = 0;
    }
    mu_end_panel(ctx);
    mu_layout_row(ctx, 2, (int[]){lw + cw, -1}, 0);
    if (cell(ui, 2000, "Reset to defaults", ui->row == INPUT_NPHONE && ui->col == 0, NULL)) {
        ui->row = INPUT_NPHONE;
        ui->col = 0;
        controls_activate(ui);
    }
    if (cell(ui, 2001, "Close", ui->row == INPUT_NPHONE && ui->col == 1, NULL)) {
        mu_end_window(ctx);
        ui_close(ui);
        return;
    }
    mu_layout_row(ctx, 1, (int[]){-1}, 0);
    mu_label(ctx, "Arrows: move   Enter: add   Del: clear   Esc: close");
    mu_end_window(ctx);
}

/* --- Save states --- */

/* detail 1: date and time to the second, 2: to the minute, 0: short */
static void format_time(SDL_Time t, char *buf, size_t n, int detail)
{
    SDL_DateTime dt;
    if (!SDL_TimeToDateTime(t, &dt, true)) {
        SDL_strlcpy(buf, "?", n);
        return;
    }
    if (detail == 1)
        SDL_snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    else if (detail == 2)
        SDL_snprintf(buf, n, "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
    else
        SDL_snprintf(buf, n, "%02d-%02d %02d:%02d", dt.month, dt.day, dt.hour, dt.minute);
}

static void states_action(Ui *ui, int what)
{
    int slot = ui->row;
    switch (what) {
    case 0: /* load */
        if (!ui->info[slot].exists) {
            SDL_snprintf(ui->msg, sizeof ui->msg, "Slot %d is empty.", slot);
            return;
        }
        menu_post(MENU_CMD_SLOT0 + slot);
        menu_post(MENU_CMD_QUICK_LOAD);
        ui_close(ui);
        return;
    case 1: /* save */
        menu_post(MENU_CMD_SLOT0 + slot);
        menu_post(MENU_CMD_QUICK_SAVE); /* the main loop calls ui_states_changed */
        SDL_snprintf(ui->msg, sizeof ui->msg, "Saved to slot %d.", slot);
        ui->confirm_delete = -1;
        return;
    default: /* delete, after a second press */
        if (!ui->info[slot].exists) {
            SDL_snprintf(ui->msg, sizeof ui->msg, "Slot %d is empty.", slot);
            return;
        }
        if (ui->confirm_delete != slot) {
            ui->confirm_delete = slot;
            SDL_snprintf(ui->msg, sizeof ui->msg, "Delete slot %d? Press Delete again.", slot);
            return;
        }
        persist_delete(slot);
        ui->confirm_delete = -1;
        SDL_snprintf(ui->msg, sizeof ui->msg, "Slot %d deleted.", slot);
        ui_states_changed(ui);
        return;
    }
}

static void states_frame(Ui *ui)
{
    mu_Context *ctx = ui->mu;
    if (!begin_panel(ui, "Save States", panel_rect(ui, 480, 340), MU_OPT_NOSCROLL))
        return;
    mu_Container *win = mu_get_current_container(ctx);
    int bw = win->body.w - 2 * ctx->style->padding;
    int full = bw >= 330;
    int lw = full ? 8 * 21 + 6 : 8 * 14 + 6;
    int rowh = 8 + 2 * ctx->style->padding;
    int foot = 4 * rowh + 4 * ctx->style->spacing; /* date, buttons, message, keys */
    mu_layout_row(ctx, 2, (int[]){lw, -1}, -foot);
    mu_layout_begin_column(ctx);
    char buf[96], t[40];
    for (int s = 0; s < PERSIST_NSLOTS; s++) {
        mu_layout_row(ctx, 1, (int[]){-1}, 0);
        if (ui->info[s].exists) {
            format_time(ui->info[s].mtime, t, sizeof t, full ? 2 : 0);
            SDL_snprintf(buf, sizeof buf, "%d%s %s", s, s == persist_slot() ? "*" : " ", t);
        } else {
            SDL_snprintf(buf, sizeof buf, "%d%s empty", s, s == persist_slot() ? "*" : " ");
        }
        if (cell(ui, 3000 + s, buf, ui->row == s, NULL)) {
            ui->row = s;
            ui->confirm_delete = -1;
        }
    }
    mu_layout_end_column(ctx);
    int slot = ui->row;
    mu_Rect pr = mu_layout_next(ctx);
    mu_draw_rect(ctx, pr, ctx->style->colors[MU_COLOR_BASE]);
    SDL_Texture *tex = ui->thumb[slot];
    if (tex) {
        float tw, th;
        SDL_GetTextureSize(tex, &tw, &th);
        /* the largest whole multiple that fits, or a fraction if even 1x does not */
        float k = SDL_min(pr.w / tw, pr.h / th);
        if (k >= 1)
            k = SDL_floorf(k);
        int dw = (int)(tw * k), dh = (int)(th * k);
        mu_draw_icon(ctx, UI_ICON_THUMB0 + slot, mu_rect(pr.x + (pr.w - dw) / 2, pr.y + (pr.h - dh) / 2, dw, dh),
                     rgba(255, 255, 255, 255));
    } else {
        const char *m = ui->info[slot].exists ? "no preview" : "empty";
        int w = text_width(NULL, m, -1);
        mu_draw_text(ctx, NULL, m, -1, mu_vec2(pr.x + (pr.w - w) / 2, pr.y + pr.h / 2 - 4),
                     ctx->style->colors[MU_COLOR_TEXT]);
    }
    mu_layout_row(ctx, 1, (int[]){-1}, 0);
    if (ui->info[slot].exists) {
        format_time(ui->info[slot].mtime, t, sizeof t, 1);
        SDL_snprintf(buf, sizeof buf, "Slot %d: %s", slot, t);
    } else {
        SDL_snprintf(buf, sizeof buf, "Slot %d: empty", slot);
    }
    mu_label(ctx, buf);
    int third = (bw - 2 * ctx->style->spacing) / 3;
    mu_layout_row(ctx, 3, (int[]){third, third, -1}, 0);
    int act = -1;
    if (mu_button(ctx, "Load"))
        act = 0;
    if (mu_button(ctx, "Save"))
        act = 1;
    if (mu_button(ctx, "Delete"))
        act = 2;
    mu_layout_row(ctx, 1, (int[]){-1}, 0);
    mu_label(ctx, ui->msg[0] ? ui->msg : "* is the quick save slot (F5 / F8).");
    mu_layout_row(ctx, 1, (int[]){-1}, 0);
    mu_label(ctx, full ? "Enter: load   F5: save   Del: delete   Esc: close" : "Enter load  F5 save  Del delete");
    mu_end_window(ctx);
    if (act >= 0)
        states_action(ui, act);
}

/* --- Help and About --- */

static void hotkeys_of(Ui *ui, const MenuItem *items)
{
    char sc[64], plain[256], line[400];
    for (const MenuItem *it = items; it->kind != MENU_END; it++) {
        if (it->kind == MENU_SUB) {
            hotkeys_of(ui, it->sub);
        } else if (it->key) {
            menu_shortcut_text(it, sc, sizeof sc);
            menu_plain_label(it->label, plain, sizeof plain);
            SDL_snprintf(line, sizeof line, "%-8s %s", sc, plain);
            mu_label(ui->mu, line);
        }
    }
}

static void help_frame(Ui *ui)
{
    mu_Context *ctx = ui->mu;
    if (!begin_panel(ui, "Help: Controls", panel_rect(ui, 560, 100000), 0))
        return;
    mu_Container *win = mu_get_current_container(ctx);
    if (ui->scroll_by) {
        win->scroll.y = SDL_max(0, win->scroll.y + ui->scroll_by);
        ui->scroll_by = 0;
    }
    int bw = win->body.w - 2 * ctx->style->padding;
    int lw = 13 * 8 + 6, cw = (bw - lw - 2 * ctx->style->spacing) / 2;
    mu_layout_row(ctx, 1, (int[]){-1}, 0);
    mu_text(ctx, "Games were written for a phone keypad: most move with 2/4/6/8 and fire or "
                 "select with 5 or OK. Change the bindings in Input > Controls.");
    mu_layout_row(ctx, 3, (int[]){lw, cw, -1}, 0);
    mu_label(ctx, "Phone key");
    mu_label(ctx, "Keyboard");
    mu_label(ctx, "Gamepad");
    char buf[256];
    for (int r = 0; r < INPUT_NPHONE; r++) {
        int code = input_phone_at(r);
        mu_label(ctx, input_phone_label(code));
        input_describe(ui->host.input, code, 0, buf, sizeof buf);
        mu_label(ctx, buf[0] ? buf : "-");
        input_describe(ui->host.input, code, 1, buf, sizeof buf);
        mu_label(ctx, buf[0] ? buf : "-");
    }
    mu_layout_row(ctx, 1, (int[]){-1}, 0);
    mu_label(ctx, "");
    mu_label(ctx, "Hotkeys");
    hotkeys_of(ui, menu_bar());
    if (ui->strip)
        mu_label(ctx, "F10, Alt show the menu");
    mu_label(ctx, "Esc      closes a panel or the menu");
    mu_end_window(ctx);
}

static void about_frame(Ui *ui)
{
    mu_Context *ctx = ui->mu;
    if (!begin_panel(ui, "About exen-reloaded", panel_rect(ui, 440, 100000), 0))
        return;
    char buf[200];
    mu_layout_row(ctx, 1, (int[]){-1}, 0);
    SDL_snprintf(buf, sizeof buf, "exen-reloaded %s", ui->host.version ? ui->host.version : "");
    mu_text(ctx, buf);
    mu_text(ctx, "A portable reimplementation of In-Fusio's ExEn mobile games platform "
                 "(2002-2004).");
    mu_text(ctx, "");
    mu_text(ctx, "Free software under the GNU General Public License, version 2.");
    mu_text(ctx, "https://github.com/zdiemer/exen-reloaded");
    mu_text(ctx, "");
    mu_text(ctx, "Credits");
    mu_text(ctx, "- In-Fusio's EXEN-V2 Generic Simulator (gensimu.exe), whose ExEn framework "
                 "bytecode runs unmodified; abandoned freeware.");
    mu_text(ctx, "- microui by rxi (MIT licence): the in-window menu and panels.");
    mu_text(ctx, "- font8x8 by Daniel Hepper (public domain): the UI font.");
    mu_text(ctx, "- SDL 3 (zlib licence): windows, input and sound.");
    mu_text(ctx, "- TinySoundFont by Bernhard Schelling (MIT licence): the General MIDI voice.");
    mu_text(ctx, "- TimGM6mb SoundFont by Tim Brechbill and David Bolton (GPL-2): its instruments.");
    mu_text(ctx, "");
    mu_text(ctx, "No games are included. ExEn and the game titles are trademarks of their owners.");
    mu_layout_row(ctx, 2, (int[]){120, -1}, 0);
    if (mu_button(ctx, "Project page"))
        SDL_OpenURL("https://github.com/zdiemer/exen-reloaded");
    if (mu_button(ctx, "Close")) {
        mu_end_window(ctx);
        ui_close(ui);
        return;
    }
    mu_end_window(ctx);
}

/* ---------------------------------------------------------------- frames and events */

static void frame(Ui *ui)
{
    mu_begin(ui->mu);
    switch (ui->panel) {
    case UI_PANEL_CONTROLS: controls_frame(ui); break;
    case UI_PANEL_STATES: states_frame(ui); break;
    case UI_PANEL_HELP: help_frame(ui); break;
    case UI_PANEL_ABOUT: about_frame(ui); break;
    default: break;
    }
    if (ui->shown)
        strip_frame(ui);
    mu_end(ui->mu);
}

void ui_render(Ui *ui)
{
    if (!ui_active(ui))
        return;
    update_scale(ui);
    /* microui settles hover and focus one frame late: the first frame takes the input, the
     * second shows the result */
    frame(ui);
    ui->opening = 0;
    if (!ui_active(ui))
        return;
    frame(ui);
    draw_commands(ui);
}

enum { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK, NAV_BACK, NAV_DELETE, NAV_SAVE,
       NAV_PAGEUP, NAV_PAGEDOWN };

static int nav_of(const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        switch (ev->key.key) {
        case SDLK_UP: return NAV_UP;
        case SDLK_DOWN: return NAV_DOWN;
        case SDLK_LEFT: return NAV_LEFT;
        case SDLK_RIGHT: return NAV_RIGHT;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return ev->key.repeat ? NAV_NONE : NAV_OK;
        case SDLK_ESCAPE: return NAV_BACK;
        case SDLK_DELETE: case SDLK_BACKSPACE: return ev->key.repeat ? NAV_NONE : NAV_DELETE;
        case SDLK_F5: return ev->key.repeat ? NAV_NONE : NAV_SAVE;
        case SDLK_PAGEUP: return NAV_PAGEUP;
        case SDLK_PAGEDOWN: return NAV_PAGEDOWN;
        }
    } else if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        switch (ev->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return NAV_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return NAV_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return NAV_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return NAV_RIGHT;
        case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: return NAV_OK;
        case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: return NAV_BACK;
        case SDL_GAMEPAD_BUTTON_NORTH: return NAV_DELETE;
        case SDL_GAMEPAD_BUTTON_WEST: return NAV_SAVE;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return NAV_PAGEUP;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return NAV_PAGEDOWN;
        }
    }
    return NAV_NONE;
}

/* Controls panel waiting for an input to bind. Returns 1 if the event was used. */
static int capture_event(Ui *ui, const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat) {
        SDL_Keycode k = ev->key.key;
        if (k == SDLK_ESCAPE) {
            ui->capture = 0;
            SDL_strlcpy(ui->msg, "Cancelled.", sizeof ui->msg);
        } else if (ui->col == 1) {
            /* waiting for a gamepad input; keys other than Esc are ignored */
        } else if (menu_shortcut(k, 0) || (ui->strip && k == SDLK_F10)) {
            SDL_snprintf(ui->msg, sizeof ui->msg, "%s is a hotkey and cannot be bound. Esc cancels.", SDL_GetKeyName(k));
        } else if (k != SDLK_UNKNOWN) {
            controls_bound(ui, INPUT_KEY, (int)k, 0);
        }
        return 1;
    }
    if (ui->col != 1)
        return ev->type == SDL_EVENT_KEY_UP || ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
               ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP || ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION;
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        controls_bound(ui, INPUT_BUTTON, ev->gbutton.button, 0);
        return 1;
    }
    if (ev->type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        int v = ev->gaxis.value, a = ev->gaxis.axis;
        int trigger = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (v >= ui->host.input->axis_press || (!trigger && -v >= ui->host.input->axis_press))
            controls_bound(ui, INPUT_AXIS, a, v > 0 ? 1 : -1);
        return 1;
    }
    return ev->type == SDL_EVENT_KEY_UP || ev->type == SDL_EVENT_GAMEPAD_BUTTON_UP;
}

static void panel_nav(Ui *ui, int nav)
{
    switch (ui->panel) {
    case UI_PANEL_CONTROLS:
        switch (nav) {
        case NAV_UP: ui->row = (ui->row + CONTROLS_ROWS - 1) % CONTROLS_ROWS; break;
        case NAV_DOWN: ui->row = (ui->row + 1) % CONTROLS_ROWS; break;
        case NAV_PAGEUP: ui->row = SDL_max(0, ui->row - 6); break;
        case NAV_PAGEDOWN: ui->row = SDL_min(CONTROLS_ROWS - 1, ui->row + 6); break;
        case NAV_LEFT: case NAV_RIGHT: ui->col ^= 1; break;
        case NAV_OK: controls_activate(ui); break;
        case NAV_DELETE:
            if (ui->row < INPUT_NPHONE) {
                int code = input_phone_at(ui->row);
                input_unbind(ui->host.input, code, ui->col);
                SDL_snprintf(ui->msg, sizeof ui->msg, "Cleared the %s bindings of %s.", ui->col ? "gamepad" : "keyboard",
                             input_phone_label(code));
                controls_save(ui);
            }
            break;
        case NAV_BACK: ui_close(ui); break;
        }
        ui->scroll_to = 1;
        break;
    case UI_PANEL_STATES:
        if (nav != NAV_DELETE)
            ui->confirm_delete = -1;
        switch (nav) {
        case NAV_UP: case NAV_LEFT: ui->row = (ui->row + PERSIST_NSLOTS - 1) % PERSIST_NSLOTS; break;
        case NAV_DOWN: case NAV_RIGHT: ui->row = (ui->row + 1) % PERSIST_NSLOTS; break;
        case NAV_OK: states_action(ui, 0); break;
        case NAV_SAVE: states_action(ui, 1); break;
        case NAV_DELETE: states_action(ui, 2); break;
        case NAV_BACK: ui_close(ui); break;
        }
        break;
    default: /* Help, About */
        switch (nav) {
        case NAV_UP: ui->scroll_by -= 12; break;
        case NAV_DOWN: ui->scroll_by += 12; break;
        case NAV_PAGEUP: ui->scroll_by -= 120; break;
        case NAV_PAGEDOWN: ui->scroll_by += 120; break;
        case NAV_OK: case NAV_BACK: ui_close(ui); break;
        }
        break;
    }
}

static int mouse_button(Uint8 b)
{
    return b == SDL_BUTTON_LEFT ? MU_MOUSE_LEFT : b == SDL_BUTTON_RIGHT ? MU_MOUSE_RIGHT : MU_MOUSE_MIDDLE;
}

int ui_event(Ui *ui, const SDL_Event *in)
{
    if (!ui_active(ui))
        return 0;
    SDL_Event e = *in;
    SDL_ConvertEventToRenderCoordinates(ui->ren, &e);
    update_scale(ui);
    mu_Context *ctx = ui->mu;
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        mu_input_mousemove(ctx, (int)e.motion.x / ui->s, (int)e.motion.y / ui->s);
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        int x = (int)e.button.x / ui->s, y = (int)e.button.y / ui->s;
        if (ui->capture) {
            ui->capture = 0;
            SDL_strlcpy(ui->msg, "Cancelled.", sizeof ui->msg);
        }
        if (ui->shown && !ui->panel) {
            int inside = in_rect(mu_rect(0, 0, ui->W, bar_height()), x, y);
            for (int l = 0; l < ui->depth; l++)
                inside |= in_rect(ui->drop_rect[l], x, y);
            if (!inside) {
                if (ui->depth)
                    ui->depth = 0;
                else
                    ui_show_strip(ui, 0);
                return 1;
            }
        }
        mu_input_mousemove(ctx, x, y);
        mu_input_mousedown(ctx, x, y, mouse_button(e.button.button));
        return 1;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        mu_input_mouseup(ctx, (int)e.button.x / ui->s, (int)e.button.y / ui->s, mouse_button(e.button.button));
        return 1;
    case SDL_EVENT_MOUSE_WHEEL:
        mu_input_scroll(ctx, 0, (int)(-e.wheel.y * 24));
        return 1;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        break;
    default:
        return 0;
    }
    if (ui->panel == UI_PANEL_CONTROLS && ui->capture)
        return capture_event(ui, &e);
    int nav = nav_of(&e);
    if (ui->panel) {
        if (nav)
            panel_nav(ui, nav);
        return 1;
    }
    /* the strip */
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_LALT || e.key.key == SDLK_RALT) {
            ui->alt_armed = !e.key.repeat;
            return 1;
        }
        ui->alt_armed = 0;
        strip_nav(ui, e.key.key);
        return 1;
    }
    if (e.type == SDL_EVENT_KEY_UP) {
        if ((e.key.key == SDLK_LALT || e.key.key == SDLK_RALT) && ui->alt_armed) {
            ui->alt_armed = 0;
            if (ui->pinned)
                ui->depth = 0;
            else
                ui_show_strip(ui, 0);
        }
        return 1;
    }
    static const SDL_Keycode nav_keys[] = {0, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_RETURN, SDLK_ESCAPE};
    if (nav > NAV_NONE && nav <= NAV_BACK)
        strip_nav(ui, nav_keys[nav]);
    return 1;
}
