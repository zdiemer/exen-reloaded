/* The menu bar of the SDL frontend: one model, three backends.
 *
 * The model (menu.c) is a static tree of MenuItem tables: File, Video, State, Options, Input,
 * Help. Each item has a command id (MENU_CMD_*), a label and an optional keyboard shortcut.
 * What can change at run time, whether an item is enabled, checked or shown and the text of
 * the dynamic ones (the recent files, the gamepads), is asked of the application through a
 * MenuQueryFn every time a menu opens, so the backends hold no state of their own and stay in
 * sync with each other and with the hotkeys.
 *
 * Backends:
 *   - Windows: a native Win32 menu bar attached to the SDL window (menu_win32.c).
 *   - macOS: native NSMenu items in the application's menu bar (menu_cocoa.m).
 *   - Everywhere else, and in fullscreen on Windows: an in-window microui menu strip (ui.c),
 *     opened with F10 or Alt.
 * Every backend reports a chosen item by pushing an SDL event of type menu_event_type() with
 * the command id in user.code (menu_post), so commands always run on the main loop, outside
 * any native menu callback. Keyboard shortcuts go through the same table (menu_shortcut).
 */
#ifndef EXEN_SDL_MENU_H
#define EXEN_SDL_MENU_H

#include <SDL3/SDL.h>

enum {
    MENU_CMD_NONE = 0,
    /* File */
    MENU_CMD_OPEN,
    MENU_CMD_RECENT0,
    MENU_CMD_RECENT_LAST = MENU_CMD_RECENT0 + 9,
    MENU_CMD_RECENT_CLEAR,
    MENU_CMD_RELOAD,
    MENU_CMD_CLOSE,
    MENU_CMD_EXIT,
    /* Video */
    MENU_CMD_SCALE1,
    MENU_CMD_SCALE_LAST = MENU_CMD_SCALE1 + 5, /* 1x-6x */
    MENU_CMD_FULLSCREEN,
    MENU_CMD_HANDSET_AUTO,
    MENU_CMD_HANDSET0, /* menu_handset_sizes order */
    MENU_CMD_HANDSET_LAST = MENU_CMD_HANDSET0 + 6,
    MENU_CMD_SCREENSHOT,
    /* State */
    MENU_CMD_QUICK_SAVE,
    MENU_CMD_QUICK_LOAD,
    MENU_CMD_SLOT0,
    MENU_CMD_SLOT_LAST = MENU_CMD_SLOT0 + 9,
    MENU_CMD_SLOT_PREV,
    MENU_CMD_SLOT_NEXT,
    MENU_CMD_STATES,
    /* Options */
    MENU_CMD_VOICE_RINGTONE,
    MENU_CMD_VOICE_GM,
    MENU_CMD_VOICE_TOGGLE,
    MENU_CMD_MUTE,
    MENU_CMD_VOLUME25,
    MENU_CMD_VOLUME50,
    MENU_CMD_VOLUME75,
    MENU_CMD_VOLUME100,
    MENU_CMD_SMS_SERVER,
    MENU_CMD_SMS_DELIVERED,
    MENU_CMD_SMS_OFFLINE,
    /* Input */
    MENU_CMD_CONTROLS,
    MENU_CMD_PAD0, /* the connected gamepads, read-only */
    MENU_CMD_PAD_LAST = MENU_CMD_PAD0 + 7,
    /* Help */
    MENU_CMD_HELP_CONTROLS,
    MENU_CMD_ABOUT,
    /* submenus, so that a whole submenu can be greyed out */
    MENU_CMD_SUB_RECENT,
    MENU_CMD_SUB_HANDSET,
    MENU_CMD_SUB_SLOT,
    MENU_CMD_SUB_VOICE,
    MENU_CMD_SUB_VOLUME,
    MENU_CMD_SUB_SMS,
    MENU_CMD_SUB_PADS,
    MENU_CMD_COUNT
};

enum { MENU_END = 0, MENU_ITEM, MENU_CHECK, MENU_RADIO, MENU_SUB, MENU_SEP };

typedef struct MenuItem {
    int kind;  /* MENU_* */
    int cmd;   /* MENU_CMD_*; for MENU_SUB, the id its state is queried with */
    /* UTF-8; '&' marks the access key (Win32 style, "&&" is a literal &). The query may
     * replace it (MenuItemState.text). */
    const char *label;
    SDL_Keycode key; /* shortcut, 0 if none */
    SDL_Keymod mod;  /* SDL_KMOD_CTRL or 0 */
    const struct MenuItem *sub; /* MENU_SUB: the submenu, ended by MENU_END */
} MenuItem;

typedef struct {
    int enabled, checked, hidden;
    char text[256]; /* label; starts as MenuItem.label */
} MenuItemState;

/* Fills in `st` for command `cmd`. Called with st preset to enabled, unchecked, shown and the
 * item's own label. */
typedef void (*MenuQueryFn)(void *user, int cmd, MenuItemState *st);

/* The menu bar: an array of MENU_SUB items ended by MENU_END. */
const MenuItem *menu_bar(void);

/* The state of `it` (defaults, then the query). */
void menu_item_state(const MenuItem *it, MenuQueryFn query, void *user, MenuItemState *st);

/* The command bound to a key press, or MENU_CMD_NONE. Modifiers must match exactly (Shift,
 * Ctrl, Alt; left and right count the same); on Apple platforms Cmd counts as Ctrl. */
int menu_shortcut(SDL_Keycode key, SDL_Keymod mod);

/* The shortcut as text ("Ctrl+O", "F5"), "" if none. */
void menu_shortcut_text(const MenuItem *it, char *buf, size_t n);
/* The label without its '&' markers. Returns the access key (lower case ASCII), 0 if none. */
int menu_plain_label(const char *label, char *buf, size_t n);

/* Find the item with command `cmd` anywhere in the tree (NULL if none). */
const MenuItem *menu_find(int cmd);

/* Handset screens of the Video > Handset screen submenu, MENU_CMD_HANDSET0 onwards. */
#define MENU_NHANDSETS 7
extern const int menu_handset_sizes[MENU_NHANDSETS][2];

/* The SDL event type that carries chosen commands (registered on first use). */
Uint32 menu_event_type(void);
/* Push command `cmd` as an event (thread-safe, like SDL_PushEvent). */
void menu_post(int cmd);

/* Native menu bar (menu_win32.c, menu_cocoa.m; stubs elsewhere). menu_native_attach returns 1
 * if `win` now has a native menu bar. menu_native_show hides or shows it again (Windows hides
 * it in fullscreen). menu_native_fit makes the window's client area exactly w x h, allowing for
 * a menu bar that wraps onto several lines in a narrow window. */
int menu_native_attach(SDL_Window *win, MenuQueryFn query, void *user);
void menu_native_detach(SDL_Window *win);
void menu_native_show(SDL_Window *win, int show);
void menu_native_fit(SDL_Window *win, int w, int h);

#endif
