/* Native Win32 menu bar for the SDL window (menu.h).
 *
 * The bar is built from the model once; each popup is refilled from the model when it opens
 * (WM_INITMENUPOPUP), so labels, check marks, greyed items and the dynamic lists (recent files,
 * gamepads) always reflect the application's state. The window procedure is subclassed for
 * that and for WM_COMMAND, which becomes a menu_post() event: the command then runs on the
 * main loop, not inside the window procedure or the menu's modal loop.
 *
 * The client area must stay exactly scale times the handset screen so that integer scaling is
 * exact: SDL sizes the window for a one-line menu bar, and menu_native_fit corrects it when the
 * bar wraps onto more lines in a narrow window.
 */
#include "menu.h"

#ifdef SDL_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MAX_POPUPS 32

static HWND g_hwnd;
static WNDPROC g_prev;
static HMENU g_bar;
static int g_shown;
static MenuQueryFn g_query;
static void *g_user;
static struct {
    HMENU h;
    const MenuItem *items;
} g_pop[MAX_POPUPS];
static int g_npop;

static HMENU popup_for(const MenuItem *items)
{
    for (int i = 0; i < g_npop; i++)
        if (g_pop[i].items == items)
            return g_pop[i].h;
    if (g_npop == MAX_POPUPS)
        return NULL;
    HMENU h = CreatePopupMenu();
    if (h) {
        g_pop[g_npop].h = h;
        g_pop[g_npop].items = items;
        g_npop++;
    }
    return h;
}

/* UTF-8 label (with '&' access keys, which Win32 menus understand as is) and shortcut text,
 * separated by a tab, which right-aligns the shortcut. */
static void append(HMENU menu, UINT flags, UINT_PTR id, const char *label, const char *shortcut)
{
    char text[400];
    SDL_snprintf(text, sizeof text, "%s%s%s", label, *shortcut ? "\t" : "", shortcut);
    wchar_t w[400];
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, w, (int)(sizeof w / sizeof *w)))
        w[0] = 0;
    AppendMenuW(menu, flags, id, w);
}

static void fill(HMENU menu, const MenuItem *items)
{
    /* RemoveMenu, not DeleteMenu: the submenus are kept and reused */
    while (GetMenuItemCount(menu) > 0)
        RemoveMenu(menu, 0, MF_BYPOSITION);
    int last_sep = 1; /* no separator at the top or twice in a row */
    for (const MenuItem *it = items; it->kind != MENU_END; it++) {
        if (it->kind == MENU_SEP) {
            if (!last_sep)
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            last_sep = 1;
            continue;
        }
        MenuItemState st;
        menu_item_state(it, g_query, g_user, &st);
        if (st.hidden)
            continue;
        UINT grey = st.enabled ? MF_ENABLED : MF_GRAYED;
        if (it->kind == MENU_SUB) {
            HMENU sub = popup_for(it->sub);
            if (sub)
                append(menu, MF_POPUP | grey, (UINT_PTR)sub, st.text, "");
        } else {
            char sc[64];
            menu_shortcut_text(it, sc, sizeof sc);
            append(menu, MF_STRING | grey | (st.checked ? MF_CHECKED : MF_UNCHECKED), (UINT_PTR)it->cmd,
                   st.text, sc);
            if (it->kind == MENU_RADIO) { /* a bullet instead of a tick */
                MENUITEMINFOW mi;
                SDL_zero(mi);
                mi.cbSize = sizeof mi;
                mi.fMask = MIIM_FTYPE;
                if (GetMenuItemInfoW(menu, (UINT)it->cmd, FALSE, &mi)) {
                    mi.fType |= MFT_RADIOCHECK;
                    SetMenuItemInfoW(menu, (UINT)it->cmd, FALSE, &mi);
                }
            }
        }
        last_sep = 0;
    }
    /* drop a trailing separator */
    int n = GetMenuItemCount(menu);
    if (n > 0 && (GetMenuState(menu, (UINT)(n - 1), MF_BYPOSITION) & MF_SEPARATOR))
        RemoveMenu(menu, (UINT)(n - 1), MF_BYPOSITION);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITMENUPOPUP:
        for (int i = 0; i < g_npop; i++)
            if (g_pop[i].h == (HMENU)wp) {
                fill(g_pop[i].h, g_pop[i].items);
                return 0;
            }
        break;
    case WM_COMMAND:
        /* HIWORD 0: from a menu (1 would be an accelerator, which we do not use) */
        if (HIWORD(wp) == 0 && lp == 0 && LOWORD(wp) > MENU_CMD_NONE && LOWORD(wp) < MENU_CMD_COUNT) {
            menu_post(LOWORD(wp));
            return 0;
        }
        break;
    }
    return CallWindowProcW(g_prev, hwnd, msg, wp, lp);
}

int menu_native_attach(SDL_Window *win, MenuQueryFn query, void *user)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd || g_hwnd)
        return 0;
    g_query = query;
    g_user = user;
    menu_event_type(); /* registered here, on the main thread */
    g_bar = CreateMenu();
    if (!g_bar)
        return 0;
    for (const MenuItem *it = menu_bar(); it->kind != MENU_END; it++) {
        HMENU sub = popup_for(it->sub);
        if (sub) {
            fill(sub, it->sub);
            append(g_bar, MF_POPUP, (UINT_PTR)sub, it->label, "");
        }
    }
    g_hwnd = hwnd;
    g_prev = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)proc);
    if (!g_prev || !SetMenu(hwnd, g_bar)) {
        if (g_prev)
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev);
        DestroyMenu(g_bar);
        g_bar = NULL;
        g_hwnd = NULL;
        g_npop = 0;
        return 0;
    }
    g_shown = 1;
    return 1;
}

void menu_native_detach(SDL_Window *win)
{
    (void)win;
    if (!g_hwnd)
        return;
    SetMenu(g_hwnd, NULL);
    SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_prev);
    DestroyMenu(g_bar); /* and, recursively, the popups attached to it */
    for (int i = 0; i < g_npop; i++)
        if (IsMenu(g_pop[i].h))
            DestroyMenu(g_pop[i].h);
    g_npop = 0;
    g_bar = NULL;
    g_hwnd = NULL;
    g_shown = 0;
}

void menu_native_show(SDL_Window *win, int show)
{
    (void)win;
    if (!g_hwnd || !!show == g_shown)
        return;
    g_shown = !!show;
    SetMenu(g_hwnd, show ? g_bar : NULL);
    DrawMenuBar(g_hwnd);
}

void menu_native_fit(SDL_Window *win, int w, int h)
{
    (void)win;
    if (!g_hwnd || !g_shown || IsZoomed(g_hwnd))
        return;
    /* the bar's height depends on the width it wraps at, so this may take a second pass */
    for (int pass = 0; pass < 3; pass++) {
        RECT rc, wr;
        if (!GetClientRect(g_hwnd, &rc) || !GetWindowRect(g_hwnd, &wr))
            return;
        int dw = w - (rc.right - rc.left), dh = h - (rc.bottom - rc.top);
        if (!dw && !dh)
            return;
        SetWindowPos(g_hwnd, NULL, 0, 0, (wr.right - wr.left) + dw, (wr.bottom - wr.top) + dh,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

#endif
