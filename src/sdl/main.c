/* exen-reloaded SDL3 frontend: the app.
 *
 *   exen-reloaded [--sim gensimu.exe] [--scale N] [--size WxH] [--fullscreen]
 *                 [--mute] [--volume N] [--voice ringtone|gm] [--soundfont PATH]
 *                 [--sms MODE] [--keys-config PATH] [--no-native-menu] [game.exn]
 *   exen-reloaded [--keys-config PATH] --print-bindings
 *
 * The app starts with an empty window (or the game given on the command line). Games are
 * opened, reloaded, switched and closed from the menus (menu.h: a native menu bar on Windows
 * and macOS, the in-window menu strip elsewhere), by dropping a .exn on the window, or from
 * the recent list. Settings live in settings.ini (settings.h); command-line options override
 * them for one run.
 *
 * Pacing is event driven: the thread sleeps in SDL_WaitEventTimeout until the next game tick
 * or input arrives. The original simulator busy-waits in its menus and repaints through slow
 * per-pixel float conversion; nothing here spins. While a panel or the menu strip is open
 * (ui.h) the game is paused: no ticks, audio paused, and the VM's clock stops.
 *
 * Audio: the core renders the melodies (exen_audio_render). This thread pushes the PCM into an
 * SDL audio stream right after the events that start them, and keeps AUDIO_TARGET_MS queued
 * while a melody plays, waking when the queue falls to AUDIO_LOW_MS. The core therefore stays
 * single-threaded (no lock, no audio callback). When nothing plays, nothing is rendered and
 * the loop does not wake for audio.
 *
 * Like the original, a tick that runs late is not repeated: missed ticks are dropped rather
 * than queued, so a slow frame slows the game instead of causing a burst of catch-up ticks.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exen.h"
#include "input.h"
#include "menu.h"
#include "paths.h"
#include "persist.h"
#include "png.h"
#include "rgb332.h"
#include "settings.h"
#include "ui.h"

#ifndef EXEN_VERSION
#define EXEN_VERSION "dev"
#endif

#define EM_DASH "\xE2\x80\x94"
#define TRIANGLE "\xE2\x96\xB8"
#define TIMES "\xC3\x97"

#define AUDIO_RATE 48000
#define AUDIO_TARGET_MS 60
#define AUDIO_LOW_MS 30

/* Windows hides the native menu bar in fullscreen and uses the menu strip there instead */
#ifdef SDL_PLATFORM_WINDOWS
#define NATIVE_MENU_IN_FULLSCREEN 0
#else
#define NATIVE_MENU_IN_FULLSCREEN 1
#endif

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int w, h;               /* handset screen; EXEN_DEFAULT_* without a game */
    uint32_t *rgb;          /* XRGB8888 frame */
    uint8_t *px;            /* the RGB332 frame, for thumbnails and screenshots */
    int dirty;
    int period_ms;          /* < 0: timer stopped */
    uint64_t next_tick_ns;
    uint64_t start_ns;
    uint64_t paused_ns;     /* time spent paused since start_ns, not seen by the VM */
    uint64_t pause_since;
    int paused;
    uint32_t lut[256];      /* RGB332 -> XRGB8888 as the original display path computes it */
    SDL_AudioStream *audio; /* NULL: no audio device */

    /* settings in effect: settings.ini, then the command line */
    int scale, fullscreen, volume, muted, voice, sms;
    Settings set;
    char settings_path[1024];
    const char *keys_path;

    /* the framework, from gensimu.exe */
    const char *sim;
    void *simb;
    size_t siml;

    /* the General MIDI voice's SoundFont, read once at start; each VM keeps its own copy */
    void *sfb;
    size_t sfl;

    /* the game */
    ExenVM *vm;
    void *gameb;
    size_t gamel;
    char game_path[SETTINGS_PATH_MAX];
    char title[128];
    char key[80];           /* persist_game_key */
    int db_w, db_h;         /* the screen the game database gives */
    int halted_shown;
    int cli_w, cli_h;       /* --size, for the command-line game */
    char cli_game[SETTINGS_PATH_MAX];

    Input in;
    Ui *ui;
    int native_menu;        /* a native menu bar is attached */
    int strip, pinned;      /* what the UI was last told (ui_set_strip) */
    Uint32 open_event;      /* file dialog results */
    int dialog_open;
    int running;
} App;

static App g_app;

/* ---------------------------------------------------------------- host callbacks */

static void on_present(void *u, const uint8_t *px, int w, int h)
{
    App *a = u;
    if (w != a->w || h != a->h)
        return;
    SDL_memcpy(a->px, px, (size_t)w * (size_t)h);
    for (int i = 0; i < w * h; i++)
        a->rgb[i] = a->lut[px[i]];
    a->dirty = 1;
}

static void on_timer(void *u, int ms)
{
    App *a = u;
    a->period_ms = ms;
    if (ms > 0)
        a->next_tick_ns = SDL_GetTicksNS() + (uint64_t)ms * 1000000u;
}

static uint32_t on_ticks(void *u)
{
    App *a = u;
    uint64_t now = SDL_GetTicksNS();
    uint64_t paused = a->paused_ns + (a->paused ? now - a->pause_since : 0);
    return (uint32_t)((now - a->start_ns - paused) / 1000000u);
}

static void on_log(void *u, const char *m)
{
    (void)u;
    SDL_Log("%s", m);
}

/* Phone key transitions from the controls (input.c). */
static void on_phone_key(void *u, int code, int pressed)
{
    App *a = u;
    if (a->vm && !exen_halted(a->vm))
        exen_key(a->vm, code, pressed);
}

/* ---------------------------------------------------------------- helpers */

/* Top the audio stream up while the core has something to play. Returns the ms until the
 * queue falls to the low-water mark, or -1 if there is nothing to wait for. */
static int audio_pump(App *a)
{
    if (!a->audio || !a->vm || a->paused || !exen_audio_active(a->vm))
        return -1;
    int queued = SDL_GetAudioStreamQueued(a->audio) / 4;
    int target = AUDIO_RATE * AUDIO_TARGET_MS / 1000;
    int16_t buf[2 * 1024];
    while (queued < target) {
        int n = target - queued > 1024 ? 1024 : target - queued;
        exen_audio_render(a->vm, buf, n);
        SDL_PutAudioStreamData(a->audio, buf, n * 4);
        queued += n;
    }
    int ms = (queued - AUDIO_RATE * AUDIO_LOW_MS / 1000) * 1000 / AUDIO_RATE;
    return ms > 1 ? ms : 1;
}

static void set_title(App *a)
{
    char t[512];
    const char *m = a->muted ? " [muted]" : "";
    const char *st = persist_title(); /* transient status: slot, voice, ... */
    if (!a->vm)
        SDL_snprintf(t, sizeof t, "exen-reloaded%s%s", m, st);
    else if (exen_halted(a->vm))
        SDL_snprintf(t, sizeof t, "%s " EM_DASH " stopped: %s%s%s", a->title, exen_error(a->vm), m, st);
    else
        SDL_snprintf(t, sizeof t, "%s " EM_DASH " exen-reloaded%s%s", a->title, m, st);
    SDL_SetWindowTitle(a->win, t);
}

static void save_settings(App *a)
{
    settings_save(&a->set, a->settings_path);
}

static void message(App *a, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
static void message(App *a, const char *fmt, ...)
{
    char msg[1200];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SDL_Log("%s", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "exen-reloaded", msg, a->win);
}

/* The frame buffers and texture for a w x h screen, black. */
static int set_screen(App *a, int w, int h)
{
    free(a->rgb);
    free(a->px);
    a->w = w;
    a->h = h;
    a->rgb = calloc((size_t)w * (size_t)h, sizeof *a->rgb);
    a->px = calloc((size_t)w * (size_t)h, 1);
    if (!a->rgb || !a->px)
        return -1;
    for (int i = 0; i < w * h; i++)
        a->rgb[i] = 0xff000000u;
    if (a->tex)
        SDL_DestroyTexture(a->tex);
    a->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (a->tex)
        SDL_SetTextureScaleMode(a->tex, SDL_SCALEMODE_NEAREST);
    a->dirty = 1;
    return a->tex ? 0 : -1;
}

/* Window client area = screen x scale, exactly (unless fullscreen). */
static void fit_window(App *a)
{
    if (a->fullscreen)
        return;
    if (SDL_GetWindowFlags(a->win) & SDL_WINDOW_MAXIMIZED)
        SDL_RestoreWindow(a->win);
    int w = a->w * a->scale, h = a->h * a->scale;
    SDL_SetWindowSize(a->win, w, h);
    SDL_SyncWindow(a->win);
    if (a->native_menu)
        menu_native_fit(a->win, w, h);
    a->dirty = 1;
}

/* Native menu bar or menu strip, for the current state. */
static void update_menus(App *a)
{
    int native = a->native_menu && (NATIVE_MENU_IN_FULLSCREEN || !a->fullscreen);
    if (a->native_menu)
        menu_native_show(a->win, native);
#ifdef SDL_PLATFORM_WINDOWS
    /* with the bar shown, Alt and F10 open it and Alt+F the File menu; without it they are
     * the strip's (Windows would otherwise enter an invisible menu mode) */
    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MENU_MNEMONICS, native ? "1" : "0");
#endif
    int strip = !native, pinned = strip && !a->vm;
    if (strip != a->strip || pinned != a->pinned) {
        a->strip = strip;
        a->pinned = pinned;
        ui_set_strip(a->ui, strip, pinned);
    }
}

static void set_fullscreen(App *a, int on)
{
    a->fullscreen = on;
    if (on) {
        update_menus(a); /* the native bar goes first, so it is not part of the fullscreen size */
        SDL_SetWindowFullscreen(a->win, true);
        SDL_SyncWindow(a->win);
    } else {
        SDL_SetWindowFullscreen(a->win, false);
        SDL_SyncWindow(a->win);
        update_menus(a);
        fit_window(a);
    }
    a->dirty = 1;
}

static void set_paused(App *a, int p)
{
    p = p && a->vm;
    if (p == a->paused)
        return;
    uint64_t now = SDL_GetTicksNS();
    a->paused = p;
    if (p) {
        a->pause_since = now;
        input_release_all(&a->in); /* the game sees the keys go up before it stops */
        if (a->audio)
            SDL_PauseAudioStreamDevice(a->audio);
    } else {
        a->paused_ns += now - a->pause_since;
        if (a->period_ms > 0)
            a->next_tick_ns = now + (uint64_t)a->period_ms * 1000000u;
        if (a->audio)
            SDL_ResumeAudioStreamDevice(a->audio);
    }
}

/* ---------------------------------------------------------------- opening and closing */

static void close_game(App *a, int resize)
{
    if (!a->vm)
        return;
    set_paused(a, 0); /* resumes the audio device; the next game pauses again if need be */
    input_release_all(&a->in);
    persist_close(); /* flushes the EEPROM */
    exen_destroy(a->vm);
    a->vm = NULL;
    SDL_free(a->gameb);
    a->gameb = NULL;
    a->gamel = 0;
    a->game_path[0] = a->title[0] = a->key[0] = 0;
    a->period_ms = -1;
    a->paused_ns = 0;
    if (a->audio)
        SDL_ClearAudioStream(a->audio);
    if (ui_panel(a->ui) == UI_PANEL_STATES)
        ui_close(a->ui);
    set_screen(a, EXEN_DEFAULT_WIDTH, EXEN_DEFAULT_HEIGHT);
    update_menus(a);
    if (resize)
        fit_window(a);
    set_title(a);
}

/* Open (or reload, or switch to) the game at `path`. Returns 1 if it runs. */
static int open_game(App *a, const char *path_in)
{
    char path[SETTINGS_PATH_MAX];
    SDL_strlcpy(path, path_in, sizeof path); /* path_in may be a->game_path or in the recent list */
    if (!a->simb) {
        message(a, "Cannot find gensimu.exe, In-Fusio's original simulator, which exen-reloaded "
                   "reads the ExEn framework from.\n\nKeep it beside the app, or start the app with "
                   "--sim PATH.");
        return 0;
    }
    size_t gl = 0;
    void *gb = SDL_LoadFile(path, &gl);
    if (!gb) {
        message(a, "Cannot read %s:\n%s", path, SDL_GetError());
        return 0;
    }
    ExenGameInfo gi;
    exen_game_info(gb, gl, &gi);
    char key[80];
    persist_game_key(gb, gl, key, sizeof key);
    int w = gi.width, h = gi.height, ow, oh;
    if (a->cli_w > 0 && !SDL_strcmp(path, a->cli_game)) {
        w = a->cli_w;
        h = a->cli_h;
    } else if (settings_get_handset(&a->set, key, &ow, &oh)) {
        w = ow;
        h = oh;
    }

    close_game(a, 0);
    if (set_screen(a, w, h) < 0) {
        SDL_free(gb);
        message(a, "Out of memory.");
        return 0;
    }
    ExenConfig cfg = {0};
    cfg.screen_width = w;
    cfg.screen_height = h;
    cfg.audio_rate = AUDIO_RATE;
    cfg.host = (ExenHost){
        .user = a,
        .present = on_present,
        .set_timer = on_timer,
        .ticks_ms = on_ticks,
        .log = on_log,
        .eeprom_changed = persist_eeprom_changed,
    };
    a->start_ns = SDL_GetTicksNS();
    a->paused_ns = 0;
    a->period_ms = -1;
    ExenVM *vm = exen_create(&cfg);
    int ok = vm && exen_load_framework(vm, a->simb, a->siml) >= 0 && exen_load_game(vm, gb, gl) >= 0;
    if (ok) {
        persist_open(vm, a->sim, gb, gl);
        ok = exen_boot(vm) >= 0;
    }
    if (!ok) {
        message(a, "Cannot start %s:\n%s", exen_path_basename(path), vm ? exen_error(vm) : "out of memory");
        persist_close();
        if (vm)
            exen_destroy(vm);
        SDL_free(gb);
        set_screen(a, EXEN_DEFAULT_WIDTH, EXEN_DEFAULT_HEIGHT);
        fit_window(a);
        set_title(a);
        return 0;
    }
    a->vm = vm;
    a->gameb = gb;
    a->gamel = gl;
    a->db_w = gi.width;
    a->db_h = gi.height;
    a->halted_shown = 0;
    SDL_strlcpy(a->game_path, path, sizeof a->game_path);
    SDL_strlcpy(a->key, key, sizeof a->key);
    if (gi.match == EXEN_GAME_EXACT && gi.title[0]) {
        SDL_strlcpy(a->title, gi.title, sizeof a->title);
    } else {
        SDL_strlcpy(a->title, exen_path_basename(path), sizeof a->title);
        char *dot = SDL_strrchr(a->title, '.');
        if (dot && dot != a->title)
            *dot = 0;
    }
    exen_audio_set_volume(vm, a->muted ? 0 : a->volume);
    exen_audio_set_voice(vm, a->voice);
    if (a->sfb && exen_audio_set_soundfont(vm, a->sfb, a->sfl) < 0)
        SDL_Log("cannot use the SoundFont; the General MIDI voice plays a piano approximation");
    /* SMS: the mock server dates its rankings with today's date */
    exen_sms_set_mode(vm, a->sms);
    SDL_Time now;
    SDL_DateTime dt;
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
        exen_sms_set_date(vm, dt.year, dt.month, dt.day);

    settings_add_recent(&a->set, path);
    SDL_strlcpy(a->set.last_dir, path, sizeof a->set.last_dir);
    a->set.last_dir[exen_path_basename(a->set.last_dir) - a->set.last_dir] = 0;
    save_settings(a);
    update_menus(a);
    if (a->strip)
        persist_status("F10 or Alt: menu");
    if (ui_panel(a->ui) == UI_PANEL_STATES)
        ui_states_changed(a->ui);
    fit_window(a);
    set_title(a);
    a->dirty = 1;
    return 1;
}

static void SDLCALL dialog_done(void *user, const char *const *files, int filter)
{
    /* may run on another thread: hand the result to the main loop */
    App *a = user;
    (void)filter;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = a->open_event;
    if (!files) {
        ev.user.code = 1;
        ev.user.data1 = SDL_strdup(SDL_GetError());
    } else if (files[0]) {
        ev.user.data1 = SDL_strdup(files[0]);
    }
    SDL_PushEvent(&ev);
}

static void open_dialog(App *a)
{
    static const SDL_DialogFileFilter filters[] = {
        {"ExEn games (*.exn)", "exn"},
        {"All files", "*"},
    };
    if (a->dialog_open)
        return;
    a->dialog_open = 1;
    SDL_ShowOpenFileDialog(dialog_done, a, a->win, filters, 2, a->set.last_dir[0] ? a->set.last_dir : NULL, false);
}

static void screenshot(App *a)
{
    char path[1400], stamp[32] = "";
    SDL_Time now;
    SDL_DateTime dt;
    if (SDL_GetCurrentTime(&now) && SDL_TimeToDateTime(now, &dt, true))
        SDL_snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", dt.year, dt.month, dt.day, dt.hour,
                     dt.minute, dt.second);
    SDL_snprintf(path, sizeof path, "%sscreenshots/%s-%s.png", exen_data_dir(), a->key, stamp);
    for (int n = 2; SDL_GetPathInfo(path, NULL) && n < 100; n++)
        SDL_snprintf(path, sizeof path, "%sscreenshots/%s-%s-%d.png", exen_data_dir(), a->key, stamp, n);
    size_t len = 0;
    uint8_t *png = png_encode_xrgb8888(a->rgb, a->w, a->h, 1, &len);
    if (png && exen_write_file(path, png, len) == 0)
        persist_status("screenshot %s", exen_path_basename(path));
    else
        persist_status("screenshot failed");
    free(png);
}

/* ---------------------------------------------------------------- menus */

static void escape_amp(char *out, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 2 < n; s++) {
        if (*s == '&')
            out[o++] = '&';
        out[o++] = *s;
    }
    out[o] = 0;
}

static int handset_choice(App *a, int *w, int *h)
{
    return a->vm && settings_get_handset(&a->set, a->key, w, h);
}

static void app_query(void *user, int cmd, MenuItemState *st)
{
    App *a = user;
    int game = a->vm != NULL, ow = 0, oh = 0;
    int over = handset_choice(a, &ow, &oh);
    if (cmd >= MENU_CMD_RECENT0 && cmd <= MENU_CMD_RECENT_LAST) {
        int i = cmd - MENU_CMD_RECENT0;
        if (i >= a->set.nrecent) {
            st->hidden = i > 0;
            st->enabled = 0;
            SDL_strlcpy(st->text, "(none)", sizeof st->text);
        } else {
            char base[200];
            escape_amp(base, sizeof base, exen_path_basename(a->set.recent[i]));
            SDL_snprintf(st->text, sizeof st->text, "&%d %s", (i + 1) % 10, base);
        }
        return;
    }
    if (cmd >= MENU_CMD_SCALE1 && cmd <= MENU_CMD_SCALE_LAST) {
        st->checked = a->scale == cmd - MENU_CMD_SCALE1 + 1;
        return;
    }
    if (cmd >= MENU_CMD_HANDSET0 && cmd <= MENU_CMD_HANDSET_LAST) {
        const int *sz = menu_handset_sizes[cmd - MENU_CMD_HANDSET0];
        st->enabled = game;
        st->checked = over && ow == sz[0] && oh == sz[1];
        return;
    }
    if (cmd >= MENU_CMD_SLOT0 && cmd <= MENU_CMD_SLOT_LAST) {
        int s = cmd - MENU_CMD_SLOT0;
        PersistSlotInfo info;
        st->checked = persist_slot() == s;
        SDL_DateTime dt;
        if (persist_slot_info(s, &info) && SDL_TimeToDateTime(info.mtime, &dt, true))
            SDL_snprintf(st->text, sizeof st->text, "Slot &%d   %04d-%02d-%02d %02d:%02d", s, dt.year, dt.month,
                         dt.day, dt.hour, dt.minute);
        else if (game)
            SDL_snprintf(st->text, sizeof st->text, "Slot &%d   (empty)", s);
        return;
    }
    if (cmd >= MENU_CMD_PAD0 && cmd <= MENU_CMD_PAD_LAST) {
        int i = cmd - MENU_CMD_PAD0;
        st->enabled = 0; /* a list, not commands */
        if (i < a->in.npads) {
            const char *name = SDL_GetGamepadName(a->in.pads[i]);
            escape_amp(st->text, sizeof st->text, name ? name : "Gamepad");
        } else {
            st->hidden = i > 0;
            SDL_strlcpy(st->text, "(no gamepads)", sizeof st->text);
        }
        return;
    }
    PersistSlotInfo info;
    switch (cmd) {
    case MENU_CMD_RECENT_CLEAR: st->enabled = a->set.nrecent > 0; break;
    case MENU_CMD_RELOAD:
    case MENU_CMD_CLOSE:
    case MENU_CMD_SCREENSHOT:
    case MENU_CMD_STATES:
    case MENU_CMD_SUB_HANDSET: st->enabled = game; break;
    case MENU_CMD_FULLSCREEN: st->checked = a->fullscreen; break;
    case MENU_CMD_HANDSET_AUTO:
        st->enabled = game;
        st->checked = !over;
        if (game)
            SDL_snprintf(st->text, sizeof st->text, "&Auto (%d" TIMES "%d)", a->db_w, a->db_h);
        break;
    case MENU_CMD_QUICK_SAVE: st->enabled = game && !exen_halted(a->vm); break;
    case MENU_CMD_QUICK_LOAD: st->enabled = game && persist_slot_info(persist_slot(), &info); break;
    case MENU_CMD_VOICE_RINGTONE: st->checked = a->voice != EXEN_VOICE_GM; break;
    case MENU_CMD_VOICE_GM: st->checked = a->voice == EXEN_VOICE_GM; break;
    case MENU_CMD_MUTE: st->checked = a->muted; break;
    case MENU_CMD_VOLUME25: st->checked = a->volume == 25; break;
    case MENU_CMD_VOLUME50: st->checked = a->volume == 50; break;
    case MENU_CMD_VOLUME75: st->checked = a->volume == 75; break;
    case MENU_CMD_VOLUME100: st->checked = a->volume == 100; break;
    case MENU_CMD_SMS_SERVER: st->checked = a->sms == EXEN_SMS_SERVER; break;
    case MENU_CMD_SMS_DELIVERED: st->checked = a->sms == EXEN_SMS_DELIVERED; break;
    case MENU_CMD_SMS_OFFLINE: st->checked = a->sms == EXEN_SMS_OFFLINE; break;
    default: break;
    }
}

static void toggle_panel(App *a, int panel)
{
    if (ui_panel(a->ui) == panel)
        ui_close(a->ui);
    else
        ui_open(a->ui, panel);
}

static void set_volume(App *a)
{
    if (a->vm)
        exen_audio_set_volume(a->vm, a->muted ? 0 : a->volume);
    a->set.volume = a->volume;
    a->set.mute = a->muted;
    save_settings(a);
}

static void set_voice(App *a, int voice)
{
    a->voice = voice;
    if (a->vm) /* a host setting: the switch crossfades, the melody keeps time */
        exen_audio_set_voice(a->vm, voice);
    a->set.voice = voice;
    save_settings(a);
    persist_status("voice: %s", voice == EXEN_VOICE_GM ? "General MIDI" : "ring tone");
}

static void choose_handset(App *a, int w, int h)
{
    settings_set_handset(&a->set, a->key, w, h);
    save_settings(a);
    if (!w) {
        w = a->db_w;
        h = a->db_h;
    }
    if (!SDL_strcmp(a->game_path, a->cli_game))
        a->cli_w = a->cli_h = 0; /* the menu choice replaces --size */
    if (w == a->w && h == a->h)
        return;
    char msg[400];
    SDL_snprintf(msg, sizeof msg,
                 "The game sees the new %dx%d screen when it starts. Restart %s now?\n\n"
                 "Progress since the last save is lost; save states are for one screen size.",
                 w, h, a->title);
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Restart now"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Later"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, a->win, "Handset screen", msg, 2, buttons, NULL};
    int choice = 0;
    if (SDL_ShowMessageBox(&box, &choice) && choice == 1)
        open_game(a, a->game_path);
    else
        persist_status("%dx%d from the next reload", w, h);
}

static void app_command(App *a, int cmd)
{
    const MenuItem *it = menu_find(cmd);
    if (!it)
        return;
    MenuItemState st;
    menu_item_state(it, app_query, a, &st);
    if (!st.enabled || st.hidden)
        return;
    if (cmd >= MENU_CMD_RECENT0 && cmd <= MENU_CMD_RECENT_LAST) {
        int i = cmd - MENU_CMD_RECENT0;
        char path[SETTINGS_PATH_MAX];
        SDL_strlcpy(path, a->set.recent[i], sizeof path);
        if (!SDL_GetPathInfo(path, NULL)) {
            message(a, "%s no longer exists; it is removed from the recent list.", path);
            settings_remove_recent(&a->set, i);
            save_settings(a);
            return;
        }
        open_game(a, path);
        return;
    }
    if (cmd >= MENU_CMD_SCALE1 && cmd <= MENU_CMD_SCALE_LAST) {
        a->scale = a->set.scale = cmd - MENU_CMD_SCALE1 + 1;
        save_settings(a);
        fit_window(a);
        return;
    }
    if (cmd >= MENU_CMD_HANDSET0 && cmd <= MENU_CMD_HANDSET_LAST) {
        const int *sz = menu_handset_sizes[cmd - MENU_CMD_HANDSET0];
        choose_handset(a, sz[0], sz[1]);
        return;
    }
    if (cmd >= MENU_CMD_SLOT0 && cmd <= MENU_CMD_SLOT_LAST) {
        persist_select_slot(cmd - MENU_CMD_SLOT0);
        set_title(a);
        return;
    }
    switch (cmd) {
    case MENU_CMD_OPEN: open_dialog(a); break;
    case MENU_CMD_RECENT_CLEAR:
        a->set.nrecent = 0;
        save_settings(a);
        break;
    case MENU_CMD_RELOAD: open_game(a, a->game_path); break;
    case MENU_CMD_CLOSE: close_game(a, 1); break;
    case MENU_CMD_EXIT: a->running = 0; break;
    case MENU_CMD_FULLSCREEN:
        set_fullscreen(a, !a->fullscreen);
        a->set.fullscreen = a->fullscreen;
        save_settings(a);
        break;
    case MENU_CMD_HANDSET_AUTO: choose_handset(a, 0, 0); break;
    case MENU_CMD_SCREENSHOT: screenshot(a); break;
    case MENU_CMD_QUICK_SAVE:
        persist_save(persist_slot(), a->px, a->w, a->h);
        if (ui_panel(a->ui) == UI_PANEL_STATES)
            ui_states_changed(a->ui);
        break;
    case MENU_CMD_QUICK_LOAD:
        input_release_all(&a->in); /* into the old state */
        if (persist_load(persist_slot())) {
            if (a->audio)
                SDL_ClearAudioStream(a->audio);
            a->halted_shown = 0;
        }
        break;
    case MENU_CMD_SLOT_PREV: persist_select_slot(persist_slot() - 1); break;
    case MENU_CMD_SLOT_NEXT: persist_select_slot(persist_slot() + 1); break;
    case MENU_CMD_STATES: toggle_panel(a, UI_PANEL_STATES); break;
    case MENU_CMD_VOICE_RINGTONE: set_voice(a, EXEN_VOICE_RINGTONE); break;
    case MENU_CMD_VOICE_GM: set_voice(a, EXEN_VOICE_GM); break;
    case MENU_CMD_VOICE_TOGGLE:
        set_voice(a, a->voice == EXEN_VOICE_GM ? EXEN_VOICE_RINGTONE : EXEN_VOICE_GM);
        break;
    case MENU_CMD_MUTE:
        a->muted = !a->muted;
        set_volume(a);
        break;
    case MENU_CMD_VOLUME25:
    case MENU_CMD_VOLUME50:
    case MENU_CMD_VOLUME75:
    case MENU_CMD_VOLUME100:
        a->volume = 25 * (cmd - MENU_CMD_VOLUME25 + 1);
        a->muted = 0;
        set_volume(a);
        persist_status("volume %d%%", a->volume);
        break;
    case MENU_CMD_SMS_SERVER:
    case MENU_CMD_SMS_DELIVERED:
    case MENU_CMD_SMS_OFFLINE:
        a->sms = a->set.sms = cmd == MENU_CMD_SMS_SERVER ? EXEN_SMS_SERVER
                              : cmd == MENU_CMD_SMS_DELIVERED ? EXEN_SMS_DELIVERED
                                                              : EXEN_SMS_OFFLINE;
        if (a->vm)
            exen_sms_set_mode(a->vm, a->sms);
        save_settings(a);
        persist_status("SMS: %s", a->sms == EXEN_SMS_SERVER ? "mock server" : a->sms == EXEN_SMS_DELIVERED ? "delivered" : "offline");
        break;
    case MENU_CMD_CONTROLS: toggle_panel(a, UI_PANEL_CONTROLS); break;
    case MENU_CMD_HELP_CONTROLS: toggle_panel(a, UI_PANEL_HELP); break;
    case MENU_CMD_ABOUT: toggle_panel(a, UI_PANEL_ABOUT); break;
    default: break;
    }
    set_title(a);
}

/* ---------------------------------------------------------------- events and drawing */

static void handle_event(App *a, const SDL_Event *ev)
{
    if (ev->type == SDL_EVENT_QUIT) {
        a->running = 0;
        return;
    }
    if (ev->type == menu_event_type()) {
        app_command(a, ev->user.code);
        a->dirty = 1;
        return;
    }
    if (ev->type == a->open_event) {
        a->dialog_open = 0;
        if (ev->user.code)
            message(a, "Cannot show the file dialog: %s", ev->user.data1 ? (const char *)ev->user.data1 : "");
        else if (ev->user.data1)
            open_game(a, ev->user.data1);
        SDL_free(ev->user.data1);
        a->dirty = 1;
        return;
    }
    switch (ev->type) {
    case SDL_EVENT_DROP_FILE:
        if (ev->drop.data)
            open_game(a, ev->drop.data);
        return;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        a->dirty = 1;
        return;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ui_strip_key(a->ui, ev);
        input_handle_event(&a->in, ev); /* releases held keys */
        return;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        input_handle_event(&a->in, ev);
        a->dirty = 1;
        return;
    }
    int key = ev->type == SDL_EVENT_KEY_DOWN || ev->type == SDL_EVENT_KEY_UP;
    /* hotkeys work everywhere, but with the strip open plain letters are its access keys, and
     * a panel takes every key except the ones that toggle panels and fullscreen */
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode k = ev->key.key;
        int plain = ((k >= 'a' && k <= 'z') || (k >= '0' && k <= '9')) && !(ev->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI));
        int cmd = ui_active(a->ui) && plain ? MENU_CMD_NONE : menu_shortcut(k, ev->key.mod);
        if (ui_panel(a->ui) && cmd != MENU_CMD_CONTROLS && cmd != MENU_CMD_STATES && cmd != MENU_CMD_FULLSCREEN)
            cmd = MENU_CMD_NONE;
        if (cmd) {
            if (!ev->key.repeat) {
                if (ui_active(a->ui))
                    ui_show_strip(a->ui, 0);
                app_command(a, cmd);
            }
            a->dirty = 1;
            return;
        }
    }
    if (ui_active(a->ui)) {
        if (ui_event(a->ui, ev))
            a->dirty = 1;
        return;
    }
    if (ui_strip_key(a->ui, ev)) {
        a->dirty = 1;
        return;
    }
    if (key && menu_shortcut(ev->key.key, 0))
        return; /* the key-up of a hotkey */
    input_handle_event(&a->in, ev);
}

static void render(App *a)
{
    SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 255);
    SDL_RenderClear(a->ren);
    if (a->vm && a->tex) {
        int ow = 0, oh = 0;
        SDL_GetCurrentRenderOutputSize(a->ren, &ow, &oh);
        SDL_UpdateTexture(a->tex, NULL, a->rgb, a->w * 4);
        /* the largest whole scale that fits, centred; a fraction only if even 1x does not */
        int s = SDL_min(ow / a->w, oh / a->h), dw, dh;
        if (s >= 1) {
            dw = a->w * s;
            dh = a->h * s;
        } else {
            float k = SDL_min((float)ow / a->w, (float)oh / a->h);
            dw = (int)(a->w * k);
            dh = (int)(a->h * k);
        }
        SDL_FRect d = {(float)((ow - dw) / 2), (float)((oh - dh) / 2), (float)dw, (float)dh};
        SDL_RenderTexture(a->ren, a->tex, NULL, &d);
    } else {
        ui_draw_hint(a->ui, "File " TRIANGLE " Open a game,\nor drop a .exn here");
    }
    ui_render(a->ui);
    SDL_RenderPresent(a->ren);
}

/* ---------------------------------------------------------------- start-up */

/* --keys-config, else keys.ini in the data directory. */
static const char *controls_path(const char *given)
{
    static char buf[1024];
    if (given)
        return given;
    SDL_snprintf(buf, sizeof buf, "%skeys.ini", exen_data_dir());
    return buf;
}

static const char *find_sim(const char *given)
{
    static char buf[1024];
    if (given)
        return given;
    const char *base = SDL_GetBasePath();
    const char *cands[] = {"gensimu.exe", "vendor/gensimu/gensimu.exe", "../vendor/gensimu/gensimu.exe"};
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
        SDL_snprintf(buf, sizeof buf, "%s%s", base ? base : "", cands[i]);
        if (SDL_GetPathInfo(buf, NULL))
            return buf;
        if (SDL_GetPathInfo(cands[i], NULL))
            return cands[i];
    }
    return NULL;
}

/* --soundfont, else TimGM6mb.sf2 in soundfont/ beside the app or beside it, else the source
 * tree's vendor/soundfont/ (each also relative to the working directory). NULL: none. */
static const char *find_soundfont(const char *given)
{
    static char buf[1024];
    if (given)
        return given;
    const char *base = SDL_GetBasePath();
    const char *cands[] = {"soundfont/TimGM6mb.sf2", "TimGM6mb.sf2",
                           "vendor/soundfont/TimGM6mb.sf2", "../vendor/soundfont/TimGM6mb.sf2"};
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
        SDL_snprintf(buf, sizeof buf, "%s%s", base ? base : "", cands[i]);
        if (SDL_GetPathInfo(buf, NULL))
            return buf;
        if (SDL_GetPathInfo(cands[i], NULL))
            return cands[i];
    }
    return NULL;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: exen-reloaded [options] [game.exn]\n"
            "  --sim PATH      original simulator (gensimu.exe); searched beside the app by default\n"
            "  --scale N       window scale (default 4, or as last set in the Video menu)\n"
            "  --size WxH      screen size of the target handset for game.exn (default: from the\n"
            "                  game database, else 132x176)\n"
            "  --fullscreen    start fullscreen (F11 toggles)\n"
            "  --mute          start with the sound off (M toggles)\n"
            "  --volume N      sound volume 0-100 (default 100)\n"
            "  --voice V       melody voice: ringtone (default, a 2003 handset) or gm (General\n"
            "                  MIDI, like the 2003 simulator; V toggles). \"piano\" = gm\n"
            "  --soundfont PATH\n"
            "                  General MIDI SoundFont for the gm voice (default: TimGM6mb.sf2\n"
            "                  in soundfont/ beside the app, or beside it; without one the gm\n"
            "                  voice is a piano approximation)\n"
            "  --sms MODE      SMS: server (default; a mock of In-Fusio's SMS server answers),\n"
            "                  delivered (sent, never answered) or offline (not sent)\n"
            "  --keys-config PATH\n"
            "                  controls file (default: keys.ini beside the app, or in the user\n"
            "                  data directory if that is not writable; written with the defaults)\n"
            "  --no-native-menu\n"
            "                  use the in-window menu (F10 or Alt) instead of the menu bar\n"
            "  --print-bindings\n"
            "                  print the effective controls and exit\n"
            "Options override settings.ini (written beside keys.ini) for this run.\n"
            "default controls (remap them in Input > Controls or keys.ini; see README.md):\n"
            "  keyboard: arrows or W/A/S/D = 2/4/6/8, Q/E/Z/C = 1/3/7/9, Space = 5, X = 0,\n"
            "            [ = *, ] = #, digits and numpad = phone keypad (numpad 7 = 1),\n"
            "            Enter = OK/select, Esc/Backspace = clear/back\n"
            "  gamepad:  d-pad/left stick = 2/4/6/8, A = 5, B/Back = clear, Start = OK,\n"
            "            X = *, Y = #, LB/RB = 1/3, LT/RT = 7/9, stick clicks = 0\n"
            "  hotkeys:  Ctrl+O = open, Ctrl+R = reload, F1 = controls, F4 = save states,\n"
            "            F5 = save state, F8 = load state, F6/F7 = previous/next slot (0-9),\n"
            "            M = mute, V = melody voice, F11 = fullscreen, F12 = screenshot\n");
}

int main(int argc, char **argv)
{
    const char *sim = NULL, *game = NULL;
    const char *keys_config = NULL, *soundfont = NULL;
    int scale = 0, w = 0, h = 0, fullscreen = 0, mute = 0, volume = -1, print_bindings = 0;
    int voice = -1, sms = -1, no_native_menu = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim") && i + 1 < argc) sim = argv[++i];
        else if (!strcmp(argv[i], "--soundfont") && i + 1 < argc) soundfont = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) volume = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "piano") || !strcmp(v, "gm"))
                voice = EXEN_VOICE_GM;
            else if (!strcmp(v, "ringtone"))
                voice = EXEN_VOICE_RINGTONE;
            else { usage(); return 2; }
        }
        else if (!strcmp(argv[i], "--sms") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "server"))
                sms = EXEN_SMS_SERVER;
            else if (!strcmp(v, "delivered"))
                sms = EXEN_SMS_DELIVERED;
            else if (!strcmp(v, "offline"))
                sms = EXEN_SMS_OFFLINE;
            else { usage(); return 2; }
        }
        else if (!strcmp(argv[i], "--keys-config") && i + 1 < argc) keys_config = argv[++i];
        else if (!strcmp(argv[i], "--no-native-menu")) no_native_menu = 1;
        else if (!strcmp(argv[i], "--print-bindings")) print_bindings = 1;
        else if (argv[i][0] == '-') { usage(); return 2; }
        else game = argv[i];
    }
    if (print_bindings) {
        Input in;
        input_init(&in, NULL, NULL);
        const char *path = controls_path(keys_config);
        int loaded = input_load_or_create(&in, path, 0);
        char *text = input_format_config(&in, 0);
        printf("; %s %s\n%s", loaded ? "controls from" : "built-in controls; no", path, text ? text : "");
        SDL_free(text);
        return 0;
    }
    if (w < 0 || h < 0 || scale < 0 || scale > 16 || volume < -1 || volume > 100) {
        usage();
        return 2;
    }
    SDL_SetAppMetadata("exen-reloaded", EXEN_VERSION, "io.github.zdiemer.exen-reloaded");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    App *a = &g_app;
    a->running = 1;
    a->period_ms = -1;
    exen_rgb332_lut(a->lut); /* EXManufDisplayUpdate (0x4342f0), shared with the libretro core */

    /* settings.ini, then the command line for this run */
    SDL_snprintf(a->settings_path, sizeof a->settings_path, "%ssettings.ini", exen_data_dir());
    settings_load(&a->set, a->settings_path);
    a->scale = scale ? scale : a->set.scale;
    a->fullscreen = fullscreen || a->set.fullscreen;
    a->muted = mute || a->set.mute;
    a->volume = volume >= 0 ? volume : a->set.volume;
    a->voice = voice >= 0 ? voice : a->set.voice;
    a->sms = sms >= 0 ? sms : a->set.sms;
    if (game && w > 0 && h > 0) {
        a->cli_w = w;
        a->cli_h = h;
        SDL_strlcpy(a->cli_game, game, sizeof a->cli_game);
    }

    a->sim = find_sim(sim);
    a->simb = a->sim ? SDL_LoadFile(a->sim, &a->siml) : NULL;
    {
        const char *sf = find_soundfont(soundfont);
        a->sfb = sf ? SDL_LoadFile(sf, &a->sfl) : NULL;
        if (a->sfb)
            SDL_Log("General MIDI SoundFont: %s", sf);
        else
            SDL_Log("no SoundFont (TimGM6mb.sf2) found; the General MIDI voice plays a piano "
                    "approximation (see --soundfont)");
    }

    if (!SDL_CreateWindowAndRenderer("exen-reloaded", EXEN_DEFAULT_WIDTH * a->scale, EXEN_DEFAULT_HEIGHT * a->scale,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN, &a->win, &a->ren)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(a->ren, 1);
    a->open_event = SDL_RegisterEvents(1);
    menu_event_type();

    /* controls (input.c); gamepads arrive as SDL_EVENT_GAMEPAD_ADDED */
    a->keys_path = controls_path(keys_config);
    input_init(&a->in, on_phone_key, a);
    input_load_or_create(&a->in, a->keys_path, 1);
    input_open_gamepads(&a->in);

    UiHost host = {a, app_query, &a->in, a->keys_path, EXEN_VERSION};
    a->ui = ui_create(a->ren, &host);
    if (!a->ui) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (!no_native_menu && menu_native_attach(a->win, app_query, a))
        a->native_menu = 1;
    a->strip = -1;
    set_screen(a, EXEN_DEFAULT_WIDTH, EXEN_DEFAULT_HEIGHT);
    update_menus(a);
    {
        int fs = a->fullscreen;
        a->fullscreen = 0;
        fit_window(a);
        SDL_ShowWindow(a->win);
        if (fs)
            set_fullscreen(a, 1);
    }

    /* Audio is optional: without a device the game runs silently. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, AUDIO_RATE};
        a->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (a->audio)
            SDL_ResumeAudioStreamDevice(a->audio);
    }
    if (!a->audio)
        SDL_Log("audio disabled: %s", SDL_GetError());

    set_title(a);
    if (!a->simb)
        message(a, "Cannot find gensimu.exe, In-Fusio's original simulator, which exen-reloaded "
                   "reads the ExEn framework from.\n\nKeep it beside the app, or start the app with "
                   "--sim PATH.");
    else if (game)
        open_game(a, game);

    int audio_wait = audio_pump(a);
    a->dirty = 1;
    while (a->running) {
        set_paused(a, ui_active(a->ui));
        /* sleep until the next tick or an event */
        int timeout = -1;
        if (a->vm && !a->paused && a->period_ms > 0 && !exen_halted(a->vm)) {
            uint64_t now = SDL_GetTicksNS();
            timeout = a->next_tick_ns > now ? (int)((a->next_tick_ns - now + 999999u) / 1000000u) : 0;
        }
        if (audio_wait >= 0 && (timeout < 0 || audio_wait < timeout))
            timeout = audio_wait;
        int status_wait = persist_title_wait_ms(); /* the status in the title expires */
        if (status_wait == 0)
            set_title(a);
        else if (status_wait > 0 && (timeout < 0 || status_wait < timeout))
            timeout = status_wait;
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, timeout)) {
            do {
                handle_event(a, &ev);
            } while (a->running && SDL_PollEvent(&ev));
        }
        if (!a->running)
            break;
        set_paused(a, ui_active(a->ui));
        if (a->vm && !a->paused && a->period_ms > 0 && !exen_halted(a->vm)) {
            uint64_t now = SDL_GetTicksNS();
            if (now >= a->next_tick_ns) {
                exen_tick(a->vm);
                uint64_t period = (uint64_t)a->period_ms * 1000000u;
                a->next_tick_ns += period;
                if (a->next_tick_ns <= now) /* late: drop missed ticks */
                    a->next_tick_ns = now + period;
            }
        }
        audio_wait = audio_pump(a);
        if (a->vm && exen_halted(a->vm) && !a->halted_shown) {
            a->halted_shown = 1;
            set_title(a);
        }
        if (a->dirty) {
            a->dirty = 0;
            render(a);
        }
    }
    close_game(a, 0);
    ui_destroy(a->ui);
    if (a->native_menu)
        menu_native_detach(a->win);
    input_close_gamepads(&a->in);
    if (a->audio)
        SDL_DestroyAudioStream(a->audio);
    SDL_free(a->simb);
    SDL_free(a->sfb);
    free(a->rgb);
    free(a->px);
    SDL_Quit();
    return 0;
}
