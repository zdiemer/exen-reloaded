/* exen-run: headless runner.
 *
 *   exen-run [options] game.exn
 *
 * Boots the game and delivers N timer ticks on a virtual clock (no sleeping), then prints a
 * report: frames presented, timer period, halt reason, and native-call counts with the
 * unimplemented ones flagged. Frames can be dumped as PNG, the audio rendered to a WAV file in
 * step with the virtual clock, and key presses scripted (--help).
 * The compatibility harness builds on this.
 *
 * Save states: --save-at N FILE writes a state before tick N, --load FILE resumes from one
 * (the file is the core's state followed by a 16-byte trailer with this runner's own clock:
 * "XRUN", tick, virtual ms, frames presented), and --selftest-state checks that a state round
 * trip is exact: run to tick N, save, run on to the last tick recording every frame, the audio
 * and the final state; then load the state into a fresh VM, run to the same tick, and compare.
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
    /* --selftest-state: CRC of every frame presented while `record` is set */
    int record, nrec, caprec;
    uint32_t *rec;
    int loading;   /* a state load presents the restored screen: not a game frame */
    int eeprom_writes;
} Run;

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = (v & 1) ? (v >> 1) ^ 0xedb88320u : v >> 1;
            t[i] = v;
        }
    c = ~c;
    while (n--)
        c = t[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void on_present(void *u, const uint8_t *px, int w, int h)
{
    Run *r = u;
    if (r->loading)
        return;
    if (r->record) {
        if (r->nrec == r->caprec) {
            int nc = r->caprec ? r->caprec * 2 : 256;
            uint32_t *n = realloc(r->rec, (size_t)nc * sizeof *n);
            if (!n)
                return;
            r->rec = n;
            r->caprec = nc;
        }
        r->rec[r->nrec++] = crc32_update(0, px, (size_t)w * (size_t)h);
    }
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
static void on_eeprom(void *u) { ((Run *)u)->eeprom_writes++; }

/* --sms MODE, --sms-log (SMS: the core's mock In-Fusio server, docs/re/SMS.md) */
static int g_sms_mode = EXEN_SMS_SERVER, g_sms_log;

static void on_log(void *u, const char *m)
{
    if (g_sms_log && !strncmp(m, "sms: ", 5))
        printf("tick %d: %s\n", ((Run *)u)->tick, m); /* on stdout even with --quiet */
    else if (!((Run *)u)->quiet)
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
    FILE *f;       /* NULL: render only (--selftest-state hashes the audio) */
    int on;
    uint32_t frames;
    int rate;
    uint32_t crc;
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
        if (w->f)
            fwrite(le, 4, (size_t)n, w->f);
        w->crc = crc32_update(w->crc, le, (size_t)n * 4);
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
            "  --size WxH        screen size (default: the game's handset from the game\n"
            "                    database, else 132x176)\n"
            "  --eeprom FILE     initial 2048-byte EEPROM image\n"
            "  --eeprom-out FILE write the EEPROM image to FILE at the end (may equal --eeprom)\n"
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
            "  --voice V         melody voice: ringtone (default) or gm (General MIDI, like the\n"
            "                    2003 simulator; \"piano\" is accepted for gm)\n"
            "  --soundfont PATH  General MIDI SoundFont (.sf2) for the gm voice. Default, with\n"
            "                    --voice gm: TimGM6mb.sf2 in soundfont/ beside the program or\n"
            "                    beside it, else vendor/soundfont/. \"none\": the gm voice's\n"
            "                    procedural piano (its fallback without a SoundFont)\n"
            "  --melodies        print each melody the game starts (time and bytes)\n"
            "  --save-at N FILE  write a save state to FILE just before tick N\n"
            "  --load FILE       resume from a state written by --save-at instead of booting;\n"
            "                    ticks continue from the saved tick up to --frames\n"
            "  --selftest-state  check that a save state round trip is exact: save before tick\n"
            "                    N (--save-at N, FILE optional; default --frames / 2), run to\n"
            "                    --frames, then load the state into a fresh VM and run again.\n"
            "                    Frames, audio and the final state must be identical (with\n"
            "                    a SoundFont, its voices too). Prints PASS or FAIL; exit\n"
            "                    status 0 or 1.\n"
            "  --sms MODE        SMS: server (default; the mock In-Fusio server answers),\n"
            "                    delivered (sent, never answered) or offline (not sent)\n"
            "  --sms-log         print the decoded SMS traffic (and a hex dump) on stdout\n"
            "  --quiet           no VM log\n");
}

typedef struct {
    const uint8_t *sim, *game, *eeprom, *sf2;
    size_t sl, gl, el, sf2l;
    int sw, sh, rate, voice;
} Inputs;

static ExenVM *make_vm(const Inputs *in, Run *run)
{
    ExenConfig cfg = {0};
    cfg.screen_width = in->sw;
    cfg.screen_height = in->sh;
    cfg.audio_rate = in->rate;
    cfg.host = (ExenHost){
        .user = run,
        .present = on_present,
        .set_timer = on_timer,
        .ticks_ms = on_ticks,
        .melody_play = on_melody,
        .melody_stop = on_melody_stop,
        .log = on_log,
        .eeprom_changed = on_eeprom,
    };
    ExenVM *vm = exen_create(&cfg);
    if (!vm || exen_load_framework(vm, in->sim, in->sl) < 0 ||
        exen_load_game(vm, in->game, in->gl) < 0) {
        fprintf(stderr, "load failed: %s\n", vm ? exen_error(vm) : "out of memory");
        exen_destroy(vm);
        return NULL;
    }
    if (in->eeprom)
        memcpy(exen_eeprom(vm), in->eeprom, in->el < EXEN_EEPROM_SIZE ? in->el : EXEN_EEPROM_SIZE);
    if (in->sf2 && exen_audio_set_soundfont(vm, in->sf2, in->sf2l) < 0) {
        fprintf(stderr, "the SoundFont is not a usable SoundFont 2 file\n");
        exen_destroy(vm);
        return NULL;
    }
    exen_audio_set_voice(vm, in->voice);
    exen_sms_set_mode(vm, g_sms_mode);
    exen_sms_set_log(vm, g_sms_log);
    return vm;
}

/* Deliver ticks [*tick, to): the scripted keys, then the virtual clock advances, the audio is
 * rendered up to it and the tick runs. */
static int run_ticks(ExenVM *vm, Run *run, Wav *wav, int *tick, int to, const KeyEv *keys,
                     int nkeys)
{
    int rc = 0;
    for (; rc == 0 && *tick < to && !exen_halted(vm); (*tick)++) {
        int t = *tick;
        run->tick = t;
        for (int k = 0; k < nkeys && rc == 0; k++) {
            if (keys[k].tick == t)
                rc = exen_key(vm, keys[k].key, 1);
            if (rc == 0 && keys[k].tick + keys[k].hold == t)
                rc = exen_key(vm, keys[k].key, 0);
        }
        if (rc != 0 || exen_halted(vm))
            break;
        run->now_ms += run->period > 0 ? (uint32_t)run->period : 100;
        if (wav->on) /* the audio up to this tick, which plays what earlier events started */
            wav_render_to(wav, vm, run->now_ms);
        rc = exen_tick(vm);
    }
    return rc;
}

/* The current state in a malloc'd buffer, or NULL with a message. */
static uint8_t *take_state(ExenVM *vm, size_t *len)
{
    size_t n = exen_state_size(vm);
    uint8_t *b = n ? malloc(n) : NULL;
    int r = b ? exen_state_save(vm, b, n) : -1;
    if (r < 0) {
        fprintf(stderr, "save state failed: %s\n", exen_error(vm));
        free(b);
        return NULL;
    }
    *len = (size_t)r;
    return b;
}

/* A --save-at file is the core's state followed by this runner's clock. */
#define TRAILER 16

static int write_state_file(const char *path, ExenVM *vm, const Run *run, int tick)
{
    size_t n;
    uint8_t *b = take_state(vm, &n);
    if (!b)
        return -1;
    uint8_t t[TRAILER] = {'X', 'R', 'U', 'N'};
    put32(t + 4, (uint32_t)tick);
    put32(t + 8, run->now_ms);
    put32(t + 12, (uint32_t)run->frames);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(b, 1, n, f) == n && fwrite(t, 1, TRAILER, f) == TRAILER;
    if (f && fclose(f) != 0)
        ok = 0;
    free(b);
    if (!ok) {
        fprintf(stderr, "cannot write %s\n", path);
        return -1;
    }
    printf("state: saved %s before tick %d (%zu bytes)\n", path, tick, n);
    return 0;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Resume `vm` from a --save-at file: the runner's clock first, so that the core's clock offset
 * comes out right. */
static int load_state_file(const char *path, ExenVM *vm, Run *run, Wav *wav, int *tick)
{
    size_t n;
    uint8_t *b = slurp(path, &n);
    if (!b) {
        fprintf(stderr, "cannot read %s\n", path);
        return -1;
    }
    if (n > TRAILER && !memcmp(b + n - TRAILER, "XRUN", 4)) {
        *tick = (int)get32(b + n - TRAILER + 4);
        run->now_ms = get32(b + n - TRAILER + 8);
        run->frames = (int)get32(b + n - TRAILER + 12);
    }
    wav->frames = (uint32_t)((uint64_t)run->now_ms * (uint64_t)wav->rate / 1000u);
    run->loading = 1;
    int r = exen_state_load(vm, b, n);
    run->loading = 0;
    free(b);
    if (r < 0) {
        fprintf(stderr, "%s: %s\n", path, exen_error(vm));
        return -1;
    }
    printf("state: loaded %s, resuming at tick %d\n", path, *tick);
    return 0;
}

static int same(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    return a && b && an == bn && !memcmp(a, b, an);
}

/* --selftest-state: run A boots, runs to tick n, saves S, runs on to m. Run B loads S into a
 * fresh VM and runs to m. Compared: S against a state taken right after the load, every frame
 * presented after the save point, the audio rendered after it, and the final states. */
static int selftest(const Inputs *in, int n, int m, const KeyEv *keys, int nkeys, int quiet,
                    const char *save_path)
{
    static Wav wa, wb;
    Run ra = {0}, rb = {0};
    ra.period = rb.period = -1;
    ra.dump_every = rb.dump_every = 1;
    ra.quiet = rb.quiet = quiet;
    wa.on = wb.on = 1;
    int tick = 0;
    size_t s1n = 0, s1bn = 0, san = 0, sbn = 0;
    uint8_t *s1b = NULL, *sa = NULL, *sb = NULL;
    const char *why = NULL;

    ExenVM *a = make_vm(in, &ra);
    if (!a)
        return 2;
    wa.rate = exen_audio_rate(a);
    if (exen_boot(a) == 0)
        run_ticks(a, &ra, &wa, &tick, n, keys, nkeys);
    int save_tick = tick, halted_early = exen_halted(a);
    if (save_path)
        write_state_file(save_path, a, &ra, tick);
    uint8_t *s1 = take_state(a, &s1n);
    if (!s1) {
        exen_destroy(a);
        printf("selftest-state: FAIL (cannot save)\n");
        return 1;
    }
    uint32_t a_ms = ra.now_ms, a_audio = wa.frames;
    int a_frames = ra.frames;
    ra.record = 1;
    wa.crc = 0;
    run_ticks(a, &ra, &wa, &tick, m, keys, nkeys);
    int a_end = tick, a_halted = exen_halted(a);
    sa = take_state(a, &san);

    ExenVM *b = make_vm(in, &rb);
    if (!b) {
        exen_destroy(a);
        free(s1);
        return 2;
    }
    wb.rate = exen_audio_rate(b);
    rb.now_ms = a_ms;
    rb.frames = a_frames;
    wb.frames = a_audio;
    rb.loading = 1;
    int lr = exen_state_load(b, s1, s1n);
    rb.loading = 0;
    if (lr < 0) {
        why = exen_error(b);
    } else {
        s1b = take_state(b, &s1bn);
        if (!same(s1, s1n, s1b, s1bn))
            why = "the state taken right after the load differs from the saved one";
        tick = save_tick;
        rb.record = 1;
        wb.crc = 0;
        run_ticks(b, &rb, &wb, &tick, m, keys, nkeys);
        sb = take_state(b, &sbn);
        if (!why && (tick != a_end || exen_halted(b) != a_halted))
            why = "the runs ended differently";
        if (!why && rb.nrec != ra.nrec)
            why = "different number of frames";
        for (int i = 0; !why && i < ra.nrec; i++)
            if (ra.rec[i] != rb.rec[i]) {
                printf("selftest-state: frame %d after the save point differs\n", i);
                why = "a frame differs";
            }
        if (!why && (wa.frames != wb.frames || wa.crc != wb.crc))
            why = "the audio differs";
        if (!why && !same(sa, san, sb, sbn))
            why = "the final states differ";
    }
    if (!why)
        printf("selftest-state: PASS (saved before tick %d%s, ran to tick %d%s; %d frames and "
               "%u audio frames identical; state %zu bytes)\n",
               save_tick, halted_early ? " (VM already halted)" : "", a_end,
               a_halted && !halted_early ? " (halted)" : "", ra.nrec, wa.frames - a_audio, s1n);
    else
        printf("selftest-state: FAIL (%s; saved before tick %d, ran to tick %d)\n", why,
               save_tick, a_end);
    free(s1);
    free(s1b);
    free(sa);
    free(sb);
    free(ra.rec);
    free(rb.rec);
    exen_destroy(a);
    exen_destroy(b);
    return why ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *sim = NULL, *game = NULL, *eeprom = NULL, *eeprom_out = NULL, *wav_path = NULL;
    const char *save_path = NULL, *load_path = NULL, *sf_path = NULL;
    static Wav wav;
    wav.rate = 44100;
    int frames = 300, sw = 0, sh = 0, save_at = -1, self = 0, voice = EXEN_VOICE_RINGTONE;
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
        else if (!strcmp(argv[i], "--sms-log")) g_sms_log = 1;
        else if (!strcmp(argv[i], "--sms") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "server"))
                g_sms_mode = EXEN_SMS_SERVER;
            else if (!strcmp(m, "delivered"))
                g_sms_mode = EXEN_SMS_DELIVERED;
            else if (!strcmp(m, "offline"))
                g_sms_mode = EXEN_SMS_OFFLINE;
            else {
                fprintf(stderr, "--sms: unknown mode %s (server, delivered, offline)\n", m);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "ringtone"))
                voice = EXEN_VOICE_RINGTONE;
            else if (!strcmp(v, "gm") || !strcmp(v, "piano"))
                voice = EXEN_VOICE_GM;
            else {
                usage();
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--soundfont") && i + 1 < argc) sf_path = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) run.dump_dir = argv[++i];
        else if (!strcmp(argv[i], "--dump-every") && i + 1 < argc) run.dump_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) run.scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc) eeprom = argv[++i];
        else if (!strcmp(argv[i], "--eeprom-out") && i + 1 < argc) eeprom_out = argv[++i];
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) load_path = argv[++i];
        else if (!strcmp(argv[i], "--selftest-state")) self = 1;
        else if (!strcmp(argv[i], "--save-at") && i + 1 < argc) {
            save_at = atoi(argv[++i]);
            /* FILE may be left out with --selftest-state */
            size_t l = i + 1 < argc ? strlen(argv[i + 1]) : 0;
            if (i + 2 < argc && strncmp(argv[i + 1], "--", 2) &&
                !(l >= 4 && !strcmp(argv[i + 1] + l - 4, ".exn")))
                save_path = argv[++i];
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
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
    if (!game || (save_at >= 0 && !save_path && !self) || (self && load_path)) {
        usage();
        return 2;
    }
    if (run.dump_every < 1)
        run.dump_every = 1;
    size_t sl = 0, gl = 0;
    uint8_t *simb = NULL, *gameb = slurp(game, &gl);
    /* the executable's directory, with its separator ("" if argv[0] has none) */
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", argv[0]);
    char *cut = strrchr(dir, '/'), *cut2 = strrchr(dir, '\\');
    if (cut2 && (!cut || cut2 > cut)) /* the later separator; never compare against NULL */
        cut = cut2;
    if (cut)
        cut[1] = 0;
    else
        dir[0] = 0;
    if (sim) {
        simb = slurp(sim, &sl);
    } else {
        /* beside the executable, then the source-tree layout, then the working directory */
        static const char *rel[] = {"gensimu.exe", "../vendor/gensimu/gensimu.exe",
                                    "vendor/gensimu/gensimu.exe"};
        char path[1200];
        for (int k = 0; k < 6 && !simb; k++) {
            snprintf(path, sizeof path, "%s%s", k < 3 ? dir : "", rel[k % 3]);
            simb = slurp(path, &sl);
        }
    }
    if (!simb || !gameb) {
        fprintf(stderr, "cannot read %s\n", !gameb ? game : sim ? sim : "gensimu.exe (use --sim)");
        return 2;
    }
    /* The SoundFont: --soundfont, or for the GM voice the shipped one if it can be found (in
     * soundfont/ or beside the executable, the source tree, then the working directory). The
     * ring-tone voice does not need one. */
    size_t sfl = 0;
    uint8_t *sfb = NULL;
    char sf_found[1200] = "";
    if (sf_path && strcmp(sf_path, "none")) {
        sfb = slurp(sf_path, &sfl);
        if (!sfb) {
            fprintf(stderr, "cannot read %s\n", sf_path);
            return 2;
        }
        snprintf(sf_found, sizeof sf_found, "%s", sf_path);
    } else if (!sf_path && voice == EXEN_VOICE_GM) {
        static const char *rel[] = {"soundfont/TimGM6mb.sf2", "TimGM6mb.sf2",
                                    "../vendor/soundfont/TimGM6mb.sf2",
                                    "vendor/soundfont/TimGM6mb.sf2"};
        for (int k = 0; k < 8 && !sfb; k++) {
            snprintf(sf_found, sizeof sf_found, "%s%s", k < 4 ? dir : "", rel[k % 4]);
            sfb = slurp(sf_found, &sfl);
        }
        if (!sfb)
            sf_found[0] = 0;
    }
    ExenGameInfo gi;
    exen_game_info(gameb, gl, &gi);
    if (sw <= 0 || sh <= 0) {
        sw = gi.width;
        sh = gi.height;
    }
    printf("title: %s (%s)\nscreen: %dx%d\n", gi.title,
           gi.match == EXEN_GAME_EXACT ? "known" : gi.match ? "known main class" : "unknown game", sw, sh);
    if (voice == EXEN_VOICE_GM || sfb)
        printf("soundfont: %s\n",
               sfb ? sf_found : "none (the General MIDI voice plays the procedural piano)");
    Inputs in = {simb, gameb, NULL, sfb, sl, gl, 0, sfl, sw, sh, wav.rate, voice};
    uint8_t *eb = NULL;
    if (eeprom) {
        eb = slurp(eeprom, &in.el);
        if (!eb) {
            fprintf(stderr, "cannot read %s\n", eeprom);
            return 2;
        }
        in.eeprom = eb;
    }
    if (self) {
        int r = selftest(&in, save_at >= 0 ? save_at : frames / 2, frames, keys, nkeys,
                         run.quiet, save_path);
        free(eb);
        free(simb);
        free(gameb);
        free(sfb);
        return r;
    }
    ExenVM *vm = make_vm(&in, &run);
    if (!vm)
        return 1;
    wav.rate = exen_audio_rate(vm);
    if (wav_path) {
        wav.f = fopen(wav_path, "wb");
        if (!wav.f) {
            fprintf(stderr, "cannot write %s\n", wav_path);
            return 2;
        }
        wav.on = 1;
        wav_header(&wav);
    }
    int rc, ticks = 0;
    if (load_path)
        rc = load_state_file(load_path, vm, &run, &wav, &ticks);
    else
        rc = exen_boot(vm);
    if (rc == 0 && save_at >= 0) {
        rc = run_ticks(vm, &run, &wav, &ticks, save_at, keys, nkeys);
        if (write_state_file(save_path, vm, &run, ticks) < 0)
            rc = -1;
    }
    if (rc == 0)
        rc = run_ticks(vm, &run, &wav, &ticks, frames, keys, nkeys);
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
    if (eeprom_out) {
        FILE *f = fopen(eeprom_out, "wb");
        int ok = f && fwrite(exen_eeprom(vm), 1, EXEN_EEPROM_SIZE, f) == EXEN_EEPROM_SIZE;
        if (f && fclose(f) != 0)
            ok = 0;
        if (ok) {
            printf("eeprom: wrote %s (%d change%s)\n", eeprom_out, run.eeprom_writes,
                   run.eeprom_writes == 1 ? "" : "s");
        } else {
            fprintf(stderr, "cannot write %s\n", eeprom_out);
            rc = -1;
        }
    }
    int halted = exen_halted(vm);
    exen_destroy(vm);
    free(eb);
    free(simb);
    free(gameb);
    free(sfb);
    return halted || rc != 0 ? 1 : 0;
}
