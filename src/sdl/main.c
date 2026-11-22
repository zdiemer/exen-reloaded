/* exen-reloaded SDL3 frontend.
 *
 *   exen-reloaded [--sim gensimu.exe] [--scale N] [--size WxH] [--fullscreen]
 *                 [--mute] [--volume N] [--keys-config PATH] game.exn
 *   exen-reloaded [--keys-config PATH] --print-bindings
 *
 * Pacing is event driven: the thread sleeps in SDL_WaitEventTimeout until the next game tick
 * or input arrives. The original simulator busy-waits in its menus and repaints through slow
 * per-pixel float conversion; nothing here spins.
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
#include "paths.h"
#include "persist.h"

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int w, h;
    uint32_t *rgb;          /* XRGB8888 frame */
    int dirty;
    int period_ms;          /* < 0: timer stopped */
    uint64_t next_tick_ns;
    uint64_t start_ns;
    uint32_t lut[256];      /* RGB332 -> XRGB8888 as the original display path computes it */
    SDL_AudioStream *audio; /* NULL: no audio device */
    int volume, muted;
} App;

#define AUDIO_RATE 48000
#define AUDIO_TARGET_MS 60
#define AUDIO_LOW_MS 30

/* Top the audio stream up while the core has something to play. Returns the ms until the
 * queue falls to the low-water mark, or -1 if there is nothing to wait for. */
static int audio_pump(App *a, ExenVM *vm)
{
    if (!a->audio || !exen_audio_active(vm))
        return -1;
    int queued = SDL_GetAudioStreamQueued(a->audio) / 4;
    int target = AUDIO_RATE * AUDIO_TARGET_MS / 1000;
    int16_t buf[2 * 1024];
    while (queued < target) {
        int n = target - queued > 1024 ? 1024 : target - queued;
        exen_audio_render(vm, buf, n);
        SDL_PutAudioStreamData(a->audio, buf, n * 4);
        queued += n;
    }
    int ms = (queued - AUDIO_RATE * AUDIO_LOW_MS / 1000) * 1000 / AUDIO_RATE;
    return ms > 1 ? ms : 1;
}

static void set_title(App *a, const char *name, ExenVM *vm)
{
    char t[512];
    const char *m = a->muted ? " [muted]" : "";
    const char *st = persist_title(); /* save state slot status (persist.c) */
    if (exen_halted(vm))
        SDL_snprintf(t, sizeof t, "%s - stopped: %s%s%s", name, exen_error(vm), m, st);
    else
        SDL_snprintf(t, sizeof t, "%s - exen-reloaded%s%s", name, m, st);
    SDL_SetWindowTitle(a->win, t);
}

static void build_lut(App *a)
{
    for (int p = 0; p < 256; p++) {
        /* EXManufDisplayUpdate (0x4342f0): channel * 36.4286 (3-bit) or * 85.0 (2-bit), truncated */
        uint32_t r = (uint32_t)((p >> 5) * 36.4286), g = (uint32_t)(((p >> 2) & 7) * 36.4286),
                 b = (uint32_t)((p & 3) * 85.0);
        a->lut[p] = 0xff000000u | r << 16 | g << 8 | b;
    }
}

static void on_present(void *u, const uint8_t *px, int w, int h)
{
    App *a = u;
    if (w != a->w || h != a->h)
        return;
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
    return (uint32_t)((SDL_GetTicksNS() - a->start_ns) / 1000000u);
}

static void on_log(void *u, const char *m)
{
    (void)u;
    SDL_Log("%s", m);
}

/* Phone key transitions from the controls (input.c). */
static void on_phone_key(void *u, int code, int pressed)
{
    ExenVM *vm = u;
    if (!exen_halted(vm))
        exen_key(vm, code, pressed);
}

/* --keys-config, else keys.ini in the data directory. */
static const char *controls_path(const char *given)
{
    static char buf[1024];
    if (given)
        return given;
    SDL_snprintf(buf, sizeof buf, "%skeys.ini", exen_data_dir());
    return buf;
}

static void *slurp(const char *path, size_t *len)
{
    return SDL_LoadFile(path, len);
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

static void usage(void)
{
    fprintf(stderr,
            "usage: exen-reloaded [options] game.exn\n"
            "  --sim PATH      original simulator (gensimu.exe); searched beside the app by default\n"
            "  --scale N       window scale (default 4)\n"
            "  --size WxH      screen size of the target handset (default: from the game\n"
            "                  database, else 132x176)\n"
            "  --fullscreen    start fullscreen (F11 toggles)\n"
            "  --mute          start with the sound off (M toggles)\n"
            "  --volume N      sound volume 0-100 (default 100)\n"
            "  --keys-config PATH\n"
            "                  controls file (default: keys.ini beside the app, or in the user\n"
            "                  data directory if that is not writable; written with the defaults)\n"
            "  --print-bindings\n"
            "                  print the effective controls and exit\n"
            "default controls (remap them in the controls file; see README.md):\n"
            "  keyboard: arrows or W/A/S/D = 2/4/6/8, Q/E/Z/C = 1/3/7/9, Space = 5, X = 0,\n"
            "            [ = *, ] = #, digits and numpad = phone keypad (numpad 7 = 1),\n"
            "            Enter = OK/select, Esc/Backspace = clear/back\n"
            "  gamepad:  d-pad/left stick = 2/4/6/8, A = 5, B/Back = clear, Start = OK,\n"
            "            X = *, Y = #, LB/RB = 1/3, LT/RT = 7/9, stick clicks = 0\n"
            "  hotkeys:  M = mute, F11 = fullscreen,\n"
            "            F5 = save state, F8 = load state, F6/F7 = previous/next slot (0-9)\n");
}

int main(int argc, char **argv)
{
    const char *sim = NULL, *game = NULL;
    const char *keys_config = NULL;
    int scale = 4, w = 0, h = 0, fullscreen = 0, mute = 0, volume = 100, print_bindings = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim") && i + 1 < argc) sim = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "--volume") && i + 1 < argc) volume = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keys-config") && i + 1 < argc) keys_config = argv[++i];
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
    if (!game || w < 0 || h < 0 || scale <= 0 || volume < 0 || volume > 100) {
        usage();
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    sim = find_sim(sim);
    size_t sl = 0, gl = 0;
    void *simb = sim ? slurp(sim, &sl) : NULL, *gameb = slurp(game, &gl);
    if (!simb || !gameb) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "exen-reloaded",
                                 !simb ? "Cannot find gensimu.exe (use --sim)." : "Cannot read the game file.", NULL);
        return 1;
    }
    ExenGameInfo gi;
    exen_game_info(gameb, gl, &gi);
    if (w <= 0 || h <= 0) {
        w = gi.width;
        h = gi.height;
    }

    App a = {0};
    a.w = w;
    a.h = h;
    a.period_ms = -1;
    a.volume = volume;
    a.muted = mute;
    a.rgb = calloc((size_t)w * (size_t)h, sizeof *a.rgb);
    build_lut(&a);
    for (int i = 0; i < w * h; i++)
        a.rgb[i] = 0xff000000u;

    char title[256];
    const char *slash = strrchr(game, '/'), *bslash = strrchr(game, '\\');
    const char *name = slash > bslash ? slash + 1 : bslash ? bslash + 1 : game;
    SDL_snprintf(title, sizeof title, "%s - exen-reloaded", name);
    if (!SDL_CreateWindowAndRenderer(title, w * scale, h * scale, SDL_WINDOW_RESIZABLE |
                                     (fullscreen ? SDL_WINDOW_FULLSCREEN : 0), &a.win, &a.ren)) {
        fprintf(stderr, "window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(a.ren, 1);
    SDL_SetRenderLogicalPresentation(a.ren, w, h, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    a.tex = SDL_CreateTexture(a.ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    SDL_SetTextureScaleMode(a.tex, SDL_SCALEMODE_NEAREST);

    /* Audio is optional: without a device the game runs silently. */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, AUDIO_RATE};
        a.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (a.audio)
            SDL_ResumeAudioStreamDevice(a.audio);
    }
    if (!a.audio)
        SDL_Log("audio disabled: %s", SDL_GetError());

    ExenConfig cfg = {0};
    cfg.screen_width = w;
    cfg.screen_height = h;
    cfg.audio_rate = AUDIO_RATE;
    cfg.host = (ExenHost){
        .user = &a,
        .present = on_present,
        .set_timer = on_timer,
        .ticks_ms = on_ticks,
        .log = on_log,
        .eeprom_changed = persist_eeprom_changed,
    };
    a.start_ns = SDL_GetTicksNS();
    ExenVM *vm = exen_create(&cfg);
    if (!vm || exen_load_framework(vm, simb, sl) < 0 || exen_load_game(vm, gameb, gl) < 0 ||
        (persist_open(vm, sim, gameb, gl), exen_boot(vm) < 0)) {
        char msg[512];
        SDL_snprintf(msg, sizeof msg, "Failed to start %s:\n%s", name, vm ? exen_error(vm) : "out of memory");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "exen-reloaded", msg, a.win);
        return 1;
    }
    exen_audio_set_volume(vm, a.muted ? 0 : a.volume);
    /* controls (input.c); gamepads arrive as SDL_EVENT_GAMEPAD_ADDED */
    Input controls;
    input_init(&controls, on_phone_key, vm);
    input_load_or_create(&controls, controls_path(keys_config), 1);
    input_open_gamepads(&controls);
    if (a.muted)
        set_title(&a, name, vm);

    int running = 1, halted_shown = 0, hk, audio_wait = audio_pump(&a, vm);
    a.dirty = 1;
    while (running) {
        /* sleep until the next tick or an event */
        int timeout = -1;
        if (a.period_ms > 0 && !exen_halted(vm)) {
            uint64_t now = SDL_GetTicksNS();
            timeout = a.next_tick_ns > now ? (int)((a.next_tick_ns - now + 999999u) / 1000000u) : 0;
        }
        if (audio_wait >= 0 && (timeout < 0 || audio_wait < timeout))
            timeout = audio_wait;
        int status_wait = persist_title_wait_ms(); /* the slot status in the title expires */
        if (status_wait == 0)
            set_title(&a, name, vm);
        else if (status_wait > 0 && (timeout < 0 || status_wait < timeout))
            timeout = status_wait;
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, timeout)) {
            do {
                switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = 0;
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:
                    if (ev.key.key == SDLK_F11 && ev.type == SDL_EVENT_KEY_DOWN) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(a.win, fullscreen);
                    } else if (ev.key.key == SDLK_M) {
                        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                            a.muted = !a.muted;
                            exen_audio_set_volume(vm, a.muted ? 0 : a.volume);
                            set_title(&a, name, vm);
                        }
                    } else if ((hk = persist_hotkey(&ev)) != PERSIST_NONE) { /* F5-F8 */
                        if (hk == PERSIST_LOADING) {
                            input_release_all(&controls); /* into the old state */
                            if (persist_load_slot()) {
                                if (a.audio)
                                    SDL_ClearAudioStream(a.audio);
                                halted_shown = 0;
                            }
                        }
                        set_title(&a, name, vm);
                    } else {
                        input_handle_event(&controls, &ev);
                    }
                    break;
                case SDL_EVENT_WINDOW_EXPOSED:
                    a.dirty = 1;
                    break;
                default: /* gamepads, focus loss */
                    input_handle_event(&controls, &ev);
                    break;
                }
            } while (SDL_PollEvent(&ev));
        }
        if (a.period_ms > 0 && !exen_halted(vm)) {
            uint64_t now = SDL_GetTicksNS();
            if (now >= a.next_tick_ns) {
                exen_tick(vm);
                uint64_t period = (uint64_t)a.period_ms * 1000000u;
                a.next_tick_ns += period;
                if (a.next_tick_ns <= now) /* late: drop missed ticks */
                    a.next_tick_ns = now + period;
            }
        }
        audio_wait = audio_pump(&a, vm);
        if (exen_halted(vm) && !halted_shown) {
            halted_shown = 1;
            set_title(&a, name, vm);
        }
        if (a.dirty) {
            a.dirty = 0;
            SDL_UpdateTexture(a.tex, NULL, a.rgb, w * 4);
            SDL_SetRenderDrawColor(a.ren, 0, 0, 0, 255);
            SDL_RenderClear(a.ren);
            SDL_RenderTexture(a.ren, a.tex, NULL, NULL);
            SDL_RenderPresent(a.ren);
        }
    }
    input_close_gamepads(&controls);
    if (a.audio)
        SDL_DestroyAudioStream(a.audio);
    persist_close();
    exen_destroy(vm);
    SDL_free(simb);
    SDL_free(gameb);
    free(a.rgb);
    SDL_Quit();
    return 0;
}
