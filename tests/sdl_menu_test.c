/* Tests for the SDL frontend's menu model (src/sdl/menu.c): the tree is well formed, every
 * command appears once, shortcuts are unique and resolve to their items, labels and shortcut
 * texts format as the backends expect, and the query decides what each backend shows. No
 * window is needed. Exit status 0 on success. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "menu.h"

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static int seen[MENU_CMD_COUNT];
static int nitems, nsubs, max_depth;

struct shortcut {
    SDL_Keycode key;
    SDL_Keymod mod;
    int cmd;
};
static struct shortcut shortcuts[64];
static int nshortcuts;

static void walk(const MenuItem *m, int depth)
{
    if (depth > max_depth)
        max_depth = depth;
    int n = 0;
    for (; m->kind != MENU_END; m++, n++) {
        CHECK(m->kind >= MENU_ITEM && m->kind <= MENU_SEP);
        if (m->kind == MENU_SEP) {
            CHECK(m->cmd == MENU_CMD_NONE && !m->key && !m->sub);
            continue;
        }
        CHECK(m->label && *m->label);
        if (m->kind == MENU_SUB) {
            CHECK(m->sub != NULL && !m->key);
            nsubs++;
            if (m->sub)
                walk(m->sub, depth + 1);
            continue;
        }
        nitems++;
        CHECK(m->cmd > MENU_CMD_NONE && m->cmd < MENU_CMD_COUNT);
        CHECK(!m->sub);
        if (m->cmd > MENU_CMD_NONE && m->cmd < MENU_CMD_COUNT)
            seen[m->cmd]++;
        if (m->key && nshortcuts < 64)
            shortcuts[nshortcuts++] = (struct shortcut){m->key, m->mod, m->cmd};
    }
    CHECK(n > 0);
}

static void test_tree(void)
{
    const MenuItem *bar = menu_bar();
    const char *const tops[] = {"File", "Video", "State", "Options", "Input", "Help"};
    int i = 0;
    char plain[64];
    for (const MenuItem *m = bar; m->kind != MENU_END; m++, i++) {
        CHECK(m->kind == MENU_SUB);
        CHECK(i < 6 && menu_plain_label(m->label, plain, sizeof plain) && !strcmp(plain, tops[i]));
    }
    CHECK(i == 6);
    walk(bar, 0);
    CHECK(max_depth == 2); /* File > Open Recent > 1 */
    /* every command but the submenu ids is an item, once */
    for (int c = MENU_CMD_NONE + 1; c < MENU_CMD_COUNT; c++) {
        int sub = c >= MENU_CMD_SUB_RECENT && c <= MENU_CMD_SUB_PADS;
        if (sub ? seen[c] != 0 : seen[c] != 1)
            printf("command %d appears %d times\n", c, seen[c]);
        CHECK(sub ? seen[c] == 0 : seen[c] == 1);
        if (!sub)
            CHECK(menu_find(c) && menu_find(c)->cmd == c);
    }
    CHECK(menu_find(MENU_CMD_NONE) == NULL);
    CHECK(menu_find(MENU_CMD_COUNT) == NULL);
    CHECK(MENU_CMD_COUNT < 0x8000); /* Win32 menu ids are 16-bit */
}

static void test_shortcuts(void)
{
    /* unique */
    for (int i = 0; i < nshortcuts; i++)
        for (int j = i + 1; j < nshortcuts; j++)
            CHECK(shortcuts[i].key != shortcuts[j].key || shortcuts[i].mod != shortcuts[j].mod);
    /* every one resolves to its item */
    for (int i = 0; i < nshortcuts; i++)
        CHECK(menu_shortcut(shortcuts[i].key, shortcuts[i].mod) == shortcuts[i].cmd);
    /* the documented hotkeys */
    CHECK(menu_shortcut(SDLK_O, SDL_KMOD_LCTRL) == MENU_CMD_OPEN);
    CHECK(menu_shortcut(SDLK_O, SDL_KMOD_RCTRL | SDL_KMOD_NUM) == MENU_CMD_OPEN); /* lock keys ignored */
    CHECK(menu_shortcut(SDLK_R, SDL_KMOD_CTRL) == MENU_CMD_RELOAD);
    CHECK(menu_shortcut(SDLK_F1, 0) == MENU_CMD_CONTROLS);
    CHECK(menu_shortcut(SDLK_F4, 0) == MENU_CMD_STATES);
    CHECK(menu_shortcut(SDLK_F5, 0) == MENU_CMD_QUICK_SAVE);
    CHECK(menu_shortcut(SDLK_F6, 0) == MENU_CMD_SLOT_PREV);
    CHECK(menu_shortcut(SDLK_F7, 0) == MENU_CMD_SLOT_NEXT);
    CHECK(menu_shortcut(SDLK_F8, 0) == MENU_CMD_QUICK_LOAD);
    CHECK(menu_shortcut(SDLK_F11, 0) == MENU_CMD_FULLSCREEN);
    CHECK(menu_shortcut(SDLK_F12, 0) == MENU_CMD_SCREENSHOT);
    CHECK(menu_shortcut(SDLK_M, 0) == MENU_CMD_MUTE);
    CHECK(menu_shortcut(SDLK_V, 0) == MENU_CMD_VOICE_TOGGLE);
    /* modifiers must match: plain O and R are game keys, Ctrl+M is not mute */
    CHECK(menu_shortcut(SDLK_O, 0) == MENU_CMD_NONE);
    CHECK(menu_shortcut(SDLK_R, SDL_KMOD_SHIFT) == MENU_CMD_NONE);
    CHECK(menu_shortcut(SDLK_M, SDL_KMOD_CTRL) == MENU_CMD_NONE);
    CHECK(menu_shortcut(SDLK_F5, SDL_KMOD_ALT) == MENU_CMD_NONE);
    CHECK(menu_shortcut(SDLK_F10, 0) == MENU_CMD_NONE); /* reserved for the menu strip */
    CHECK(menu_shortcut(0, 0) == MENU_CMD_NONE);
    /* none of the default game keys is a hotkey */
    const SDL_Keycode game_keys[] = {SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_W, SDLK_A, SDLK_S,
                                     SDLK_D, SDLK_Q, SDLK_E, SDLK_Z, SDLK_C, SDLK_X, SDLK_SPACE,
                                     SDLK_RETURN, SDLK_ESCAPE, SDLK_BACKSPACE, SDLK_LEFTBRACKET,
                                     SDLK_RIGHTBRACKET, SDLK_0, SDLK_5, SDLK_KP_5, SDLK_KP_ENTER};
    for (size_t i = 0; i < sizeof game_keys / sizeof *game_keys; i++)
        CHECK(menu_shortcut(game_keys[i], 0) == MENU_CMD_NONE);
}

static void test_text(void)
{
    char buf[64];
    menu_shortcut_text(menu_find(MENU_CMD_OPEN), buf, sizeof buf);
    CHECK(!strcmp(buf, "Ctrl+O"));
    menu_shortcut_text(menu_find(MENU_CMD_QUICK_SAVE), buf, sizeof buf);
    CHECK(!strcmp(buf, "F5"));
    menu_shortcut_text(menu_find(MENU_CMD_EXIT), buf, sizeof buf);
    CHECK(!strcmp(buf, ""));
    menu_shortcut_text(NULL, buf, sizeof buf);
    CHECK(!strcmp(buf, ""));

    CHECK(menu_plain_label("&File", buf, sizeof buf) == 'f' && !strcmp(buf, "File"));
    CHECK(menu_plain_label("Open &Recent", buf, sizeof buf) == 'r' && !strcmp(buf, "Open Recent"));
    CHECK(menu_plain_label("Tom && Jerry", buf, sizeof buf) == 0 && !strcmp(buf, "Tom & Jerry"));
    CHECK(menu_plain_label("&1 A&&B.exn", buf, sizeof buf) == '1' && !strcmp(buf, "1 A&B.exn"));
    CHECK(menu_plain_label("trailing&", buf, sizeof buf) == 0 && !strcmp(buf, "trailing"));
    CHECK(menu_plain_label(NULL, buf, sizeof buf) == 0 && !strcmp(buf, ""));
    CHECK(menu_plain_label("&Open\xE2\x80\xA6", buf, 6) == 'o' && !strcmp(buf, "Open\xE2"));

    /* access keys are unique within each menu (Alt+letter, the strip's letters) */
    const MenuItem *stack[8];
    int sp = 0;
    stack[sp++] = menu_bar();
    while (sp) {
        const MenuItem *m = stack[--sp];
        int used[128] = {0};
        for (; m->kind != MENU_END; m++) {
            if (m->kind == MENU_SEP)
                continue;
            int k = menu_plain_label(m->label, buf, sizeof buf);
            if (k > 0 && k < 128) {
                if (used[k])
                    printf("access key %c used twice (%s)\n", k, m->label);
                CHECK(!used[k]);
                used[k] = 1;
            }
            if (m->kind == MENU_SUB && sp < 8)
                stack[sp++] = m->sub;
        }
    }
}

/* A query like the app's: no game loaded, scale 3. */
static void query(void *user, int cmd, MenuItemState *st)
{
    int *calls = user;
    (*calls)++;
    if (cmd == MENU_CMD_RELOAD)
        st->enabled = 0;
    if (cmd == MENU_CMD_SCALE1 + 2)
        st->checked = 1;
    if (cmd == MENU_CMD_RECENT0 + 1)
        st->hidden = 1;
    if (cmd == MENU_CMD_RECENT0)
        SDL_strlcpy(st->text, "&1 Worms.exn", sizeof st->text);
}

static void test_state(void)
{
    int calls = 0;
    MenuItemState st;
    menu_item_state(menu_find(MENU_CMD_OPEN), query, &calls, &st);
    CHECK(st.enabled && !st.checked && !st.hidden && !strcmp(st.text, "&Open\xE2\x80\xA6"));
    menu_item_state(menu_find(MENU_CMD_RELOAD), query, &calls, &st);
    CHECK(!st.enabled);
    menu_item_state(menu_find(MENU_CMD_SCALE1 + 2), query, &calls, &st);
    CHECK(st.checked && st.enabled);
    menu_item_state(menu_find(MENU_CMD_RECENT0 + 1), query, &calls, &st);
    CHECK(st.hidden);
    menu_item_state(menu_find(MENU_CMD_RECENT0), query, &calls, &st);
    CHECK(!strcmp(st.text, "&1 Worms.exn"));
    CHECK(calls == 5);
    /* no query: defaults */
    menu_item_state(menu_find(MENU_CMD_MUTE), NULL, NULL, &st);
    CHECK(st.enabled && !st.checked && !st.hidden && !strcmp(st.text, "&Mute"));
    /* top-level menus are never queried (cmd 0) */
    menu_item_state(menu_bar(), query, &calls, &st);
    CHECK(calls == 5 && !strcmp(st.text, "&File"));
    /* the handset table matches its menu */
    for (int i = 0; i < MENU_NHANDSETS; i++) {
        char want[32], plain[64];
        SDL_snprintf(want, sizeof want, "%d\xC3\x97%d", menu_handset_sizes[i][0], menu_handset_sizes[i][1]);
        menu_plain_label(menu_find(MENU_CMD_HANDSET0 + i)->label, plain, sizeof plain);
        CHECK(!strcmp(plain, want));
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    test_tree();
    test_shortcuts();
    test_text();
    test_state();
    if (failures) {
        printf("sdl_menu_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sdl_menu_test: all passed (%d items, %d submenus, %d shortcuts)\n", nitems, nsubs, nshortcuts);
    return 0;
}
