/* exen-run: headless runner.
 *
 *   exen-run [options] game.exn
 *
 * Boots the game and delivers N timer ticks on a virtual clock (no sleeping), then prints a
 * report: frames presented, timer period, halt reason, and native-call counts with the
 * unimplemented ones flagged. Frames can be dumped as PNG, the audio rendered to a WAV file in
 * step with the virtual clock, and key presses scripted (--help).
 * The compatibility harness builds on this.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exen.h"
#include "png.h"

const char *native_class(int index);
int native_implemented(int index);

typedef struct {
    uint32_t now_ms;
    int period;
    int frames;
    int quiet;
    int tick;
    const char *dump_dir;
    int dump_every, scale;
    int melodies;
    int log_melodies;
} Run;

static void on_present(void *u, const uint8_t *px, int w, int h)
{
    Run *r = u;
    if (r->dump_dir && r->frames % r->dump_every == 0) {
        char path[1024];
        snprintf(path, sizeof path, "%s/frame_%05d_t%05d.png", r->dump_dir, r->frames, r->tick);
        if (png_write_rgb332(path, px, w, h, r->scale) < 0)
            fprintf(stderr, "cannot write %s\n", path);
    }
    r->frames++;
}
static void on_timer(void *u, int ms) { ((Run *)u)->period = ms; }
static uint32_t on_ticks(void *u) { return ((Run *)u)->now_ms; }
static void on_melody(void *u, const uint8_t *d, int n)
{
    Run *r = u;
    r->melodies++;
    if (r->log_melodies) {
        printf("melody t=%u ms, %d bytes:", r->now_ms, n);
        for (int i = 0; i < n; i++)
            printf(" %02x", d[i]);
        printf("\n");
    }
}
static void on_melody_stop(void *u)
{
    Run *r = u;
    if (r->log_melodies)
        printf("melody stop t=%u ms\n", r->now_ms);
}
static void on_log(void *u, const char *m)
{
    if (!((Run *)u)->quiet)
        fprintf(stderr, "[vm] %s\n", m);
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* WAV output: 16-bit stereo PCM. The sizes are patched in when the file is closed. */
typedef struct {
    FILE *f;
    uint32_t frames;
    int rate;
    int16_t buf[2 * 4096];
} Wav;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wav_header(Wav *w)
{
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                     16, 0, 0, 0, 1, 0, 2, 0, /* PCM, 2 channels */
                     0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a'};
    uint32_t bytes = w->frames * 4u;
    put32(h + 4, 36 + bytes);
    put32(h + 24, (uint32_t)w->rate);
    put32(h + 28, (uint32_t)w->rate * 4u);
    put32(h + 40, bytes);
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, w->f);
    fseek(w->f, 0, SEEK_END);
}

/* Render the audio up to virtual time `ms`. */
static void wav_render_to(Wav *w, ExenVM *vm, uint32_t ms)
{
    uint64_t target = (uint64_t)ms * (uint64_t)w->rate / 1000u;
    while (w->frames < target) {
        uint64_t n = target - w->frames;
        if (n > 4096)
            n = 4096;
        exen_audio_render(vm, w->buf, (int)n);
        uint8_t le[4 * 4096];
        for (uint64_t i = 0; i < 2 * n; i++) {
            le[2 * i] = (uint8_t)w->buf[i];
            le[2 * i + 1] = (uint8_t)((uint16_t)w->buf[i] >> 8);
        }
        fwrite(le, 4, (size_t)n, w->f);
        w->frames += (uint32_t)n;
    }
}

/* Scripted input: "tick:key[/hold]" items separated by commas. */
typedef struct { int tick, key, hold; } KeyEv;

#define BAD_KEY 0x7fffffff

static int key_code(const char *k)
{
    static const struct { const char *name; int code; } names[] = {
        {"ok", EXEN_KEY_OK}, {"select", EXEN_KEY_OK}, {"clear", EXEN_KEY_CLEAR},
        {"back", EXEN_KEY_CLEAR}, {"up", EXEN_KEY_2}, {"down", EXEN_KEY_8},
        {"left", EXEN_KEY_4}, {"right", EXEN_KEY_6}, {"fire", EXEN_KEY_5},
        {"star", EXEN_KEY_STAR}, {"pound", EXEN_KEY_POUND},
        {"pad-up", EXEN_KEY_PAD_UP}, {"pad-down", EXEN_KEY_PAD_DOWN},
        {"pad-left", EXEN_KEY_PAD_LEFT}, {"pad-right", EXEN_KEY_PAD_RIGHT},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(k, names[i].name))
            return names[i].code;
    if (k[0] && !k[1] && ((k[0] >= '0' && k[0] <= '9') || k[0] == '*' || k[0] == '#'))
        return k[0];
    char *end;
    long v = strtol(k, &end, 0);
    if (*k && !*end)
        return (int)v;
    return BAD_KEY;
}

static int parse_keys(const char *spec, KeyEv *ev, int max)
{
    int n = 0;
    char buf[8192];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *item = strtok(buf, ","); item && n < max; item = strtok(NULL, ",")) {
        char *colon = strchr(item, ':');
        if (!colon)
            return -1;
        *colon = 0;
        char *slash = strchr(colon + 1, '/');
        int hold = 1;
        if (slash) {
            *slash = 0;
            hold = atoi(slash + 1);
            if (hold < 1)
                hold = 1;
        }
        int code = key_code(colon + 1);
        if (code == BAD_KEY)
            return -1;
        ev[n++] = (KeyEv){atoi(item), code, hold};
    }
    return n;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: exen-run [options] game.exn\n"
            "  --frames N        timer ticks to run (default 300)\n"
            "  --sim FILE        gensimu.exe to take the framework from\n"
            "  --size WxH        screen size (default 101x80)\n"
            "  --eeprom FILE     initial 2048-byte EEPROM image\n"
            "  --dump DIR        write presented frames as DIR/frame_NNNNN_tTTTTT.png\n"
            "                    (N = frame number, T = tick it was presented in)\n"
            "  --dump-every N    only every Nth presented frame (default 1)\n"
            "  --scale S         scale dumped frames by S (default 1)\n"
            "  --keys SPEC       scripted input: comma-separated tick:key[/hold]. The key is\n"
            "                    pressed just before tick `tick` and released `hold` ticks\n"
            "                    later (default 1). Keys: 0-9 * # ok clear up down left right\n"
            "                    fire (up/down/left/right/fire send 2/8/4/6/5, as gensimu maps\n"
            "                    the arrow keys), pad-up pad-down pad-left pad-right, or a\n"
            "                    number. Example: --keys \"20:ok,40:down/3,60:ok\"\n"
            "  --wav FILE        render the game's audio to FILE (16-bit stereo WAV) in step\n"
            "                    with the virtual clock\n"
            "  --rate HZ         audio sample rate (default 44100)\n"
            "  --melodies        print each melody the game starts (time and bytes)\n"
            "  --quiet           no VM log\n");
}

int main(int argc, char **argv)
{
    const char *sim = "vendor/gensimu/gensimu.exe", *game = NULL, *eeprom = NULL, *wav_path = NULL;
    static Wav wav;
    wav.rate = 44100;
    int frames = 300, sw = 0, sh = 0;
    Run run = {0};
    run.period = -1;
    run.dump_every = 1;
    run.scale = 1;
    static KeyEv keys[1024];
    int nkeys = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sim") && i + 1 < argc) sim = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) run.quiet = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) wav.rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--melodies")) run.log_melodies = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) run.dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) run.dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) run.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc) eeprom = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &sw, &sh) != 2) {
                usage();
                return 2;
            }
        } else if (!strcmp(argv[i], "--keys") && i + 1 < argc) {
            nkeys = parse_keys(argv[++i], keys, 1024);
            if (nkeys < 0) {
                fprintf(stderr, "bad --keys spec\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            usage();
            return 2;
        } else {
            game = argv[i];
        }
    }
    if (!game) {
        usage();
        return 2;
    }
    if (run.dump_every < 1)
        run.dump_every = 1;
    size_t sl, gl;
    uint8_t *simb = slurp(sim, &sl), *gameb = slurp(game, &gl);
    if (!simb || !gameb) {
        fprintf(stderr, "cannot read %s\n", simb ? game : sim);
        return 2;
    }
    ExenConfig cfg = {0};
    cfg.screen_width = sw;
    cfg.screen_height = sh;
    cfg.audio_rate = wav.rate;
    cfg.host = (ExenHost){&run, on_present, on_timer, on_ticks, on_melody, on_melody_stop, on_log};
    ExenVM *vm = exen_create(&cfg);
    if (!vm || exen_load_framework(vm, simb, sl) < 0 || exen_load_game(vm, gameb, gl) < 0) {
        fprintf(stderr, "load failed: %s\n", vm ? exen_error(vm) : "out of memory");
        return 1;
    }
    if (eeprom) {
        size_t el;
        uint8_t *eb = slurp(eeprom, &el);
        if (!eb) {
            fprintf(stderr, "cannot read %s\n", eeprom);
            return 2;
        }
        memcpy(exen_eeprom(vm), eb, el < 2048 ? el : 2048);
        free(eb);
    }
    if (wav_path) {
        wav.rate = exen_audio_rate(vm);
        wav.f = fopen(wav_path, "wb");
        if (!wav.f) {
            fprintf(stderr, "cannot write %s\n", wav_path);
            return 2;
        }
        wav_header(&wav);
    }
    int rc = exen_boot(vm);
    int ticks = 0;
    for (; rc == 0 && ticks < frames && !exen_halted(vm); ticks++) {
        run.tick = ticks;
        for (int k = 0; k < nkeys && rc == 0; k++) {
            if (keys[k].tick == ticks)
                rc = exen_key(vm, keys[k].key, 1);
            if (rc == 0 && keys[k].tick + keys[k].hold == ticks)
                rc = exen_key(vm, keys[k].key, 0);
        }
        if (rc != 0 || exen_halted(vm))
            break;
        run.now_ms += run.period > 0 ? (uint32_t)run.period : 100;
        if (wav.f) /* the audio up to this tick, which plays what earlier events started */
            wav_render_to(&wav, vm, run.now_ms);
        rc = exen_tick(vm);
    }
    if (wav.f) {
        /* one more period, so that what the last tick started is heard */
        wav_render_to(&wav, vm, run.now_ms + (run.period > 0 ? (uint32_t)run.period : 100));
        wav_header(&wav);
        fclose(wav.f);
    }
    printf("game: %s\nticks: %d  frames presented: %d  timer period: %d ms  melodies: %d\n", game,
           ticks, run.frames, run.period, run.melodies);
    if (wav_path)
        printf("audio: %s, %u frames at %d Hz (%.1f s)\n", wav_path, wav.frames, wav.rate,
               (double)wav.frames / wav.rate);
    printf("status: %s\n", exen_halted(vm) ? exen_error(vm) : "running");
    printf("natives called:\n");
    for (int i = 0; i < 256; i++) {
        const char *name;
        uint32_t n = exen_native_calls(vm, i, &name);
        if (n)
            printf("  #%-3d %-24s %-22s %8u%s\n", i, native_class(i), name, n,
                   native_implemented(i) ? "" : "   (unimplemented)");
    }
    int halted = exen_halted(vm);
    exen_destroy(vm);
    free(simb);
    free(gameb);
    return halted ? 1 : 0;
}
