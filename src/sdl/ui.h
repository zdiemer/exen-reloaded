/* In-window UI of the SDL frontend, drawn with microui (vendor/microui) and the SDL renderer:
 *
 *   - the menu strip: the menu model (menu.h) as a bar at the top of the window with drop-down
 *     menus, for platforms without a native menu bar and for fullscreen on Windows. F10 or a
 *     tap of Alt shows it; it covers the top of the game image without changing its scaling.
 *   - panels: Controls (remap the keyboard and gamepads), Save States (slots with thumbnails),
 *     Help (the bindings and hotkeys) and About.
 *
 * While the strip or a panel is open the UI is "active": the game is paused (the caller stops
 * ticks and audio) and the caller passes every input event to ui_event. Everything can be
 * driven by mouse, keyboard or gamepad; Esc (or B) closes. Text is drawn with an embedded 8x8
 * bitmap font (font8x8.h), scaled with the window.
 *
 * Chosen menu items and panel actions that change the game (load or save a state) are posted
 * as menu commands (menu_post) for the main loop, as the native menus do.
 */
#ifndef EXEN_SDL_UI_H
#define EXEN_SDL_UI_H

#include <SDL3/SDL.h>

#include "input.h"
#include "menu.h"

enum { UI_PANEL_NONE = 0, UI_PANEL_CONTROLS, UI_PANEL_STATES, UI_PANEL_HELP, UI_PANEL_ABOUT };

typedef struct {
    void *user;
    MenuQueryFn query;     /* the state of the menu items, for the strip */
    Input *input;          /* edited by the Controls panel */
    const char *keys_path; /* where the Controls panel saves (keys.ini) */
    const char *version;   /* for About */
} UiHost;

typedef struct Ui Ui;

Ui *ui_create(SDL_Renderer *ren, const UiHost *host);
void ui_destroy(Ui *ui);

/* Use the menu strip (no native menu bar). `pinned` keeps it shown (no game loaded): Esc and
 * F10 then only close its drop-downs. */
void ui_set_strip(Ui *ui, int enabled, int pinned);
int ui_strip_enabled(const Ui *ui);
void ui_show_strip(Ui *ui, int show);

void ui_open(Ui *ui, int panel);
void ui_close(Ui *ui);
int ui_panel(const Ui *ui);

/* A panel or the strip is open: the game is paused and input belongs to the UI. */
int ui_active(const Ui *ui);

/* F10 / Alt tap toggles the strip (when enabled). Call for every key event while no panel is
 * open, before the game sees it. Returns 1 if the event was consumed. */
int ui_strip_key(Ui *ui, const SDL_Event *ev);

/* Feed an input event while ui_active. Returns 1 if consumed (everything but quit, window and
 * gamepad hotplug events). */
int ui_event(Ui *ui, const SDL_Event *ev);

/* The slots changed (saved, loaded, deleted, another game): reload the state panel's list. */
void ui_states_changed(Ui *ui);

/* Draw the UI over the frame being rendered (does nothing when not active). */
void ui_render(Ui *ui);

/* A centred, word-wrapped message in the UI font (the empty window's hint). Lines are
 * separated by '\n'. */
void ui_draw_hint(Ui *ui, const char *text);

#endif
