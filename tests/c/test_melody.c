/* Melody sequencer timing and synthesis (src/core/melody.c, docs/re/NATIVES.md §3.2).
 *
 * The original player arms a 10 ms timer, then re-arms it for 15 ms * duration units after
 * each note (tempo 120: 1875 / 120 = 15 ms per unit). At 8000 Hz one millisecond is 8 frames,
 * so every event lands on an exact frame.
 *
 * The piano voice (EXEN_VOICE_PIANO) is checked for pitch, decay and release, and for not
 * changing the sequencing: switching voices mid-note keeps every note onset, and the switch is
 * a crossfade between the two voices' own outputs. A save state round trip uses the vendored
 * catalog.exn only as the "loaded game" a state needs.
 *
 * The General MIDI voice with the vendored SoundFont (vendor/soundfont/TimGM6mb.sf2) is checked
 * for pitch, the drum kit on channel 9, note-ons on the player's exact frames, voice switches
 * and exact save state round trips (and the restart when a state cannot be restored exactly).
 * Without a SoundFont the GM voice is the procedural piano, which the piano tests cover. */
#include <math.h>
#include <stdlib.h>

#include "gm.h"
#include "platform.h"
#include "support.h"
#include "test.h"

#define RATE 8000
#define MS(x) ((x) * (RATE / 1000))
#define TOTAL MS(1000)

static int16_t pcm[2 * TOTAL];
static int rendered;

static ExenVM *audio_vm(void)
{
    ExenConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.audio_rate = RATE;
    rendered = 0;
    return exen_create(&cfg);
}

static void render_to(ExenVM *vm, int frame)
{
    if (frame > TOTAL)
        frame = TOTAL;
    if (frame > rendered) {
        int n = exen_audio_render(vm, pcm + 2 * rendered, frame - rendered);
        CHECK_EQ(n, frame - rendered);
        rendered = frame;
    }
}

static int sounding(ExenVM *vm, int note)
{
    const Melody *m = &vm->plat->melody;
    for (int i = 0; i < MELODY_VOICES; i++)
        if (m->voice[i].note == note && (m->voice[i].state == 1 || m->voice[i].state == 2))
            return 1;
    return 0;
}

static int any_voice(ExenVM *vm)
{
    const Melody *m = &vm->plat->melody;
    for (int i = 0; i < MELODY_VOICES; i++)
        if (m->voice[i].state)
            return 1;
    return 0;
}

static int peak(int from, int to)
{
    int p = 0;
    for (int f = from; f < to; f++) {
        int v = abs(pcm[2 * f]);
        if (v > p)
            p = v;
    }
    return p;
}

static int rising_crossings(int from, int to)
{
    int n = 0;
    for (int f = from + 1; f < to; f++)
        if (pcm[2 * (f - 1)] < 0 && pcm[2 * f] >= 0)
            n++;
    return n;
}

static void test_two_note_timing(void)
{
    /* A4 (69) for duration index 7 (16 units = 240 ms), C5 (72) for index 5 (32 units =
     * 480 ms), then the 0 end marker */
    static const uint8_t mel[] = {69, 7, 72, 5, 0, 0};
    ExenVM *vm = audio_vm();
    REQUIRE(vm);
    CHECK_EQ(exen_audio_rate(vm), RATE);
    CHECK(!exen_audio_active(vm));
    melody_play(vm, mel, sizeof mel);
    const Melody *m = &vm->plat->melody;
    CHECK(m->playing);
    CHECK(exen_audio_active(vm));

    /* the first timer firing is 10 ms after play */
    render_to(vm, MS(10));
    CHECK(!any_voice(vm));
    CHECK_EQ(peak(0, MS(10)), 0);
    render_to(vm, MS(10) + 1);
    CHECK(sounding(vm, 69));
    CHECK_EQ(m->period_ms, 240);

    /* the second note starts 240 ms later */
    render_to(vm, MS(250));
    CHECK(sounding(vm, 69));
    CHECK(!sounding(vm, 72));
    render_to(vm, MS(250) + 1);
    CHECK(sounding(vm, 72));
    CHECK(!sounding(vm, 69)); /* monophonic: the previous note is released */
    CHECK_EQ(m->period_ms, 480);

    /* and the end marker 480 ms after that */
    render_to(vm, MS(730));
    CHECK(m->playing);
    CHECK(sounding(vm, 72));
    render_to(vm, MS(730) + 1);
    CHECK(!m->playing);
    CHECK(!sounding(vm, 72));

    render_to(vm, TOTAL);
    /* non-silent while the notes play, at the right pitch, then silence */
    CHECK(peak(MS(20), MS(250)) > 1000);
    CHECK(peak(MS(260), MS(730)) > 1000);
    int c1 = rising_crossings(MS(10), MS(250));  /* 440 Hz * 0.24 s = 105.6 */
    int c2 = rising_crossings(MS(250), MS(730)); /* 523.25 Hz * 0.48 s = 251.2 */
    CHECK(c1 >= 100 && c1 <= 111);
    CHECK(c2 >= 244 && c2 <= 258);
    CHECK(peak(MS(800), TOTAL) <= 1);
    CHECK(!exen_audio_active(vm));
    /* balanced: both channels carry the same signal */
    int same = 1;
    for (int f = 0; f < TOTAL; f++)
        same &= pcm[2 * f] == pcm[2 * f + 1];
    CHECK(same);
    printf("  crossings %d (A4) %d (C5), peak %d\n", c1, c2, peak(0, TOTAL));
    exen_destroy(vm);
}

static void test_rest_ends_melody(void)
{
    /* 0x82 ("rest") neither advances nor re-arms the timer in the original: the melody goes
     * silent there for good, and the note after it never plays */
    static const uint8_t mel[] = {69, 7, 0x82, 5, 72, 7};
    ExenVM *vm = audio_vm();
    REQUIRE(vm);
    melody_play(vm, mel, sizeof mel);
    render_to(vm, MS(250) + 1);
    CHECK(!vm->plat->melody.playing);
    render_to(vm, TOTAL);
    CHECK(!sounding(vm, 72));
    CHECK(peak(MS(300), TOTAL) <= 1);
    exen_destroy(vm);
}

static void test_mute_keeps_time(void)
{
    static const uint8_t mel[] = {69, 7, 72, 5, 0, 0};
    ExenVM *vm = audio_vm();
    REQUIRE(vm);
    exen_audio_set_volume(vm, 0);
    melody_play(vm, mel, sizeof mel);
    render_to(vm, MS(250) + 1);
    CHECK(sounding(vm, 72)); /* the sequencer ran */
    CHECK_EQ(peak(0, MS(250)), 0);
    melody_stop(vm);
    CHECK(!vm->plat->melody.playing);
    CHECK(!sounding(vm, 72));
    exen_destroy(vm);
}

static void test_idle_render_is_silent(void)
{
    ExenVM *vm = audio_vm();
    REQUIRE(vm);
    render_to(vm, MS(100));
    CHECK_EQ(peak(0, MS(100)), 0);
    CHECK(!exen_audio_active(vm));
    exen_destroy(vm);
}

/* ------------------------------------------------------------------ piano voice */

#define HI_RATE 44100
#define HMS(x) ((x) * HI_RATE / 1000)
static int16_t hbuf[2 * HI_RATE * 2]; /* 2 s at 44.1 kHz */

static ExenVM *rate_vm(int rate, int voice)
{
    ExenConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.audio_rate = rate;
    ExenVM *vm = exen_create(&cfg);
    if (vm)
        exen_audio_set_voice(vm, voice);
    return vm;
}

/* render `frames` frames into b in host-sized chunks */
static void render_chunks(ExenVM *vm, int16_t *b, int frames)
{
    for (int done = 0; done < frames;) {
        int n = frames - done > 735 ? 735 : frames - done;
        exen_audio_render(vm, b + 2 * done, n);
        done += n;
    }
}

static double rms_of(const int16_t *b, int from, int to)
{
    double s = 0.0;
    for (int f = from; f < to; f++)
        s += (double)b[2 * f] * b[2 * f];
    return to > from ? sqrt(s / (to - from)) : 0.0;
}

static int peak_of(const int16_t *b, int from, int to)
{
    int p = 0;
    for (int f = from; f < to; f++) {
        int v = abs(b[2 * f]);
        if (v > p)
            p = v;
    }
    return p;
}

/* normalised autocorrelation of the left channel over [from, from + len) at lag `lag` */
static double acorr(const int16_t *b, int from, int len, int lag)
{
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (int i = 0; i < len; i++) {
        double x = b[2 * (from + i)], y = b[2 * (from + i + lag)];
        xy += x * y;
        xx += x * x;
        yy += y * y;
    }
    return xx > 0.0 && yy > 0.0 ? xy / sqrt(xx * yy) : 0.0;
}

/* The lag in [lo, hi] with the highest autocorrelation, refined by a parabola. */
static double best_period(const int16_t *b, int from, int len, int lo, int hi, double *corr)
{
    int best = lo;
    double bc = -2.0, cm = 0.0, cp = 0.0;
    for (int l = lo; l <= hi; l++) {
        double c = acorr(b, from, len, l);
        if (c > bc) {
            bc = c;
            best = l;
        }
    }
    if (best > lo && best < hi) {
        cm = acorr(b, from, len, best - 1);
        cp = acorr(b, from, len, best + 1);
    }
    *corr = bc;
    double den = cm - 2.0 * bc + cp;
    return best + (den < 0.0 ? 0.5 * (cm - cp) / den : 0.0);
}

/* power of the left channel at frequency f (Goertzel), relative to the total power */
static double tone_share(const int16_t *b, int from, int to, double f, int rate)
{
    double w = 2.0 * 3.14159265358979 * f / rate, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0, tot = 0.0;
    for (int i = from; i < to; i++) {
        double x = b[2 * i];
        double s0 = x + c * s1 - s2;
        s2 = s1;
        s1 = s0;
        tot += x * x;
    }
    double pw = s1 * s1 + s2 * s2 - c * s1 * s2; /* |X(f)|^2 */
    return tot > 0.0 ? 2.0 * pw / ((double)(to - from) * tot) : 0.0;
}

static void test_piano_pitch(void)
{
    /* a whole note (index 1: 128 units = 1920 ms), from the lowest note of the corpus (E1) up
     * to its highest (D5); the period must be within 0.3 % of the note's */
    static const struct { uint8_t note; double hz; int fundamental; } notes[] = {
        {69, 440.0, 1}, {74, 587.330, 1}, {60, 261.626, 1}, {40, 82.407, 0}, {28, 41.203, 0},
    };
    for (size_t i = 0; i < sizeof notes / sizeof notes[0]; i++) {
        ExenVM *vm = rate_vm(HI_RATE, EXEN_VOICE_PIANO);
        REQUIRE(vm);
        CHECK_EQ(exen_audio_voice(vm), EXEN_VOICE_PIANO);
        const uint8_t mel[] = {notes[i].note, 1, 0, 0};
        melody_play(vm, mel, sizeof mel);
        int n = HI_RATE; /* 1 s */
        render_chunks(vm, hbuf, n);
        double T = HI_RATE / notes[i].hz, corr;
        int from = HI_RATE / 10, len = HI_RATE / 2;
        double p = best_period(hbuf, from, len, (int)(0.75 * T), (int)(1.3 * T) + 1, &corr);
        CHECK(peak_of(hbuf, from, from + len) > 2000);
        CHECK(fabs(p / T - 1.0) < 0.003);
        CHECK(corr > 0.6);
        double share = tone_share(hbuf, from, from + len, notes[i].hz, HI_RATE);
        /* for the treble and the middle the fundamental is strong; the soundboard makes bass
         * notes mostly overtones, as on a real piano */
        if (notes[i].fundamental)
            CHECK(share > 0.15);
        printf("  piano note %d: period %.2f (want %.2f), corr %.3f, fundamental %.0f %%\n",
               notes[i].note, p, T, corr, 100.0 * share);
        exen_destroy(vm);
    }
}

static void test_piano_decay_and_release(void)
{
    ExenVM *vm = rate_vm(HI_RATE, EXEN_VOICE_PIANO);
    REQUIRE(vm);
    /* held: A4 for a whole note (1920 ms) */
    static const uint8_t held[] = {69, 1, 0, 0};
    melody_play(vm, held, sizeof held);
    render_chunks(vm, hbuf, 2 * HI_RATE);
    double early = rms_of(hbuf, HMS(20), HMS(120)), mid = rms_of(hbuf, HMS(700), HMS(800)),
           late = rms_of(hbuf, HMS(1500), HMS(1600));
    CHECK(early > 1.5 * mid);
    CHECK(mid > 1.2 * late);
    CHECK(late > 50.0); /* still ringing while held */
    /* the release after the end of the note at 1930 ms: quiet soon, silent within ~0.6 s */
    CHECK(rms_of(hbuf, HMS(1960), HMS(1990)) < 0.5 * rms_of(hbuf, HMS(1890), HMS(1920)));
    printf("  piano decay: rms %.0f -> %.0f -> %.0f\n", early, mid, late);
    exen_destroy(vm);

    /* legato: the old note rings out briefly under the next one, then everything frees */
    vm = rate_vm(HI_RATE, EXEN_VOICE_PIANO);
    REQUIRE(vm);
    static const uint8_t two[] = {69, 7, 72, 7, 0, 0}; /* 240 ms each */
    melody_play(vm, two, sizeof two);
    render_chunks(vm, hbuf, HMS(250) + 10);
    const Melody *m = &vm->plat->melody;
    int old_tail = 0, new_note = 0;
    for (int i = 0; i < PIANO_VOICES; i++) {
        old_tail |= m->piano[i].note == 69 && m->piano[i].state == 2;
        new_note |= m->piano[i].note == 72 && m->piano[i].state == 1;
    }
    CHECK(old_tail);
    CHECK(new_note);
    render_chunks(vm, hbuf, HI_RATE + HI_RATE / 2);
    CHECK(!exen_audio_active(vm));
    CHECK_EQ(peak_of(hbuf, HI_RATE, HI_RATE + HI_RATE / 2), 0);
    for (int i = 0; i < PIANO_VOICES; i++)
        CHECK_EQ(m->piano[i].state, 0);
    exen_destroy(vm);
}

/* The sequencer and both voice pools, frame by frame; must not depend on the voice. */
typedef struct {
    uint32_t pos, events;
    int32_t period_ms;
    int64_t wait;
    uint8_t playing, ch, cur_dur;
    uint8_t vs[MELODY_VOICES], vn[MELODY_VOICES], ps[PIANO_VOICES], pn[PIANO_VOICES];
} Snap;

static Snap snap(ExenVM *vm)
{
    const Melody *m = &vm->plat->melody;
    Snap s;
    memset(&s, 0, sizeof s);
    s.pos = m->pos;
    s.events = m->events;
    s.period_ms = m->period_ms;
    s.wait = m->wait;
    s.playing = m->playing;
    s.ch = m->ch;
    s.cur_dur = m->cur_dur;
    for (int i = 0; i < MELODY_VOICES; i++) {
        s.vs[i] = m->voice[i].state;
        s.vn[i] = m->voice[i].note;
    }
    for (int i = 0; i < PIANO_VOICES; i++) {
        s.ps[i] = m->piano[i].state;
        s.pn[i] = m->piano[i].note;
    }
    return s;
}

static int16_t ring_pcm[2 * TOTAL], piano_pcm[2 * TOTAL], sw_pcm[2 * TOTAL];
static float sw_mix[TOTAL];

static void test_voice_switch_keeps_time(void)
{
    /* 1/16, 1/16, 1/8, dotted 1/16, end: notes at 10, 130, 250 and 490 ms, end at 670 ms */
    static const uint8_t mel[] = {69, 9, 72, 9, 76, 7, 74, 8, 0, 0};
    ExenVM *ring = rate_vm(RATE, EXEN_VOICE_RINGTONE), *piano = rate_vm(RATE, EXEN_VOICE_PIANO),
           *sw = rate_vm(RATE, EXEN_VOICE_RINGTONE);
    REQUIRE(ring && piano && sw);
    melody_play(ring, mel, sizeof mel);
    melody_play(piano, mel, sizeof mel);
    melody_play(sw, mel, sizeof mel);
    const int to_piano = MS(70) + 3, to_ring = MS(330) + 5; /* both in the middle of a note */
    int same_seq = 1, onsets = 0;
    Snap prev = snap(ring);
    for (int f = 0; f < TOTAL; f++) {
        if (f == to_piano)
            exen_audio_set_voice(sw, EXEN_VOICE_PIANO);
        if (f == to_ring)
            exen_audio_set_voice(sw, EXEN_VOICE_RINGTONE);
        exen_audio_render(ring, ring_pcm + 2 * f, 1);
        exen_audio_render(piano, piano_pcm + 2 * f, 1);
        exen_audio_render(sw, sw_pcm + 2 * f, 1);
        sw_mix[f] = sw->plat->melody.mix;
        Snap a = snap(ring), b = snap(piano), c = snap(sw);
        same_seq &= !memcmp(&a, &b, sizeof a) && !memcmp(&a, &c, sizeof a);
        onsets += a.events != prev.events;
        prev = a;
    }
    CHECK(same_seq);
    CHECK_EQ(onsets, 5); /* four notes and the end */
    /* the ring tone until the switch; then a crossfade (no click: it stays between the two
     * voices' own outputs), the piano, and back */
    int fade = (int)(0.030 * RATE) + 1, settle = 50, exact = 1, close = 1, differs = 0;
    for (int f = 0; f < TOTAL; f++) {
        int r = ring_pcm[2 * f], p = piano_pcm[2 * f], s = sw_pcm[2 * f];
        if (f < to_piano) {
            exact &= s == r;
        } else if (f < to_piano + fade || (f >= to_ring && f < to_ring + fade)) {
            double want = (1.0 - sw_mix[f]) * r + sw_mix[f] * p;
            if (fabs(s - want) > 100.0) {
                if (close)
                    printf("  frame %d: %d, want %.0f (ring %d, piano %d, mix %.3f)\n", f, s,
                           want, r, p, sw_mix[f]);
                close = 0;
            }
        } else if (f >= to_piano + fade + settle && f < to_ring) {
            close &= abs(s - p) <= 1;
            differs |= abs(p - r) > 1000;
        } else if (f >= to_ring + fade + settle) {
            close &= abs(s - r) <= 1;
        }
    }
    CHECK(exact);
    CHECK(close);
    CHECK(differs); /* the switch did change the sound */
    CHECK(sw_mix[to_piano - 1] == 0.0f && sw_mix[to_piano + fade] == 1.0f);
    CHECK(sw_mix[to_ring + fade] == 0.0f);
    exen_destroy(ring);
    exen_destroy(piano);
    exen_destroy(sw);
}

/* ------------------------------------------------------------------ save states */

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    return c ^ 0xffffffffu;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* A copy of the state without its `tag` chunk (PIAN: as a core from before the piano voice
 * wrote it; GMSF: as a VM without a SoundFont wrote it). NULL if there was none. */
static uint8_t *strip_chunk(const uint8_t *st, size_t n, const char *tag, size_t *out_n)
{
    uint8_t *o = malloc(n);
    if (!o)
        return NULL;
    memcpy(o, st, 40);
    size_t w = 40, r = 40, end = 40 + get32(st + 28);
    int found = 0;
    while (r + 8 <= end) {
        uint32_t len = get32(st + r + 4);
        if (!memcmp(st + r, tag, 4))
            found = 1;
        else {
            memcpy(o + w, st + r, 8 + len);
            w += 8 + len;
        }
        r += 8 + len;
    }
    put32(o + 28, (uint32_t)(w - 40));
    put32(o + 32, crc32_of(o + 40, w - 40));
    *out_n = w;
    if (!found) {
        free(o);
        return NULL;
    }
    return o;
}

static ExenVM *game_vm(const uint8_t *game, size_t gl, int voice)
{
    ExenConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.audio_rate = 22050;
    ExenVM *vm = exen_create(&cfg);
    size_t len;
    const uint8_t *exe = t_gensimu(&len);
    if (!vm || exen_load_framework(vm, exe, len) < 0 || exen_load_game(vm, game, gl) < 0) {
        exen_destroy(vm);
        return NULL;
    }
    exen_audio_set_voice(vm, voice);
    return vm;
}

static void test_piano_state_roundtrip(void)
{
    size_t gl;
    uint8_t *game = t_read_file(t_source_path("vendor/gensimu/flash/catalog.exn"), &gl);
    REQUIRE(game);
    static int16_t a[2 * 22050], b[2 * 22050];
    static const uint8_t mel[] = {57, 7, 60, 8, 64, 9, 62, 7, 0, 0};
    ExenVM *va = game_vm(game, gl, EXEN_VOICE_PIANO);
    REQUIRE(va);
    melody_play(va, mel, sizeof mel);
    render_chunks(va, a, 22050 * 3 / 10 + 17); /* mid-note, with a release tail sounding */
    size_t n = exen_state_size(va);
    uint8_t *st = malloc(n);
    REQUIRE(st);
    CHECK_EQ(exen_state_save(va, st, n), (long long)n);
    render_chunks(va, a, 22050);

    /* the same audio after loading */
    ExenVM *vb = game_vm(game, gl, EXEN_VOICE_PIANO);
    REQUIRE(vb);
    CHECK_EQ(exen_state_load(vb, st, n), 0);
    render_chunks(vb, b, 22050);
    CHECK(!memcmp(a, b, sizeof a));
    CHECK(peak_of(a, 0, 22050 / 4) > 1000);

    /* a state without the piano chunk (older cores) loads; its piano notes are silent. The
     * voice is a host setting: a state saved with the piano does not select it. */
    size_t on;
    uint8_t *old = strip_chunk(st, n, "PIAN", &on);
    REQUIRE(old);
    ExenVM *vc = game_vm(game, gl, EXEN_VOICE_RINGTONE);
    REQUIRE(vc);
    CHECK_EQ(exen_state_load(vc, old, on), 0);
    CHECK_EQ(exen_audio_voice(vc), EXEN_VOICE_RINGTONE);
    int sounding_piano = 0;
    for (int i = 0; i < PIANO_VOICES; i++)
        sounding_piano |= vc->plat->melody.piano[i].state != 0;
    CHECK(!sounding_piano);
    CHECK(vc->plat->melody.playing);
    exen_destroy(va);
    exen_destroy(vb);
    exen_destroy(vc);
    free(st);
    free(old);
    free(game);
}

/* ------------------------------------------------------------------ General MIDI (SoundFont) */

/* The vendored SoundFont (vendor/soundfont/TimGM6mb.sf2), read once. */
static uint8_t *sf2_data;
static size_t sf2_len;

static int load_sf2(void)
{
    if (!sf2_data)
        sf2_data = t_read_file(t_source_path("vendor/soundfont/TimGM6mb.sf2"), &sf2_len);
    return sf2_data != NULL;
}

static ExenVM *gm_vm(int rate, int voice)
{
    ExenVM *vm = rate_vm(rate, voice);
    if (vm && exen_audio_set_soundfont(vm, sf2_data, sf2_len) < 0) {
        exen_destroy(vm);
        return NULL;
    }
    return vm;
}

static uint32_t gm_note_count(ExenVM *vm)
{
    const GmSynth *g = vm->plat->melody.gm;
    return g ? gm_notes(g) : 0;
}

static void test_gm_soundfont_api(void)
{
    REQUIRE(load_sf2());
    ExenVM *vm = rate_vm(HI_RATE, EXEN_VOICE_GM);
    REQUIRE(vm);
    CHECK_EQ(EXEN_VOICE_PIANO, EXEN_VOICE_GM); /* the older name of the same voice */
    CHECK(!exen_audio_soundfont(vm));
    static const uint8_t junk[64] = {'R', 'I', 'F', 'F', 56, 0, 0, 0, 's', 'f', 'b', 'k'};
    CHECK(exen_audio_set_soundfont(vm, junk, sizeof junk) < 0);
    CHECK(!exen_audio_soundfont(vm));
    CHECK_EQ(exen_audio_set_soundfont(vm, sf2_data, sf2_len), 0);
    CHECK(exen_audio_soundfont(vm));
    CHECK(exen_audio_set_soundfont(vm, junk, sizeof junk) < 0); /* the loaded one stays */
    CHECK(exen_audio_soundfont(vm));
    CHECK(exen_audio_set_soundfont(vm, sf2_data, 1000) < 0);    /* truncated */
    CHECK(exen_audio_soundfont(vm));
    CHECK_EQ(exen_audio_set_soundfont(vm, NULL, 0), 0);         /* unload */
    CHECK(!exen_audio_soundfont(vm));
    exen_destroy(vm);
}

static void test_gm_pitch(void)
{
    /* GM program 0 at the corpus range (E1..D5) and the middle: a whole note each, period
     * within 0.5 % (9 cents) of the note's */
    static const struct { uint8_t note; double hz; } notes[] = {
        {69, 440.0}, {74, 587.330}, {60, 261.626}, {48, 130.813}, {40, 82.407}, {28, 41.203},
    };
    REQUIRE(load_sf2());
    for (size_t i = 0; i < sizeof notes / sizeof notes[0]; i++) {
        ExenVM *vm = gm_vm(HI_RATE, EXEN_VOICE_GM);
        REQUIRE(vm);
        const uint8_t mel[] = {notes[i].note, 1, 0, 0};
        melody_play(vm, mel, sizeof mel);
        render_chunks(vm, hbuf, HI_RATE);
        double T = HI_RATE / notes[i].hz, corr;
        int from = HI_RATE / 10, len = HI_RATE / 2;
        double p = best_period(hbuf, from, len, (int)(0.75 * T), (int)(1.3 * T) + 1, &corr);
        CHECK(peak_of(hbuf, from, from + len) > 1000);
        CHECK(fabs(p / T - 1.0) < 0.005);
        CHECK(corr > 0.6);
        printf("  GM note %d: period %.2f (want %.2f), corr %.3f, rms %.0f\n", notes[i].note, p,
               T, corr, rms_of(hbuf, from, from + len));
        exen_destroy(vm);
    }
}

/* highest normalised autocorrelation over lags [lo, hi] */
static double max_acorr(const int16_t *b, int from, int len, int lo, int hi)
{
    double best = -1.0;
    for (int l = lo; l <= hi; l++) {
        double c = acorr(b, from, len, l);
        if (c > best)
            best = c;
    }
    return best;
}

static void test_gm_drums(void)
{
    REQUIRE(load_sf2());
    /* 0x81 0: drums (channel 9, program 0). Note 42 is the closed hi-hat in GM; on channel 0
     * the same note is F#2 on the piano. */
    static const uint8_t drums[] = {0x81, 0, 42, 7, 42, 7, 0, 0};
    static const uint8_t piano[] = {42, 7, 42, 7, 0, 0};
    ExenVM *vd = gm_vm(HI_RATE, EXEN_VOICE_GM), *vp = gm_vm(HI_RATE, EXEN_VOICE_GM);
    REQUIRE(vd && vp);
    static int16_t pb[2 * HI_RATE];
    melody_play(vd, drums, sizeof drums);
    melody_play(vp, piano, sizeof piano);
    render_chunks(vd, hbuf, HI_RATE / 2);
    render_chunks(vp, pb, HI_RATE / 2);
    CHECK_EQ(vd->plat->melody.ch, 9);
    /* 20 ms from 5 ms after the first hit: loud, and noise (no period between 0.2 and 15 ms),
     * where the piano's F#2 is periodic */
    int from = HMS(15), len = HMS(20);
    double dn = max_acorr(hbuf, from, len, HMS(1) / 5, HMS(15));
    double pn = max_acorr(pb, from, len, HMS(1) / 5, HMS(15));
    CHECK(rms_of(hbuf, from, from + len) > 300.0);
    CHECK(dn < 0.5);
    CHECK(pn > 0.8);
    /* the second hit, 240 ms later, sounds again */
    CHECK(rms_of(hbuf, HMS(255), HMS(275)) > 300.0);
    printf("  GM drums: hi-hat rms %.0f, max autocorrelation %.2f (piano note: %.2f)\n",
           rms_of(hbuf, from, from + len), dn, pn);
    exen_destroy(vd);
    exen_destroy(vp);
}

/* 0x81 b (b = 1..0x80) changes the channel's program: the same note on GM program 73 (flute)
 * has the same pitch and another timbre; melody stop resets the programs to the piano. */
static void test_gm_program_change(void)
{
    REQUIRE(load_sf2());
    static const uint8_t flute[] = {0x81, 73, 69, 3, 0, 0};
    static const uint8_t piano[] = {69, 3, 0, 0};
    static int16_t pb[2 * HI_RATE];
    ExenVM *vf = gm_vm(HI_RATE, EXEN_VOICE_GM), *vp = gm_vm(HI_RATE, EXEN_VOICE_GM);
    REQUIRE(vf && vp);
    melody_play(vf, flute, sizeof flute);
    melody_play(vp, piano, sizeof piano);
    CHECK_EQ(vf->plat->melody.program[0], 0);
    render_chunks(vf, hbuf, HI_RATE / 2);
    render_chunks(vp, pb, HI_RATE / 2);
    CHECK_EQ(vf->plat->melody.program[0], 73);
    double T = HI_RATE / 440.0, corr;
    int from = HMS(150), len = HMS(250);
    double p = best_period(hbuf, from, len, (int)(0.75 * T), (int)(1.3 * T) + 1, &corr);
    CHECK(fabs(p / T - 1.0) < 0.005);
    /* the piano's overtones decay, the flute's tone does not: compare the second harmonic */
    double hf = tone_share(hbuf, from, from + len, 880.0, HI_RATE);
    double hp = tone_share(pb, from, from + len, 880.0, HI_RATE);
    CHECK(fabs(hf - hp) > 0.02);
    printf("  GM program 73: period %.2f (want %.2f), 2nd harmonic %.0f %% (piano %.0f %%)\n", p,
           T, 100.0 * hf, 100.0 * hp);
    melody_stop(vf);
    CHECK_EQ(vf->plat->melody.program[0], 0);
    exen_destroy(vf);
    exen_destroy(vp);
}

/* Note-ons reach the SoundFont synth on the player's exact frames (10 ms after the start, then
 * 15 ms * duration units), with silence before the first one. */
static void test_gm_onsets_exact(void)
{
    REQUIRE(load_sf2());
    static const uint8_t mel[] = {69, 9, 72, 9, 76, 7, 74, 8, 0, 0};
    static const int want[] = {MS(10), MS(130), MS(250), MS(490)};
    ExenVM *vm = gm_vm(RATE, EXEN_VOICE_GM);
    REQUIRE(vm);
    melody_play(vm, mel, sizeof mel);
    int got[8], n = 0;
    uint32_t prev = gm_note_count(vm);
    for (int f = 0; f < TOTAL; f++) {
        exen_audio_render(vm, pcm + 2 * f, 1);
        uint32_t c = gm_note_count(vm);
        if (c != prev && n < 8)
            got[n++] = f;
        prev = c;
    }
    CHECK_EQ(n, 4);
    for (int i = 0; i < n && i < 4; i++)
        CHECK_EQ(got[i], want[i]);
    /* silent before the first note; audible within 10 ms (the sample's own attack: TimGM6mb's
     * A4 starts with about 3 ms of near silence) */
    CHECK_EQ(peak(0, MS(10)), 0);
    CHECK(peak(MS(10), MS(20)) > 500);
    /* the end marker at 670 ms releases the last note (TimGM6mb's piano: a 1 s release) */
    CHECK(peak(MS(900), MS(1000)) < peak(MS(500), MS(600)) / 10);
    exen_destroy(vm);
}

/* The GM voice runs beside the ring tone like the piano did: switching keeps the sequencer and
 * every note's timing, and with the ring tone selected a loaded SoundFont changes nothing. */
static int16_t gm_pcm[2 * TOTAL], ring_nosf_pcm[2 * TOTAL];

static void test_gm_voice_switch_keeps_time(void)
{
    REQUIRE(load_sf2());
    static const uint8_t mel[] = {69, 9, 72, 9, 76, 7, 74, 8, 0, 0};
    ExenVM *ring = gm_vm(RATE, EXEN_VOICE_RINGTONE), *gm = gm_vm(RATE, EXEN_VOICE_GM),
           *sw = gm_vm(RATE, EXEN_VOICE_RINGTONE), *plain = rate_vm(RATE, EXEN_VOICE_RINGTONE);
    REQUIRE(ring && gm && sw && plain);
    melody_play(ring, mel, sizeof mel);
    melody_play(gm, mel, sizeof mel);
    melody_play(sw, mel, sizeof mel);
    melody_play(plain, mel, sizeof mel);
    const int to_gm = MS(70) + 3, to_ring = MS(330) + 5; /* both in the middle of a note */
    int same_seq = 1, same_notes = 1;
    for (int f = 0; f < TOTAL; f++) {
        if (f == to_gm)
            exen_audio_set_voice(sw, EXEN_VOICE_GM);
        if (f == to_ring)
            exen_audio_set_voice(sw, EXEN_VOICE_RINGTONE);
        exen_audio_render(ring, ring_pcm + 2 * f, 1);
        exen_audio_render(gm, gm_pcm + 2 * f, 1);
        exen_audio_render(sw, sw_pcm + 2 * f, 1);
        exen_audio_render(plain, ring_nosf_pcm + 2 * f, 1);
        sw_mix[f] = sw->plat->melody.mix;
        Snap a = snap(ring), b = snap(gm), c = snap(sw), d = snap(plain);
        same_seq &= !memcmp(&a, &b, sizeof a) && !memcmp(&a, &c, sizeof a) &&
                    !memcmp(&a, &d, sizeof a);
        same_notes &= gm_note_count(ring) == gm_note_count(gm) &&
                      gm_note_count(sw) == gm_note_count(gm);
    }
    CHECK(same_seq);
    CHECK(same_notes);
    CHECK_EQ(gm_note_count(gm), 4);
    /* a loaded SoundFont leaves the ring tone bit-identical */
    CHECK(!memcmp(ring_pcm, ring_nosf_pcm, sizeof ring_pcm));
    int fade = (int)(0.030 * RATE) + 1, settle = 50, exact = 1, close = 1, differs = 0;
    for (int f = 0; f < TOTAL; f++) {
        int r = ring_pcm[2 * f], g = gm_pcm[2 * f], s = sw_pcm[2 * f];
        if (f < to_gm) {
            exact &= s == r;
        } else if (f < to_gm + fade || (f >= to_ring && f < to_ring + fade)) {
            double want = (1.0 - sw_mix[f]) * r + sw_mix[f] * g;
            if (fabs(s - want) > 100.0)
                close = 0;
        } else if (f >= to_gm + fade + settle && f < to_ring) {
            close &= abs(s - g) <= 1;
            differs |= abs(g - r) > 1000;
        } else if (f >= to_ring + fade + settle) {
            close &= abs(s - r) <= 1;
        }
    }
    CHECK(exact);
    CHECK(close);
    CHECK(differs);
    exen_destroy(ring);
    exen_destroy(gm);
    exen_destroy(sw);
    exen_destroy(plain);
}

/* The chunk `tag` of a state, or NULL. */
static uint8_t *find_chunk(uint8_t *st, size_t n, const char *tag, uint32_t *len)
{
    size_t r = 40, end = 40 + get32(st + 28);
    (void)n;
    while (r + 8 <= end) {
        uint32_t l = get32(st + r + 4);
        if (!memcmp(st + r, tag, 4)) {
            *len = l;
            return st + r + 8;
        }
        r += 8 + l;
    }
    return NULL;
}

static void test_gm_state_roundtrip(void)
{
    REQUIRE(load_sf2());
    size_t gl;
    uint8_t *game = t_read_file(t_source_path("vendor/gensimu/flash/catalog.exn"), &gl);
    REQUIRE(game);
    static int16_t a[2 * 22050], b[2 * 22050];
    static const uint8_t mel[] = {57, 7, 60, 8, 64, 9, 62, 7, 0, 0};
    ExenVM *va = game_vm(game, gl, EXEN_VOICE_GM);
    REQUIRE(va);
    CHECK_EQ(exen_audio_set_soundfont(va, sf2_data, sf2_len), 0);
    melody_play(va, mel, sizeof mel);
    render_chunks(va, a, 22050 * 3 / 10 + 17); /* mid-note, with a release tail sounding */
    size_t n = exen_state_size(va);
    uint8_t *st = malloc(n);
    REQUIRE(st);
    CHECK_EQ(exen_state_save(va, st, n), (long long)n);
    uint32_t glen = 0;
    CHECK(find_chunk(st, n, "GMSF", &glen) != NULL);
    CHECK_EQ(glen, GM_STATE_BYTES);
    CHECK(n <= exen_state_max_size(va));
    render_chunks(va, a, 22050);

    /* the same SoundFont and rate: exactly the same audio */
    ExenVM *vb = game_vm(game, gl, EXEN_VOICE_GM);
    REQUIRE(vb);
    CHECK_EQ(exen_audio_set_soundfont(vb, sf2_data, sf2_len), 0);
    CHECK_EQ(exen_state_load(vb, st, n), 0);
    render_chunks(vb, b, 22050);
    CHECK(!memcmp(a, b, sizeof a));
    CHECK(peak_of(a, 0, 22050 / 4) > 1000);

    /* no SoundFont in the loading VM: the state loads, the GM voice is the procedural piano */
    ExenVM *vc = game_vm(game, gl, EXEN_VOICE_GM);
    REQUIRE(vc);
    CHECK_EQ(exen_state_load(vc, st, n), 0);
    CHECK(vc->plat->melody.playing);
    render_chunks(vc, b, 22050);
    CHECK(peak_of(b, 0, 22050 / 4) > 1000);

    /* a state without the GMSF chunk, and one whose GMSF record is damaged: the SoundFont
     * voice restarts with the current note struck again */
    size_t on;
    uint8_t *old = strip_chunk(st, n, "GMSF", &on);
    REQUIRE(old);
    uint8_t *bad = malloc(n);
    REQUIRE(bad);
    memcpy(bad, st, n);
    uint8_t *rec = find_chunk(bad, n, "GMSF", &glen);
    REQUIRE(rec);
    for (int i = 0; i < GM_VOICES; i++) /* every voice's region index out of range */
        if (get32(rec + 24 + 264 * i) != 0xffffffffu)
            put32(rec + 24 + 264 * i + 16, 0x7fffffffu);
    put32(bad + 32, crc32_of(bad + 40, get32(bad + 28)));
    const uint8_t *variants[] = {old, bad};
    const size_t lens[] = {on, n};
    for (int k = 0; k < 2; k++) {
        ExenVM *vd = game_vm(game, gl, EXEN_VOICE_GM);
        REQUIRE(vd);
        CHECK_EQ(exen_audio_set_soundfont(vd, sf2_data, sf2_len), 0);
        CHECK_EQ(exen_state_load(vd, variants[k], lens[k]), 0);
        CHECK(gm_active(vd->plat->melody.gm));
        CHECK_EQ(gm_note_count(vd), 1); /* the sounding note, struck again */
        render_chunks(vd, b, 22050);
        CHECK(peak_of(b, 0, 22050 / 10) > 1000);
        exen_destroy(vd);
    }

    /* another output rate: the record does not apply, the voice restarts */
    {
        ExenConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.audio_rate = 44100;
        ExenVM *ve = exen_create(&cfg);
        size_t len;
        const uint8_t *exe = t_gensimu(&len);
        REQUIRE(ve);
        CHECK(exen_load_framework(ve, exe, len) == 0 && exen_load_game(ve, game, gl) == 0);
        CHECK_EQ(exen_audio_set_soundfont(ve, sf2_data, sf2_len), 0);
        CHECK_EQ(exen_state_load(ve, st, n), 0);
        CHECK_EQ(gm_note_count(ve), 1);
        exen_destroy(ve);
    }
    exen_destroy(va);
    exen_destroy(vb);
    exen_destroy(vc);
    free(st);
    free(old);
    free(bad);
    free(game);
}

int main(void)
{
    RUN(test_two_note_timing);
    RUN(test_rest_ends_melody);
    RUN(test_mute_keeps_time);
    RUN(test_idle_render_is_silent);
    RUN(test_piano_pitch);
    RUN(test_piano_decay_and_release);
    RUN(test_voice_switch_keeps_time);
    RUN(test_piano_state_roundtrip);
    RUN(test_gm_soundfont_api);
    RUN(test_gm_pitch);
    RUN(test_gm_drums);
    RUN(test_gm_program_change);
    RUN(test_gm_onsets_exact);
    RUN(test_gm_voice_switch_keeps_time);
    RUN(test_gm_state_roundtrip);
    free(sf2_data);
    TEST_MAIN_END();
}
