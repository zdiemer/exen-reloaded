/* exen-reloaded libretro core (docs/LIBRETRO.md).
 *
 * Content is a .exn game, loaded from memory. The ExEn framework classes come from the
 * original simulator, gensimu.exe, which the user puts in the frontend's system directory.
 *
 * Timing
 * ------
 * The core runs at a fixed 60 frames per second on a virtual clock; nothing here reads the
 * host clock, sleeps or spawns threads, so the same inputs always give the same frames and
 * audio (rewind, runahead and netplay depend on that). The clock counts units of 1/60000 s,
 * so that a frame (1000 units) and a millisecond (60 units) are both whole numbers. Each
 * retro_run delivers the frame's key changes at the frame's start, then every game tick whose
 * deadline falls within the frame, with the clock set to that deadline, at most
 * MAX_TICKS_PER_FRAME of them; a game further behind drops the missed ticks, as the original
 * and the SDL frontend do. ExenHost.ticks_ms reads this clock.
 *
 * Audio is rendered in step with the clock: up to each tick's deadline before the tick runs
 * (so a melody it starts begins on the right sample), and to the end of the frame after the
 * last one, 735 stereo frames at 44100 Hz per retro_run, pushed with one audio_batch call.
 *
 * Booting is deferred to the first retro_run (or retro_serialize), because frontends load the
 * save RAM into retro_get_memory_data's buffer after retro_load_game returns, and the game
 * reads its saved context while it boots.
 *
 * Save RAM
 * --------
 * RETRO_MEMORY_SAVE_RAM is the 2048-byte ExEn EEPROM. The core keeps it in a buffer of its own
 * (so the pointer stays valid across retro_reset, which recreates the VM) and mirrors it: the
 * buffer is copied into the VM before boot, and back from the VM whenever the core reports a
 * change (ExenHost.eeprom_changed) and at the end of every retro_run. A new game starts from
 * the simulator's flash/eeprom.dat (found beside gensimu.exe) with an empty context, like the
 * SDL frontend (exen_eeprom_reset).
 *
 * Save states
 * -----------
 * retro_serialize_size is fixed for the session: exen_state_max_size (the worst case for the
 * loaded game) plus a trailer. The buffer holds the core's state, zeros, and the trailer in
 * its last TRAILER_BYTES: the virtual clock, the next tick deadline, the timer period, and the
 * phone keys held, so that key edges after a load are computed against the keys held when the
 * state was taken.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exen.h"
#include "libretro.h"
#include "rgb332.h"

#ifndef EXEN_LIBRETRO_VERSION
#define EXEN_LIBRETRO_VERSION "0.1.0-dev"
#endif

#define FPS 60
#define AUDIO_RATE 44100
#define FRAME_SAMPLES (AUDIO_RATE / FPS) /* 735 */
#define UNITS_PER_MS FPS                  /* clock unit: 1/60000 s */
#define UNITS_PER_FRAME 1000u
#define MAX_TICKS_PER_FRAME 8
#define MAX_WIDTH 176                     /* geometry limit for screen size changes */
#define MAX_HEIGHT 220

#define TRAILER_BYTES 64
#define TRAILER_MAGIC "EXLR"
#define TRAILER_VERSION 1u

/* Analog stick thresholds (fraction of full travel), as the SDL frontend's defaults. */
#define STICK_PRESS (32767 * 50 / 100)
#define STICK_RELEASE (32767 * 35 / 100)

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

/* Phone keys, by bit index in the held masks. */
static const int phone_keys[] = {
    EXEN_KEY_1, EXEN_KEY_2, EXEN_KEY_3, EXEN_KEY_4, EXEN_KEY_5,    EXEN_KEY_6,  EXEN_KEY_7,
    EXEN_KEY_8, EXEN_KEY_9, EXEN_KEY_STAR, EXEN_KEY_0, EXEN_KEY_POUND, EXEN_KEY_OK, EXEN_KEY_CLEAR,
};
#define NPHONE ((int)(sizeof phone_keys / sizeof phone_keys[0]))
enum { K1, K2, K3, K4, K5, K6, K7, K8, K9, KSTAR, K0, KPOUND, KOK, KCLEAR };

/* RetroPad bindings. Face buttons by position, as in the SDL frontend: bottom (RetroPad B) is
 * 5, right (A) is clear, left (Y) is *, top (X) is #. */
static const struct {
    unsigned id;
    int key;
    const char *desc;
} pad_binds[] = {
    {RETRO_DEVICE_ID_JOYPAD_UP, K2, "2 (up)"},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, K8, "8 (down)"},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, K4, "4 (left)"},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, K6, "6 (right)"},
    {RETRO_DEVICE_ID_JOYPAD_B, K5, "5 (fire / select)"},
    {RETRO_DEVICE_ID_JOYPAD_A, KCLEAR, "Clear / back"},
    {RETRO_DEVICE_ID_JOYPAD_Y, KSTAR, "*"},
    {RETRO_DEVICE_ID_JOYPAD_X, KPOUND, "#"},
    {RETRO_DEVICE_ID_JOYPAD_START, KOK, "OK / select (soft key)"},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, KCLEAR, "Clear / back"},
    {RETRO_DEVICE_ID_JOYPAD_L, K1, "1"},
    {RETRO_DEVICE_ID_JOYPAD_R, K3, "3"},
    {RETRO_DEVICE_ID_JOYPAD_L2, K7, "7"},
    {RETRO_DEVICE_ID_JOYPAD_R2, K9, "9"},
    {RETRO_DEVICE_ID_JOYPAD_L3, K0, "0"},
    {RETRO_DEVICE_ID_JOYPAD_R3, K0, "0"},
};
#define NPAD ((int)(sizeof pad_binds / sizeof pad_binds[0]))

/* Keyboard (RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK): digits, the numpad in the phone's layout
 * (numpad 7 is phone 1), [ and ] for * and #, Enter for OK, Backspace for clear. Letters and
 * arrows are left to the frontend's RetroPad mapping. */
static const struct {
    unsigned code;
    int key;
} kb_binds[] = {
    {RETROK_1, K1}, {RETROK_2, K2}, {RETROK_3, K3}, {RETROK_4, K4}, {RETROK_5, K5},
    {RETROK_6, K6}, {RETROK_7, K7}, {RETROK_8, K8}, {RETROK_9, K9}, {RETROK_0, K0},
    {RETROK_KP7, K1}, {RETROK_KP8, K2}, {RETROK_KP9, K3}, {RETROK_KP4, K4}, {RETROK_KP5, K5},
    {RETROK_KP6, K6}, {RETROK_KP1, K7}, {RETROK_KP2, K8}, {RETROK_KP3, K9}, {RETROK_KP0, K0},
    {RETROK_KP_MULTIPLY, KSTAR}, {RETROK_KP_DIVIDE, KPOUND},
    {RETROK_LEFTBRACKET, KSTAR}, {RETROK_RIGHTBRACKET, KPOUND},
    {RETROK_RETURN, KOK}, {RETROK_KP_ENTER, KOK}, {RETROK_BACKSPACE, KCLEAR},
};
#define NKB ((int)(sizeof kb_binds / sizeof kb_binds[0]))

static struct {
    ExenVM *vm;
    uint8_t *sim, *game, *eep_defaults;
    size_t sim_len, game_len, eep_len;
    ExenGameInfo gi;
    int w, h;              /* screen of the running VM */
    int geom_w, geom_h;    /* max geometry reported to the frontend */
    void *fb;              /* w*h pixels in the negotiated format */
    int xrgb;              /* 1: XRGB8888, 0: RGB565 */
    uint32_t lut32[256];
    uint16_t lut16[256];
    int dirty, can_dupe, bitmasks;
    int booted, halt_reported;
    uint64_t clock, next_tick; /* clock units (1/60000 s) */
    int period_ms;             /* < 0 or 0: timer stopped */
    int apos;                  /* audio frames rendered in the current retro_run */
    int16_t audio[2 * FRAME_SAMPLES];
    int audio_on;
    uint32_t held;             /* phone keys the VM has been told are down (bit per NPHONE) */
    uint32_t stick;            /* analog stick directions held: bits 0..3 = up, down, left, right */
    uint8_t kb_down[RETROK_LAST];
    uint8_t sram[EXEN_EEPROM_SIZE];
    size_t state_size;
} g;

/* ------------------------------------------------------------ logging and messages */

static void log_printf(enum retro_log_level level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_cb)
        log_cb(level, "[exen-reloaded] %s\n", buf);
    else if (level >= RETRO_LOG_WARN)
        fprintf(stderr, "[exen-reloaded] %s\n", buf);
}

/* An on-screen message, and the same text in the log. */
static void show_message(enum retro_log_level level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_printf(level, "%s", buf);
    if (!env_cb)
        return;
    unsigned version = 0;
    if (env_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &version) && version >= 1) {
        struct retro_message_ext m = {0};
        m.msg = buf;
        m.duration = 5000;
        m.priority = 1;
        m.level = level;
        m.target = RETRO_MESSAGE_TARGET_ALL;
        m.type = RETRO_MESSAGE_TYPE_NOTIFICATION;
        m.progress = -1;
        env_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &m);
    } else {
        struct retro_message m = {buf, 5 * FPS};
        env_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
    }
}

/* ------------------------------------------------------------ core options */

#define OPT_SIZE "exen_screen_size"
#define OPT_AUDIO "exen_audio"

static const char size_info[] =
    "Handset screen the game runs on. Auto takes it from the built-in game database (132x176 "
    "for games it does not know). A game drawn for another handset may be cut off or leave "
    "borders. Takes effect when the game is restarted.";
static const char audio_info[] =
    "Play the game's melodies. When off, the melodies keep running silently, so they stay in "
    "time when sound is turned back on.";

#define SIZE_VALUES                                                                          \
    {{"auto", "Auto (game database)"}, {"101x80", NULL}, {"112x112", NULL}, {"120x143", NULL}, \
     {"128x128", NULL}, {"128x141", NULL}, {"128x160", NULL}, {"132x176", NULL}, {NULL, NULL}}
#define AUDIO_VALUES {{"enabled", NULL}, {"disabled", NULL}, {NULL, NULL}}

static struct retro_core_option_v2_definition options_v2[] = {
    {OPT_SIZE, "Screen size", NULL, size_info, NULL, NULL, SIZE_VALUES, "auto"},
    {OPT_AUDIO, "Sound", NULL, audio_info, NULL, NULL, AUDIO_VALUES, "enabled"},
    {NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};
static struct retro_core_options_v2 options_v2_set = {NULL, options_v2};

static struct retro_core_option_definition options_v1[] = {
    {OPT_SIZE, "Screen size", size_info, SIZE_VALUES, "auto"},
    {OPT_AUDIO, "Sound", audio_info, AUDIO_VALUES, "enabled"},
    {NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};

static void set_options(void)
{
    unsigned version = 0;
    if (!env_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
        version = 0;
    if (version >= 2 && env_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2_set))
        return;
    if (version >= 1 && env_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, options_v1))
        return;
    static const struct retro_variable vars[] = {
        {OPT_SIZE, "Screen size; auto|101x80|112x112|120x143|128x128|128x141|128x160|132x176"},
        {OPT_AUDIO, "Sound; enabled|disabled"},
        {NULL, NULL},
    };
    env_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
}

static const char *get_option(const char *key)
{
    struct retro_variable v = {key, NULL};
    if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
        return v.value;
    return NULL;
}

/* The screen size the options ask for (auto: the game's handset). */
static void wanted_size(int *w, int *h)
{
    *w = g.gi.width;
    *h = g.gi.height;
    const char *v = get_option(OPT_SIZE);
    int ow, oh;
    if (v && strcmp(v, "auto") && sscanf(v, "%dx%d", &ow, &oh) == 2 && ow > 0 && oh > 0 &&
        ow <= 1024 && oh <= 1024) {
        *w = ow;
        *h = oh;
    }
}

static void apply_audio_option(void)
{
    const char *v = get_option(OPT_AUDIO);
    g.audio_on = !v || strcmp(v, "disabled") != 0;
    if (g.vm)
        exen_audio_set_volume(g.vm, g.audio_on ? 100 : 0);
}

static void check_options(void)
{
    bool updated = false;
    if (!env_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) || !updated)
        return;
    apply_audio_option();
    int w, h;
    wanted_size(&w, &h);
    if (w != g.w || h != g.h)
        show_message(RETRO_LOG_INFO, "Screen size %dx%d takes effect when the game is restarted", w, h);
}

/* ------------------------------------------------------------ host callbacks */

static void on_present(void *user, const uint8_t *px, int w, int h)
{
    (void)user;
    if (w != g.w || h != g.h || !g.fb)
        return;
    size_t n = (size_t)w * (size_t)h;
    if (g.xrgb) {
        uint32_t *o = g.fb;
        for (size_t i = 0; i < n; i++)
            o[i] = g.lut32[px[i]];
    } else {
        uint16_t *o = g.fb;
        for (size_t i = 0; i < n; i++)
            o[i] = g.lut16[px[i]];
    }
    g.dirty = 1;
}

static void on_timer(void *user, int period_ms)
{
    (void)user;
    g.period_ms = period_ms;
    if (period_ms > 0)
        g.next_tick = g.clock + (uint64_t)period_ms * UNITS_PER_MS;
}

static uint32_t on_ticks(void *user)
{
    (void)user;
    return (uint32_t)(g.clock / UNITS_PER_MS);
}

static void on_log(void *user, const char *msg)
{
    (void)user;
    log_printf(RETRO_LOG_INFO, "%s", msg);
}

static void on_eeprom(void *user)
{
    (void)user;
    if (g.vm)
        memcpy(g.sram, exen_eeprom(g.vm), EXEN_EEPROM_SIZE);
}

/* ------------------------------------------------------------ files */

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *b = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n > 0 && fseek(f, 0, SEEK_SET) == 0 && (b = malloc((size_t)n)) != NULL) {
            if (fread(b, 1, (size_t)n, f) == (size_t)n) {
                *len = (size_t)n;
            } else {
                free(b);
                b = NULL;
            }
        }
    }
    fclose(f);
    return b;
}

/* Find gensimu.exe in the system directory: <system>/gensimu.exe or <system>/exen/gensimu.exe.
 * On success *dir is the directory it was found in, with a trailing separator. */
static uint8_t *find_sim(const char *sys, size_t *len, char *dir, size_t dirsz, char *tried,
                         size_t triedsz)
{
    static const char *const subdirs[] = {"", "exen/"};
    size_t sl = strlen(sys);
    const char *sep = sl && (sys[sl - 1] == '/' || sys[sl - 1] == '\\') ? "" : "/";
    tried[0] = 0;
    for (size_t i = 0; i < sizeof subdirs / sizeof subdirs[0]; i++) {
        char path[1200];
        snprintf(dir, dirsz, "%s%s%s", sys, sep, subdirs[i]);
        snprintf(path, sizeof path, "%sgensimu.exe", dir);
        uint8_t *b = read_file(path, len);
        if (b)
            return b;
        size_t t = strlen(tried);
        snprintf(tried + t, triedsz - t, "%s%s", t ? " or " : "", path);
    }
    dir[0] = 0;
    return NULL;
}

/* The simulator's EEPROM defaults, beside gensimu.exe. */
static uint8_t *find_eeprom_defaults(const char *dir, size_t *len)
{
    static const char *const names[] = {"flash/eeprom.dat", "flash/EEPROM.DAT", "eeprom.dat",
                                        "EEPROM.DAT"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s%s", dir, names[i]);
        uint8_t *b = read_file(path, len);
        if (b) {
            log_printf(RETRO_LOG_INFO, "EEPROM defaults from %s", path);
            return b;
        }
    }
    log_printf(RETRO_LOG_INFO, "no flash/eeprom.dat beside gensimu.exe: the EEPROM starts out blank");
    return NULL;
}

/* ------------------------------------------------------------ VM lifecycle */

static void destroy_vm(void)
{
    exen_destroy(g.vm);
    g.vm = NULL;
}

/* Create the VM for the current game at the wanted size. `fresh`: initialise the save RAM from
 * the EEPROM defaults (a new load); otherwise the save RAM is kept (reset). */
static int create_vm(int fresh)
{
    int w, h;
    wanted_size(&w, &h);
    if (w != g.w || h != g.h || !g.fb) {
        void *fb = calloc((size_t)w * (size_t)h, 4);
        if (!fb)
            return -1;
        free(g.fb);
        g.fb = fb;
        g.w = w;
        g.h = h;
    } else {
        memset(g.fb, 0, (size_t)w * (size_t)h * 4);
    }
    /* black until the game presents its first frame */
    if (g.xrgb)
        for (int i = 0; i < w * h; i++)
            ((uint32_t *)g.fb)[i] = 0xff000000u;

    g.clock = 0;
    g.next_tick = 0;
    g.period_ms = -1;
    g.booted = 0;
    g.halt_reported = 0;
    g.held = 0;
    g.stick = 0;
    g.dirty = 1;

    ExenConfig cfg = {0};
    cfg.screen_width = w;
    cfg.screen_height = h;
    cfg.audio_rate = AUDIO_RATE;
    cfg.host = (ExenHost){
        .user = NULL,
        .present = on_present,
        .set_timer = on_timer,
        .ticks_ms = on_ticks,
        .log = on_log,
        .eeprom_changed = on_eeprom,
    };
    ExenVM *vm = exen_create(&cfg);
    if (!vm) {
        show_message(RETRO_LOG_ERROR, "out of memory");
        return -1;
    }
    if (exen_load_framework(vm, g.sim, g.sim_len) < 0) {
        show_message(RETRO_LOG_ERROR, "gensimu.exe: %s", exen_error(vm));
        exen_destroy(vm);
        return -1;
    }
    if (exen_load_game(vm, g.game, g.game_len) < 0) {
        show_message(RETRO_LOG_ERROR, "Cannot load the game: %s", exen_error(vm));
        exen_destroy(vm);
        return -1;
    }
    g.vm = vm;
    if (fresh)
        exen_eeprom_reset(vm, g.eep_defaults, g.eep_len); /* -> on_eeprom fills g.sram */
    exen_audio_set_volume(vm, g.audio_on ? 100 : 0);
    g.state_size = exen_state_max_size(vm) + TRAILER_BYTES;
    return 0;
}

/* Boot on first use, with the save RAM the frontend has loaded by now. */
static void ensure_booted(void)
{
    if (!g.vm || g.booted)
        return;
    g.booted = 1;
    memcpy(exen_eeprom(g.vm), g.sram, EXEN_EEPROM_SIZE);
    if (exen_boot(g.vm) < 0 && !exen_halted(g.vm))
        log_printf(RETRO_LOG_ERROR, "boot failed: %s", exen_error(g.vm));
}

static void report_halt(void)
{
    if (g.vm && exen_halted(g.vm) && !g.halt_reported) {
        g.halt_reported = 1;
        show_message(RETRO_LOG_WARN, "The game stopped: %s", exen_error(g.vm));
    }
}

/* ------------------------------------------------------------ input */

static uint32_t poll_phone_keys(void)
{
    uint32_t m = 0;
    unsigned bits = 0;
    if (g.bitmasks) {
        bits = (uint16_t)input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
    } else {
        for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; id++)
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id))
                bits |= 1u << id;
    }
    for (int i = 0; i < NPAD; i++)
        if (bits & (1u << pad_binds[i].id))
            m |= 1u << pad_binds[i].key;

    /* left stick: each direction presses past STICK_PRESS and releases below STICK_RELEASE */
    int x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    int y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    const int travel[4] = {-y, y, -x, x};
    static const int dir_key[4] = {K2, K8, K4, K6};
    for (int d = 0; d < 4; d++) {
        int on = g.stick >> d & 1;
        if (!on && travel[d] >= STICK_PRESS)
            g.stick |= 1u << d;
        else if (on && travel[d] < STICK_RELEASE)
            g.stick &= ~(1u << d);
        if (g.stick >> d & 1)
            m |= 1u << dir_key[d];
    }

    for (int i = 0; i < NKB; i++)
        if (g.kb_down[kb_binds[i].code])
            m |= 1u << kb_binds[i].key;
    return m;
}

/* Tell the VM about the phone keys that changed: releases first, then presses, in keypad
 * order. A phone key bound to several inputs is pressed by the first and released by the
 * last. */
static void deliver_keys(uint32_t now)
{
    uint32_t changed = now ^ g.held;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < NPHONE; i++) {
            uint32_t bit = 1u << i;
            if (!(changed & bit) || !!(now & bit) != pass)
                continue;
            if (!exen_halted(g.vm))
                exen_key(g.vm, phone_keys[i], pass);
        }
    g.held = now;
}

static void RETRO_CALLCONV on_keyboard(bool down, unsigned keycode, uint32_t character,
                                       uint16_t mods)
{
    (void)character;
    (void)mods;
    if (keycode < RETROK_LAST)
        g.kb_down[keycode] = down ? 1 : 0;
}

static void set_input_descriptors(void)
{
    static struct retro_input_descriptor desc[NPAD + 5];
    int n = 0;
    for (int i = 0; i < NPAD; i++)
        desc[n++] = (struct retro_input_descriptor){0, RETRO_DEVICE_JOYPAD, 0, pad_binds[i].id,
                                                    pad_binds[i].desc};
    desc[n++] = (struct retro_input_descriptor){0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                                RETRO_DEVICE_ID_ANALOG_X, "4 / 6 (left / right)"};
    desc[n++] = (struct retro_input_descriptor){0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                                RETRO_DEVICE_ID_ANALOG_Y, "2 / 8 (up / down)"};
    desc[n] = (struct retro_input_descriptor){0};
    env_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

/* ------------------------------------------------------------ audio */

/* Render the current frame's audio up to frame `pos` (0..FRAME_SAMPLES). */
static void audio_to(int pos)
{
    if (pos > FRAME_SAMPLES)
        pos = FRAME_SAMPLES;
    if (pos <= g.apos)
        return;
    int n = pos - g.apos;
    int16_t *out = g.audio + 2 * g.apos;
    /* exen.h: while nothing plays the host may skip rendering; it resumes where the game
     * starts the next melody */
    if (g.vm && exen_audio_active(g.vm))
        exen_audio_render(g.vm, out, n);
    else
        memset(out, 0, (size_t)n * 4);
    g.apos = pos;
}

/* ------------------------------------------------------------ libretro API */

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    env_cb = cb;
    struct retro_log_callback lc;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &lc))
        log_cb = lc.log;
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    set_options();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init(void)
{
    exen_rgb332_lut(g.lut32);
    for (int i = 0; i < 256; i++) {
        uint32_t c = g.lut32[i];
        g.lut16[i] = (uint16_t)(((c >> 19) & 0x1f) << 11 | ((c >> 10) & 0x3f) << 5 | ((c >> 3) & 0x1f));
    }
}

RETRO_API void retro_deinit(void) {}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof *info);
    info->library_name = "exen-reloaded";
    info->library_version = EXEN_LIBRETRO_VERSION;
    info->valid_extensions = "exn";
    info->need_fullpath = false;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof *info);
    int w = g.w > 0 ? g.w : EXEN_DEFAULT_WIDTH, h = g.h > 0 ? g.h : EXEN_DEFAULT_HEIGHT;
    g.geom_w = w > MAX_WIDTH ? w : MAX_WIDTH;
    g.geom_h = h > MAX_HEIGHT ? h : MAX_HEIGHT;
    info->geometry.base_width = (unsigned)w;
    info->geometry.base_height = (unsigned)h;
    info->geometry.max_width = (unsigned)g.geom_w;
    info->geometry.max_height = (unsigned)g.geom_h;
    info->geometry.aspect_ratio = (float)w / (float)h;
    info->timing.fps = FPS;
    info->timing.sample_rate = AUDIO_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port;
    (void)device;
}

static void free_content(void)
{
    destroy_vm();
    free(g.sim);
    free(g.game);
    free(g.eep_defaults);
    free(g.fb);
    g.sim = g.game = g.eep_defaults = NULL;
    g.fb = NULL;
    g.w = g.h = 0;
    g.state_size = 0;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    if (!game || !game->data || game->size == 0) {
        show_message(RETRO_LOG_ERROR, "No game data (exen-reloaded loads .exn files)");
        return false;
    }
    free_content();
    memset(g.kb_down, 0, sizeof g.kb_down);
    memset(g.sram, 0, sizeof g.sram);

    const char *sys = NULL;
    if (!env_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys) || !sys || !*sys) {
        show_message(RETRO_LOG_ERROR, "The frontend gave no system directory to find gensimu.exe in");
        return false;
    }
    char dir[1100], tried[2600];
    g.sim = find_sim(sys, &g.sim_len, dir, sizeof dir, tried, sizeof tried);
    if (!g.sim) {
        show_message(RETRO_LOG_ERROR,
                     "gensimu.exe not found: copy it from exen-reloaded's vendor/gensimu/ into "
                     "the system directory (looked for %s)", tried);
        return false;
    }
    g.eep_defaults = find_eeprom_defaults(dir, &g.eep_len);

    g.game = malloc(game->size);
    if (!g.game) {
        free_content();
        return false;
    }
    memcpy(g.game, game->data, game->size);
    g.game_len = game->size;
    exen_game_info(g.game, g.game_len, &g.gi);

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    g.xrgb = env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt) ? 1 : 0;
    if (!g.xrgb) {
        fmt = RETRO_PIXEL_FORMAT_RGB565;
        if (!env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
            log_printf(RETRO_LOG_WARN, "the frontend accepts neither XRGB8888 nor RGB565");
    }
    bool dupe = false;
    g.can_dupe = env_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe) && dupe;
    g.bitmasks = env_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL) ? 1 : 0;
    set_input_descriptors();
    struct retro_keyboard_callback kb = {on_keyboard};
    env_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb);
    /* the VM arena is stored in host byte order (src/core/state.c) */
    uint64_t quirks = RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT;
    env_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
    apply_audio_option();

    if (create_vm(1) < 0) {
        free_content();
        return false;
    }
    log_printf(RETRO_LOG_INFO, "%s (%s), screen %dx%d",
               g.gi.title[0] ? g.gi.title : g.gi.main_class,
               g.gi.match == EXEN_GAME_EXACT ? "known game"
               : g.gi.match ? "known main class" : "not in the game database",
               g.w, g.h);
    return true;
}

RETRO_API bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type;
    (void)info;
    (void)num;
    return false;
}

RETRO_API void retro_unload_game(void) { free_content(); }

RETRO_API void retro_reset(void)
{
    if (!g.game)
        return;
    int old_w = g.w, old_h = g.h;
    destroy_vm();
    if (create_vm(0) < 0)
        return; /* retro_run shows a black screen */
    if (g.w != old_w || g.h != old_h) {
        struct retro_system_av_info av;
        int grow = g.w > g.geom_w || g.h > g.geom_h;
        retro_get_system_av_info(&av);
        if (grow)
            env_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        else
            env_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av.geometry);
    }
}

RETRO_API void retro_run(void)
{
    input_poll_cb();
    check_options();
    g.apos = 0;
    uint64_t start = g.clock, end = start + UNITS_PER_FRAME;
    if (g.vm) {
        ensure_booted();
        deliver_keys(poll_phone_keys());
        int n = 0;
        while (g.period_ms > 0 && !exen_halted(g.vm) && g.next_tick <= end && n < MAX_TICKS_PER_FRAME) {
            if (g.next_tick > g.clock)
                g.clock = g.next_tick;
            audio_to((int)((g.clock - start) * FRAME_SAMPLES / UNITS_PER_FRAME));
            /* the next deadline first, so that a set_timer inside the tick replaces it */
            g.next_tick += (uint64_t)g.period_ms * UNITS_PER_MS;
            exen_tick(g.vm);
            n++;
        }
        if (g.period_ms > 0 && g.next_tick <= end) /* too far behind: drop the missed ticks */
            g.next_tick = end + (uint64_t)g.period_ms * UNITS_PER_MS;
        report_halt();
    }
    g.clock = end;
    audio_to(FRAME_SAMPLES);
    audio_batch_cb(g.audio, FRAME_SAMPLES);
    if (g.vm)
        memcpy(g.sram, exen_eeprom(g.vm), EXEN_EEPROM_SIZE);

    if (g.dirty || !g.can_dupe || !g.fb) {
        g.dirty = 0;
        if (g.fb) {
            video_cb(g.fb, (unsigned)g.w, (unsigned)g.h, (size_t)g.w * (g.xrgb ? 4 : 2));
        } else { /* no VM (a reset failed): a black frame */
            static uint32_t black[EXEN_DEFAULT_WIDTH * EXEN_DEFAULT_HEIGHT];
            video_cb(black, EXEN_DEFAULT_WIDTH, EXEN_DEFAULT_HEIGHT, EXEN_DEFAULT_WIDTH * 4);
        }
    } else {
        video_cb(NULL, (unsigned)g.w, (unsigned)g.h, (size_t)g.w * (g.xrgb ? 4 : 2));
    }
}

/* ------------------------------------------------------------ save states */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

/* Trailer (little-endian), in the last TRAILER_BYTES of the buffer:
 *   +0 "EXLR"  +4 u32 version  +8 u32 core state length  +12 u32 booted
 *   +16 u64 clock  +24 u64 next tick deadline  +32 i32 timer period (ms)
 *   +36 u32 phone keys held  +40 u32 stick directions held  +44..63 zero */

RETRO_API size_t retro_serialize_size(void) { return g.vm ? g.state_size : 0; }

RETRO_API bool retro_serialize(void *data, size_t size)
{
    if (!g.vm || size < g.state_size)
        return false;
    ensure_booted();
    uint8_t *b = data;
    int n = exen_state_save(g.vm, b, size - TRAILER_BYTES);
    if (n < 0) {
        log_printf(RETRO_LOG_ERROR, "save state: %s", exen_error(g.vm));
        return false;
    }
    /* the rest is zeroed so that the whole buffer depends only on the state (rewind and
     * netplay compare buffers) */
    memset(b + n, 0, size - (size_t)n);
    uint8_t *t = b + size - TRAILER_BYTES;
    memcpy(t, TRAILER_MAGIC, 4);
    put32(t + 4, TRAILER_VERSION);
    put32(t + 8, (uint32_t)n);
    put32(t + 12, (uint32_t)g.booted);
    put64(t + 16, g.clock);
    put64(t + 24, g.next_tick);
    put32(t + 32, (uint32_t)g.period_ms);
    put32(t + 36, g.held);
    put32(t + 40, g.stick);
    return true;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    if (!g.vm || !data || size < TRAILER_BYTES)
        return false;
    const uint8_t *b = data, *t = b + size - TRAILER_BYTES;
    uint32_t len = get32(t + 8);
    if (memcmp(t, TRAILER_MAGIC, 4) != 0 || get32(t + 4) != TRAILER_VERSION ||
        len > size - TRAILER_BYTES) {
        log_printf(RETRO_LOG_ERROR, "load state: not an exen-reloaded libretro state");
        return false;
    }
    /* the clock first: the core continues its own clock from the state's value, relative to
     * what ticks_ms returns during the load */
    uint64_t old_clock = g.clock;
    g.clock = get64(t + 16);
    if (exen_state_load(g.vm, b, len) < 0) {
        g.clock = old_clock;
        log_printf(RETRO_LOG_ERROR, "load state: %s", exen_error(g.vm));
        return false;
    }
    g.booted = get32(t + 12) != 0;
    g.next_tick = get64(t + 24);
    g.period_ms = (int32_t)get32(t + 32);
    g.held = get32(t + 36);
    g.stick = get32(t + 40);
    g.halt_reported = exen_halted(g.vm); /* a state of a stopped game: no new message */
    g.dirty = 1;
    return true;
}

RETRO_API void retro_cheat_reset(void) {}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index;
    (void)enabled;
    (void)code;
}

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void *retro_get_memory_data(unsigned id)
{
    return id == RETRO_MEMORY_SAVE_RAM && g.game ? g.sram : NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    return id == RETRO_MEMORY_SAVE_RAM && g.game ? EXEN_EEPROM_SIZE : 0;
}
