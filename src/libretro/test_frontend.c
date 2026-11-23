/* exen-libretro-test: a headless libretro frontend that checks the exen-reloaded core.
 *
 *   exen-libretro-test [options] game.exn [game.exn ...]
 *
 * Loads the core as a shared library (dlopen / LoadLibrary), as a frontend would, and for
 * each game: loads it from memory, runs it for --frames frames with scripted RetroPad and
 * keyboard input, and checks that
 *   - every retro_run delivers one video frame (or a dupe) at the advertised geometry and
 *     exactly 735 stereo audio frames;
 *   - some frame is not flat (more than one colour);
 *   - the save RAM is the 2048-byte EEPROM, initialised from the system directory's
 *     flash/eeprom.dat config area when present;
 *   - retro_serialize_size is constant, and a state taken mid-run, loaded back and run again
 *     with the same input reproduces the same frames and audio exactly, and a state taken
 *     right after the load equals the loaded one byte for byte;
 *   - unloading and loading the game again and running it with the same input reproduces the
 *     first run exactly (determinism);
 *   - retro_reset restarts the game.
 * Once per run it also checks that a load fails cleanly, with a message, when gensimu.exe is
 * not in the system directory. Prints one line per game; exit status 0 if all passed.
 *
 * Options:
 *   --core PATH        the core (default: the one built beside this program)
 *   --system DIR       system directory holding gensimu.exe (default: vendor/gensimu)
 *   --frames N         frames per run (default 600, i.e. 10 s)
 *   --dump DIR         write frames as DIR/<game>_NNNNN.png
 *   --dump-every N     only every Nth frame (default 60)
 *   --expect-audio     fail a game that produces no audible sample
 *   --option KEY=VAL   core option value (e.g. exen_screen_size=128x128)
 *   --rgb565           refuse XRGB8888, so the core falls back to RGB565
 *   --verbose          print the core's log
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "libretro.h"
#include "png.h"

#ifndef EXEN_LIBRETRO_CORE_PATH
#define EXEN_LIBRETRO_CORE_PATH ""
#endif
#ifndef EXEN_SOURCE_DIR
#define EXEN_SOURCE_DIR "."
#endif

/* ------------------------------------------------------------ the core's entry points */

static struct {
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
} core;

static void *sym(void *lib, const char *name)
{
#ifdef _WIN32
    void *p = (void *)GetProcAddress((HMODULE)lib, name);
#else
    void *p = dlsym(lib, name);
#endif
    if (!p) {
        fprintf(stderr, "core: missing %s\n", name);
        exit(2);
    }
    return p;
}

static void load_core(const char *path)
{
#ifdef _WIN32
    void *lib = (void *)LoadLibraryA(path);
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib) {
        fprintf(stderr, "cannot load the core %s\n", path);
        exit(2);
    }
#define S(field, name) *(void **)&core.field = sym(lib, name)
    S(set_environment, "retro_set_environment");
    S(set_video_refresh, "retro_set_video_refresh");
    S(set_audio_sample, "retro_set_audio_sample");
    S(set_audio_sample_batch, "retro_set_audio_sample_batch");
    S(set_input_poll, "retro_set_input_poll");
    S(set_input_state, "retro_set_input_state");
    S(init, "retro_init");
    S(deinit, "retro_deinit");
    S(api_version, "retro_api_version");
    S(get_system_info, "retro_get_system_info");
    S(get_system_av_info, "retro_get_system_av_info");
    S(reset, "retro_reset");
    S(run, "retro_run");
    S(serialize_size, "retro_serialize_size");
    S(serialize, "retro_serialize");
    S(unserialize, "retro_unserialize");
    S(load_game, "retro_load_game");
    S(unload_game, "retro_unload_game");
    S(get_memory_data, "retro_get_memory_data");
    S(get_memory_size, "retro_get_memory_size");
#undef S
}

/* ------------------------------------------------------------ frontend state */

static const char *sys_dir;
static int verbose, rgb565;
static char messages[2048];
static int log_errors;
static retro_keyboard_event_t kb_cb;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static int options_v2_set;

#define MAX_OPTS 16
static struct { char key[64], val[64]; } opts[MAX_OPTS];
static int nopts;

/* the frame on screen, as XRGB8888 */
static uint32_t screen[1024 * 1024];
static unsigned scr_w, scr_h;
static int frame_presented, frame_duped, frame_bad;
static uint32_t frame_crc;
static int nonflat;

/* audio of the current retro_run */
static int audio_calls, audio_frames_run, audio_bad;
static uint32_t audio_crc;
static long long audible;

/* scripted input */
static int frame_no;

static uint32_t crc32(uint32_t c, const void *data, size_t n)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = (v & 1) ? (v >> 1) ^ 0xedb88320u : v >> 1;
            t[i] = v;
        }
    const uint8_t *p = data;
    c = ~c;
    while (n--)
        c = t[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void RETRO_CALLCONV fe_log(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    if (level >= RETRO_LOG_ERROR)
        log_errors++;
    if (!verbose && level < RETRO_LOG_WARN)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "  [core] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void note_message(const char *msg)
{
    size_t l = strlen(messages);
    snprintf(messages + l, sizeof messages - l, "%s%s", l ? " | " : "", msg);
    if (verbose)
        fprintf(stderr, "  [message] %s\n", msg);
}

static bool RETRO_CALLCONV fe_env(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = sys_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        return false;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
        if (f == RETRO_PIXEL_FORMAT_XRGB8888 && rgb565)
            return false;
        pixfmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = fe_log;
        return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *(unsigned *)data = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        note_message(((const struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
        note_message(((const struct retro_message_ext *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
        const struct retro_core_options_v2 *o = data;
        options_v2_set = 0;
        for (const struct retro_core_option_v2_definition *d = o->definitions; d->key; d++)
            options_v2_set++;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        for (int i = 0; i < nopts; i++)
            if (!strcmp(opts[i].key, v->key)) {
                v->value = opts[i].val;
                return true;
            }
        return false; /* the core's default */
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        kb_cb = ((const struct retro_keyboard_callback *)data)->callback;
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        return true;
    default:
        return false;
    }
}

static void RETRO_CALLCONV fe_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    frame_presented++;
    if (!data) {
        frame_duped++;
        if (w != scr_w || h != scr_h)
            frame_bad++;
        return;
    }
    if (w == 0 || h == 0 || w > 1024 || h > 1024) {
        frame_bad++;
        return;
    }
    scr_w = w;
    scr_h = h;
    const uint8_t *row = data;
    for (unsigned y = 0; y < h; y++, row += pitch)
        for (unsigned x = 0; x < w; x++) {
            uint32_t c;
            if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
                uint16_t v = ((const uint16_t *)row)[x];
                uint32_t r = v >> 11, g = (v >> 5) & 0x3f, b = v & 0x1f;
                c = 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            } else {
                c = 0xff000000u | (((const uint32_t *)row)[x] & 0xffffffu);
            }
            screen[y * w + x] = c;
        }
    for (unsigned i = 1; i < w * h && !nonflat; i++)
        if (screen[i] != screen[0])
            nonflat = 1;
}

static void RETRO_CALLCONV fe_audio(int16_t left, int16_t right)
{
    (void)left;
    (void)right;
    audio_bad++; /* the core uses the batch callback only */
}

static size_t RETRO_CALLCONV fe_audio_batch(const int16_t *data, size_t frames)
{
    audio_calls++;
    audio_frames_run += (int)frames;
    audio_crc = crc32(audio_crc, data, frames * 4);
    for (size_t i = 0; i < frames * 2; i++)
        if (data[i])
            audible++;
    return frames;
}

static void RETRO_CALLCONV fe_poll(void) {}

/* The script: a press every 20 frames, cycling through menu and game keys, each held for 5
 * frames. It opens menus, starts most games and moves around in them. */
static unsigned script_pad(int f)
{
    static const unsigned seq[] = {
        RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_START,
        RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_L,
    };
    if (f < 40 || f % 20 >= 5)
        return 0;
    unsigned m = 1u << seq[(f / 20) % (sizeof seq / sizeof seq[0])];
    if (f % 200 < 5) /* now and then two buttons on one phone key: B and the 5 on the keyboard */
        m |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
    return m;
}

static int16_t RETRO_CALLCONV fe_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0)
        return 0;
    unsigned m = script_pad(frame_no);
    if (device == RETRO_DEVICE_JOYPAD) {
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
            return (int16_t)m;
        return id < 16 ? (int16_t)((m >> id) & 1) : 0;
    }
    if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
        /* push the stick right for a while every 300 frames */
        int f = frame_no % 300;
        if (id == RETRO_DEVICE_ID_ANALOG_X && f >= 250 && f < 270)
            return 30000;
    }
    return 0;
}

/* Keyboard events for frame f, through the core's keyboard callback: numpad 5 overlapping
 * the RetroPad's B now and then, and Enter. */
static void script_keyboard(int f)
{
    if (!kb_cb)
        return;
    if (f >= 40 && f % 200 == 2)
        kb_cb(true, RETROK_KP5, 0, 0);
    if (f >= 40 && f % 200 == 9)
        kb_cb(false, RETROK_KP5, 0, 0);
    if (f >= 40 && f % 400 == 110)
        kb_cb(true, RETROK_RETURN, 0, 0);
    if (f >= 40 && f % 400 == 114)
        kb_cb(false, RETROK_RETURN, 0, 0);
}

/* ------------------------------------------------------------ runs */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = n > 0 ? (size_t)n : 0;
    return b;
}

typedef struct {
    uint32_t *video; /* CRC of the screen after each frame */
    uint32_t *audio; /* CRC of each frame's audio */
} Trace;

static const char *dump_dir, *dump_stem;
static int dump_every = 60;

/* Run frames [from, to), recording into tr (indexed by frame number) when given. Returns the
 * number of frames that broke the per-frame rules. */
static int run_frames(int from, int to, Trace *tr)
{
    int bad = 0;
    for (frame_no = from; frame_no < to; frame_no++) {
        script_keyboard(frame_no);
        frame_presented = 0;
        audio_calls = audio_frames_run = 0;
        audio_crc = 0;
        int before_bad = frame_bad;
        core.run();
        if (frame_presented != 1 || frame_bad != before_bad || audio_frames_run != 735)
            bad++;
        frame_crc = crc32(0, screen, (size_t)scr_w * scr_h * 4);
        if (tr) {
            tr->video[frame_no] = frame_crc;
            tr->audio[frame_no] = audio_crc;
        }
        if (dump_dir && frame_no % dump_every == 0) {
            char path[1200];
            snprintf(path, sizeof path, "%s/%s_%05d.png", dump_dir, dump_stem, frame_no);
            if (png_write_xrgb8888(path, screen, (int)scr_w, (int)scr_h, 2) < 0)
                fprintf(stderr, "cannot write %s\n", path);
        }
    }
    return bad;
}

static int load(const uint8_t *data, size_t len, const char *path)
{
    struct retro_game_info gi = {path, data, len, NULL};
    messages[0] = 0;
    scr_w = scr_h = 0;
    return core.load_game(&gi) ? 0 : -1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b > s)
        s = b;
    return s ? s + 1 : p;
}

static int test_game(const char *path, int frames, int expect_audio, const uint8_t *defaults,
                     size_t defaults_len)
{
    const char *name = base_name(path);
    size_t len = 0;
    uint8_t *data = slurp(path, &len);
    char why[512] = "";
#define FAIL(...) do { if (!why[0]) snprintf(why, sizeof why, __VA_ARGS__); } while (0)
    if (!data) {
        printf("FAIL  %s: cannot read the file\n", name);
        return 1;
    }
    Trace a = {calloc((size_t)frames, 4), calloc((size_t)frames, 4)};
    Trace b = {calloc((size_t)frames, 4), calloc((size_t)frames, 4)};
    Trace c = {calloc((size_t)frames, 4), calloc((size_t)frames, 4)};
    uint8_t *s1 = NULL, *s2 = NULL;
    size_t ssize = 0;
    struct retro_system_av_info av;
    nonflat = 0;
    audible = 0;
    frame_bad = 0;
    log_errors = 0;

    /* run A: load, run to the middle, save, run to the end */
    if (load(data, len, path) < 0) {
        printf("FAIL  %s: retro_load_game failed (%s)\n", name, messages);
        free(data);
        return 1;
    }
    core.get_system_av_info(&av);
    if (av.timing.fps != 60.0 || av.timing.sample_rate != 44100.0)
        FAIL("timing %.2f fps %.0f Hz", av.timing.fps, av.timing.sample_rate);
    if (av.geometry.base_width > av.geometry.max_width || av.geometry.base_height > av.geometry.max_height)
        FAIL("base geometry exceeds max");
    uint8_t *sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    size_t sram_len = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!sram || sram_len != 2048)
        FAIL("save RAM %p, %zu bytes", (void *)sram, sram_len);
    else if (defaults && defaults_len >= 0x21c && memcmp(sram, defaults, 0x21c))
        FAIL("save RAM config area differs from flash/eeprom.dat");
    ssize = core.serialize_size();
    if (ssize == 0)
        FAIL("serialize_size is 0");
    /* the save point falls while a scripted key is held (pressed at frames 20k..20k+4), so
     * the key state across a load is exercised too */
    int mid = frames / 2 / 20 * 20 + 2;
    dump_stem = name;
    int bad = run_frames(0, mid, &a);
    if (core.serialize_size() != ssize)
        FAIL("serialize_size changed");
    s1 = calloc(1, ssize);
    s2 = calloc(1, ssize);
    if (!s1 || !s2 || !core.serialize(s1, ssize))
        FAIL("retro_serialize failed");
    bad += run_frames(mid, frames, &a);
    dump_dir = NULL; /* frames are dumped once, during run A */
    long long audible_a = audible;
    int nonflat_a = nonflat;
    if (audio_bad)
        FAIL("the core used the single-sample audio callback");

    /* back to the middle: the state must load, compare equal, and replay the same */
    if (!why[0]) {
        if (!core.unserialize(s1, ssize))
            FAIL("retro_unserialize failed (%s)", messages);
        else if (!core.serialize(s2, ssize))
            FAIL("retro_serialize after the load failed");
        else if (memcmp(s1, s2, ssize))
            FAIL("a state taken right after a load differs from the loaded one");
        else {
            bad += run_frames(mid, frames, &b);
            for (int f = mid; f < frames && !why[0]; f++)
                if (a.video[f] != b.video[f] || a.audio[f] != b.audio[f])
                    FAIL("after the state load, frame %d %s differs", f,
                         a.video[f] != b.video[f] ? "video" : "audio");
        }
    }
    if (core.serialize_size() != ssize)
        FAIL("serialize_size changed");

    /* reset: the game restarts and keeps running */
    core.reset();
    nonflat = 0;
    bad += run_frames(0, frames < 120 ? frames : 120, NULL);
    core.unload_game();

    /* run B: a fresh load replays run A */
    if (!why[0]) {
        if (load(data, len, path) < 0) {
            FAIL("second retro_load_game failed");
        } else {
            bad += run_frames(0, frames, &c);
            for (int f = 0; f < frames && !why[0]; f++)
                if (a.video[f] != c.video[f] || a.audio[f] != c.audio[f])
                    FAIL("not deterministic: frame %d %s differs between two loads", f,
                         a.video[f] != c.video[f] ? "video" : "audio");
            core.unload_game();
        }
    }

    /* save RAM the frontend writes after the load (as frontends load .srm files) reaches the
     * game: it is in the state's EEPROM image after the first frame; the pointer survives a
     * reset, and so does the content */
    if (!why[0] && load(data, len, path) == 0) {
        static const uint8_t marker[16] = "exen-srm-marker";
        uint8_t *p = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
        memcpy(p + 2048 - sizeof marker, marker, sizeof marker);
        frame_no = 0;
        core.run();
        int found = 0;
        if (core.serialize(s1, ssize))
            for (size_t i = 0; i + sizeof marker <= ssize && !found; i++)
                found = !memcmp(s1 + i, marker, sizeof marker);
        if (!found)
            FAIL("save RAM written after the load did not reach the game");
        core.reset();
        core.run();
        if (core.get_memory_data(RETRO_MEMORY_SAVE_RAM) != p)
            FAIL("the save RAM pointer changed on reset");
        else if (memcmp(p + 2048 - sizeof marker, marker, sizeof marker))
            FAIL("the save RAM was lost on reset");
        core.unload_game();
    }
    if (bad)
        FAIL("%d frames broke the per-frame rules (one video frame, 735 audio frames)", bad);
    if (!nonflat_a)
        FAIL("every frame is a single colour");
    if (expect_audio && !audible_a)
        FAIL("no audio");
    printf("%s  %-26s %3ux%-3u  audio %-8s state %zu bytes%s%s%s\n", why[0] ? "FAIL" : "PASS",
           name, av.geometry.base_width, av.geometry.base_height,
           audible_a ? "yes" : "silent", ssize, why[0] ? "  -- " : "", why,
           log_errors ? "  (core logged errors)" : "");
    if (messages[0] && verbose)
        printf("      messages: %s\n", messages);
    free(a.video); free(a.audio); free(b.video); free(b.audio); free(c.video); free(c.audio);
    free(s1);
    free(s2);
    free(data);
    return why[0] ? 1 : 0;
#undef FAIL
}

/* Without gensimu.exe in the system directory the load must fail with a message. */
static int test_missing_system(const char *game_path)
{
    size_t len = 0;
    uint8_t *data = slurp(game_path, &len);
    const char *saved = sys_dir;
    sys_dir = EXEN_SOURCE_DIR "/no-such-system-dir";
    int ok = data && load(data, len, game_path) < 0 && strstr(messages, "gensimu.exe");
    if (!ok && data)
        core.unload_game();
    sys_dir = saved;
    printf("%s  missing gensimu.exe is reported (%s)\n", ok ? "PASS" : "FAIL",
           messages[0] ? messages : "no message");
    /* and data that is not a game is refused */
    static const uint8_t junk[64] = "not an exn file";
    int ok2 = load(junk, sizeof junk, "junk.exn") < 0;
    if (!ok2)
        core.unload_game();
    printf("%s  a file that is not a game is refused (%s)\n", ok2 ? "PASS" : "FAIL",
           messages[0] ? messages : "no message");
    free(data);
    return ok && ok2 ? 0 : 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: exen-libretro-test [--core PATH] [--system DIR] [--frames N] "
                    "[--dump DIR] [--dump-every N]\n"
                    "                          [--expect-audio] [--option KEY=VALUE] [--rgb565] "
                    "[--verbose] game.exn...\n");
}

int main(int argc, char **argv)
{
    const char *core_path = EXEN_LIBRETRO_CORE_PATH;
    const char *dump = NULL;
    int frames = 600, expect_audio = 0, first = 0;
    sys_dir = EXEN_SOURCE_DIR "/vendor/gensimu";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
        else if (!strcmp(argv[i], "--system") && i + 1 < argc) sys_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--expect-audio")) expect_audio = 1;
        else if (!strcmp(argv[i], "--rgb565")) rgb565 = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--option") && i + 1 < argc) {
            const char *kv = argv[++i], *eq = strchr(kv, '=');
            if (!eq || nopts == MAX_OPTS) {
                usage();
                return 2;
            }
            snprintf(opts[nopts].key, sizeof opts[nopts].key, "%.*s", (int)(eq - kv), kv);
            snprintf(opts[nopts].val, sizeof opts[nopts].val, "%s", eq + 1);
            nopts++;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else if (!first) {
            first = i;
        }
    }
    if (!first || !core_path[0] || frames < 10 || dump_every < 1) {
        usage();
        return 2;
    }
    load_core(core_path);
    if (core.api_version() != RETRO_API_VERSION) {
        fprintf(stderr, "core API version %u\n", core.api_version());
        return 1;
    }
    struct retro_system_info si;
    core.get_system_info(&si);
    printf("core: %s %s, extensions \"%s\", need_fullpath %d\n", si.library_name,
           si.library_version, si.valid_extensions, si.need_fullpath);
    core.set_environment(fe_env);
    core.set_video_refresh(fe_video);
    core.set_audio_sample(fe_audio);
    core.set_audio_sample_batch(fe_audio_batch);
    core.set_input_poll(fe_poll);
    core.set_input_state(fe_input);
    core.init();
    if (options_v2_set != 4) /* screen size, sound, melody voice, SMS */
        printf("FAIL  core options: %d v2 definitions\n", options_v2_set);

    char eep_path[1200];
    snprintf(eep_path, sizeof eep_path, "%s/flash/eeprom.dat", sys_dir);
    size_t dl = 0;
    uint8_t *defaults = slurp(eep_path, &dl);

    int failed = options_v2_set != 4;
    failed += test_missing_system(argv[first]);
    int ngames = 0, nfail = 0;
    for (int i = first; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--expect-audio") && strcmp(argv[i], "--rgb565") &&
                strcmp(argv[i], "--verbose"))
                i++; /* an option's value */
            continue;
        }
        dump_dir = dump;
        ngames++;
        nfail += test_game(argv[i], frames, expect_audio, defaults, dl);
    }
    core.deinit();
    free(defaults);
    printf("%d of %d games passed\n", ngames - nfail, ngames);
    return failed || nfail ? 1 : 0;
}
