/* The menu model (menu.h). */
#include "menu.h"

#define ELLIPSIS "\xE2\x80\xA6" /* U+2026 */
#define TIMES "\xC3\x97"        /* U+00D7 */

const int menu_handset_sizes[MENU_NHANDSETS][2] = {
    {101, 80}, {112, 112}, {120, 143}, {128, 128}, {128, 141}, {128, 160}, {132, 176},
};

#define ITEM(cmd, label, key, mod) {MENU_ITEM, cmd, label, key, mod, NULL}
#define CHECK(cmd, label, key, mod) {MENU_CHECK, cmd, label, key, mod, NULL}
#define RADIO(cmd, label, key, mod) {MENU_RADIO, cmd, label, key, mod, NULL}
#define SUB(cmd, label, sub) {MENU_SUB, cmd, label, 0, 0, sub}
#define SEP {MENU_SEP, 0, NULL, 0, 0, NULL}
#define END {MENU_END, 0, NULL, 0, 0, NULL}

/* labels of the dynamic items come from the query */
static const MenuItem recent_menu[] = {
    ITEM(MENU_CMD_RECENT0 + 0, "1", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 1, "2", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 2, "3", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 3, "4", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 4, "5", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 5, "6", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 6, "7", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 7, "8", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 8, "9", 0, 0),
    ITEM(MENU_CMD_RECENT0 + 9, "10", 0, 0),
    SEP,
    ITEM(MENU_CMD_RECENT_CLEAR, "&Clear Recent", 0, 0),
    END,
};

static const MenuItem file_menu[] = {
    ITEM(MENU_CMD_OPEN, "&Open" ELLIPSIS, SDLK_O, SDL_KMOD_CTRL),
    SUB(MENU_CMD_SUB_RECENT, "Open &Recent", recent_menu),
    ITEM(MENU_CMD_RELOAD, "Re&load", SDLK_R, SDL_KMOD_CTRL),
    ITEM(MENU_CMD_CLOSE, "&Close Game", 0, 0),
    SEP,
    ITEM(MENU_CMD_EXIT, "E&xit", 0, 0),
    END,
};

static const MenuItem handset_menu[] = {
    RADIO(MENU_CMD_HANDSET_AUTO, "&Auto (game database)", 0, 0),
    SEP,
    RADIO(MENU_CMD_HANDSET0 + 0, "101" TIMES "80", 0, 0),
    RADIO(MENU_CMD_HANDSET0 + 1, "112" TIMES "112", 0, 0),
    RADIO(MENU_CMD_HANDSET0 + 2, "120" TIMES "143", 0, 0),
    RADIO(MENU_CMD_HANDSET0 + 3, "128" TIMES "128", 0, 0),
    RADIO(MENU_CMD_HANDSET0 + 4, "128" TIMES "141", 0, 0),
    RADIO(MENU_CMD_HANDSET0 + 5, "128" TIMES "160", 0, 0),
    RADIO(MENU_CMD_HANDSET0 + 6, "132" TIMES "176", 0, 0),
    END,
};

static const MenuItem video_menu[] = {
    RADIO(MENU_CMD_SCALE1 + 0, "Scale &1" TIMES, 0, 0),
    RADIO(MENU_CMD_SCALE1 + 1, "Scale &2" TIMES, 0, 0),
    RADIO(MENU_CMD_SCALE1 + 2, "Scale &3" TIMES, 0, 0),
    RADIO(MENU_CMD_SCALE1 + 3, "Scale &4" TIMES, 0, 0),
    RADIO(MENU_CMD_SCALE1 + 4, "Scale &5" TIMES, 0, 0),
    RADIO(MENU_CMD_SCALE1 + 5, "Scale &6" TIMES, 0, 0),
    SEP,
    CHECK(MENU_CMD_FULLSCREEN, "&Fullscreen", SDLK_F11, 0),
    SUB(MENU_CMD_SUB_HANDSET, "&Handset Screen", handset_menu),
    SEP,
    ITEM(MENU_CMD_SCREENSHOT, "&Screenshot", SDLK_F12, 0),
    END,
};

static const MenuItem slot_menu[] = {
    RADIO(MENU_CMD_SLOT0 + 0, "Slot &0", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 1, "Slot &1", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 2, "Slot &2", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 3, "Slot &3", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 4, "Slot &4", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 5, "Slot &5", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 6, "Slot &6", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 7, "Slot &7", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 8, "Slot &8", 0, 0),
    RADIO(MENU_CMD_SLOT0 + 9, "Slot &9", 0, 0),
    SEP,
    ITEM(MENU_CMD_SLOT_PREV, "&Previous Slot", SDLK_F6, 0),
    ITEM(MENU_CMD_SLOT_NEXT, "&Next Slot", SDLK_F7, 0),
    END,
};

static const MenuItem state_menu[] = {
    ITEM(MENU_CMD_QUICK_SAVE, "Quick &Save", SDLK_F5, 0),
    ITEM(MENU_CMD_QUICK_LOAD, "Quick &Load", SDLK_F8, 0),
    SUB(MENU_CMD_SUB_SLOT, "&Current Slot", slot_menu),
    SEP,
    ITEM(MENU_CMD_STATES, "&Manage States" ELLIPSIS, SDLK_F4, 0),
    END,
};

static const MenuItem voice_menu[] = {
    RADIO(MENU_CMD_VOICE_RINGTONE, "&Ring Tone", 0, 0),
    RADIO(MENU_CMD_VOICE_GM, "&General MIDI", 0, 0),
    SEP,
    ITEM(MENU_CMD_VOICE_TOGGLE, "&Switch Voice", SDLK_V, 0),
    END,
};

static const MenuItem volume_menu[] = {
    RADIO(MENU_CMD_VOLUME25, "25%", 0, 0),
    RADIO(MENU_CMD_VOLUME50, "50%", 0, 0),
    RADIO(MENU_CMD_VOLUME75, "75%", 0, 0),
    RADIO(MENU_CMD_VOLUME100, "100%", 0, 0),
    END,
};

static const MenuItem sms_menu[] = {
    RADIO(MENU_CMD_SMS_SERVER, "&Mock Server", 0, 0),
    RADIO(MENU_CMD_SMS_DELIVERED, "&Delivered", 0, 0),
    RADIO(MENU_CMD_SMS_OFFLINE, "&Offline", 0, 0),
    END,
};

static const MenuItem options_menu[] = {
    SUB(MENU_CMD_SUB_VOICE, "Melody &Voice", voice_menu),
    CHECK(MENU_CMD_MUTE, "&Mute", SDLK_M, 0),
    SUB(MENU_CMD_SUB_VOLUME, "V&olume", volume_menu),
    SEP,
    SUB(MENU_CMD_SUB_SMS, "&SMS", sms_menu),
    END,
};

static const MenuItem pads_menu[] = {
    ITEM(MENU_CMD_PAD0 + 0, "1", 0, 0),
    ITEM(MENU_CMD_PAD0 + 1, "2", 0, 0),
    ITEM(MENU_CMD_PAD0 + 2, "3", 0, 0),
    ITEM(MENU_CMD_PAD0 + 3, "4", 0, 0),
    ITEM(MENU_CMD_PAD0 + 4, "5", 0, 0),
    ITEM(MENU_CMD_PAD0 + 5, "6", 0, 0),
    ITEM(MENU_CMD_PAD0 + 6, "7", 0, 0),
    ITEM(MENU_CMD_PAD0 + 7, "8", 0, 0),
    END,
};

static const MenuItem input_menu[] = {
    ITEM(MENU_CMD_CONTROLS, "&Controls" ELLIPSIS, SDLK_F1, 0),
    SUB(MENU_CMD_SUB_PADS, "&Gamepads", pads_menu),
    END,
};

static const MenuItem help_menu[] = {
    ITEM(MENU_CMD_HELP_CONTROLS, "&Controls", 0, 0),
    ITEM(MENU_CMD_ABOUT, "&About exen-reloaded", 0, 0),
    END,
};

static const MenuItem bar[] = {
    SUB(MENU_CMD_NONE, "&File", file_menu),
    SUB(MENU_CMD_NONE, "&Video", video_menu),
    SUB(MENU_CMD_NONE, "&State", state_menu),
    SUB(MENU_CMD_NONE, "&Options", options_menu),
    SUB(MENU_CMD_NONE, "&Input", input_menu),
    SUB(MENU_CMD_NONE, "&Help", help_menu),
    END,
};

const MenuItem *menu_bar(void)
{
    return bar;
}

void menu_item_state(const MenuItem *it, MenuQueryFn query, void *user, MenuItemState *st)
{
    st->enabled = 1;
    st->checked = 0;
    st->hidden = 0;
    SDL_strlcpy(st->text, it->label ? it->label : "", sizeof st->text);
    if (query && it->cmd != MENU_CMD_NONE)
        query(user, it->cmd, st);
}

static const MenuItem *find_in(const MenuItem *m, int cmd, SDL_Keycode key, SDL_Keymod mod, int any_mod)
{
    for (; m->kind != MENU_END; m++) {
        if (m->kind == MENU_SUB) {
            const MenuItem *r = find_in(m->sub, cmd, key, mod, any_mod);
            if (r)
                return r;
        } else if (m->kind != MENU_SEP) {
            if (cmd ? m->cmd == cmd : m->key && m->key == key && (any_mod || m->mod == mod))
                return m;
        }
    }
    return NULL;
}

const MenuItem *menu_find(int cmd)
{
    return cmd ? find_in(bar, cmd, 0, 0, 0) : NULL;
}

/* Shift, Ctrl and Alt, each side folded onto SDL_KMOD_SHIFT/CTRL/ALT. */
static SDL_Keymod fold_mod(SDL_Keymod mod)
{
    SDL_Keymod m = 0;
    if (mod & SDL_KMOD_SHIFT)
        m |= SDL_KMOD_SHIFT;
    if (mod & SDL_KMOD_CTRL)
        m |= SDL_KMOD_CTRL;
#ifdef SDL_PLATFORM_APPLE
    if (mod & SDL_KMOD_GUI)
        m |= SDL_KMOD_CTRL;
#endif
    if (mod & SDL_KMOD_ALT)
        m |= SDL_KMOD_ALT;
    return m;
}

int menu_shortcut(SDL_Keycode key, SDL_Keymod mod)
{
    if (!key)
        return MENU_CMD_NONE;
    const MenuItem *it = find_in(bar, 0, key, fold_mod(mod), 0);
    return it ? it->cmd : MENU_CMD_NONE;
}

void menu_shortcut_text(const MenuItem *it, char *buf, size_t n)
{
    if (!n)
        return;
    buf[0] = 0;
    if (!it || !it->key)
        return;
    const char *name = SDL_GetKeyName(it->key);
    SDL_snprintf(buf, n, "%s%s%s%s", it->mod & SDL_KMOD_CTRL ? "Ctrl+" : "",
                 it->mod & SDL_KMOD_ALT ? "Alt+" : "", it->mod & SDL_KMOD_SHIFT ? "Shift+" : "",
                 name ? name : "?");
}

int menu_plain_label(const char *label, char *buf, size_t n)
{
    int access = 0;
    size_t o = 0;
    if (!n)
        return 0;
    for (const char *s = label ? label : ""; *s; s++) {
        if (*s == '&') {
            if (s[1] == '&')
                s++; /* "&&": a literal & */
            else {
                if (s[1] && !access)
                    access = (unsigned char)s[1] < 0x80 ? SDL_tolower((unsigned char)s[1]) : 0;
                continue;
            }
        }
        if (o + 1 < n)
            buf[o++] = *s;
    }
    buf[o] = 0;
    return access;
}

Uint32 menu_event_type(void)
{
    static Uint32 type;
    if (!type)
        type = SDL_RegisterEvents(1);
    return type;
}

void menu_post(int cmd)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = menu_event_type();
    ev.user.code = cmd;
    SDL_PushEvent(&ev);
}

#if !defined(SDL_PLATFORM_WINDOWS) && !defined(SDL_PLATFORM_APPLE)
/* No native menu bar: the microui menu strip (ui.c) is used instead. */
int menu_native_attach(SDL_Window *win, MenuQueryFn query, void *user)
{
    (void)win;
    (void)query;
    (void)user;
    return 0;
}
void menu_native_detach(SDL_Window *win) { (void)win; }
void menu_native_show(SDL_Window *win, int show)
{
    (void)win;
    (void)show;
}
void menu_native_fit(SDL_Window *win, int w, int h)
{
    (void)win;
    (void)w;
    (void)h;
}
#endif
