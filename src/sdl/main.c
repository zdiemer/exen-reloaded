/* exen-reloaded SDL3 frontend.
 *
 *   exen-reloaded [--sim gensimu.exe] [--scale N] [--size WxH] [--fullscreen] game.exn
 *
 * Pacing is event driven: the thread sleeps in SDL_WaitEventTimeout until the next game tick
 * or input arrives. The original simulator busy-waits in its menus and repaints through slow
 * per-pixel float conversion; nothing here spins.
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
} App;

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

/* PC key -> phone key code, following the original simulator (docs/re/NATIVES.md §4.2). */
static int map_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return EXEN_KEY_OK;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return EXEN_KEY_CLEAR;
    case SDLK_UP: return '2';
    case SDLK_LEFT: return '4';
    case SDLK_RIGHT: return '6';
    case SDLK_DOWN: return '8';
    case SDLK_KP_0: return '0';
    case SDLK_KP_7: return '1';
    case SDLK_KP_8: return '2';
    case SDLK_KP_9: return '3';
    case SDLK_KP_4: return '4';
    case SDLK_KP_5: return '5';
    case SDLK_KP_6: return '6';
    case SDLK_KP_1: return '7';
    case SDLK_KP_2: return '8';
    case SDLK_KP_3: return '9';
    case SDLK_KP_MULTIPLY: return '*';
    case SDLK_KP_DIVIDE: return '#';
    default:
        if (k >= SDLK_0 && k <= SDLK_9)
            return (int)('0' + (k - SDLK_0));
        return 0;
    }
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
            "  --size WxH      screen size of the target handset (default 101x80)\n"
            "  --fullscreen    start fullscreen (F11 toggles)\n"
            "keys: arrows = 2/4/6/8, numpad = phone keypad, 0-9, Enter = select, Esc/Backspace = clear\n");
}

int main(int argc, char **argv)
{
    const char *sim = NULL, *game = NULL;
    int scale = 4, w = 101, h = 80, fullscreen = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sim") && i + 1 < argc) sim = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (argv[i][0] == '-') { usage(); return 2; }
        else game = argv[i];
    }
    if (!game || w <= 0 || h <= 0 || scale <= 0) {
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

    App a = {0};
    a.w = w;
    a.h = h;
    a.period_ms = -1;
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

    ExenConfig cfg = {0};
    cfg.screen_width = w;
    cfg.screen_height = h;
    cfg.host = (ExenHost){&a, on_present, on_timer, on_ticks, NULL, NULL, on_log};
    a.start_ns = SDL_GetTicksNS();
    ExenVM *vm = exen_create(&cfg);
    if (!vm || exen_load_framework(vm, simb, sl) < 0 || exen_load_game(vm, gameb, gl) < 0 ||
        exen_boot(vm) < 0) {
        char msg[512];
        SDL_snprintf(msg, sizeof msg, "Failed to start %s:\n%s", name, vm ? exen_error(vm) : "out of memory");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "exen-reloaded", msg, a.win);
        return 1;
    }

    int running = 1, halted_shown = 0;
    a.dirty = 1;
    while (running) {
        /* sleep until the next tick or an event */
        int timeout = -1;
        if (a.period_ms > 0 && !exen_halted(vm)) {
            uint64_t now = SDL_GetTicksNS();
            timeout = a.next_tick_ns > now ? (int)((a.next_tick_ns - now + 999999u) / 1000000u) : 0;
        }
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
                    } else if (!ev.key.repeat) { /* the original drops auto-repeat */
                        int code = map_key(ev.key.key);
                        if (code && !exen_halted(vm))
                            exen_key(vm, code, ev.type == SDL_EVENT_KEY_DOWN);
                    }
                    break;
                case SDL_EVENT_WINDOW_EXPOSED:
                    a.dirty = 1;
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
        if (exen_halted(vm) && !halted_shown) {
            halted_shown = 1;
            char t[512];
            SDL_snprintf(t, sizeof t, "%s - stopped: %s", name, exen_error(vm));
            SDL_SetWindowTitle(a.win, t);
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
    exen_destroy(vm);
    SDL_free(simb);
    SDL_free(gameb);
    free(a.rgb);
    SDL_Quit();
    return 0;
}
