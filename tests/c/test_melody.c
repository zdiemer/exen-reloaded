/* Melody sequencer timing and synthesis (src/core/melody.c, docs/re/NATIVES.md §3.2).
 *
 * The original player arms a 10 ms timer, then re-arms it for 15 ms * duration units after
 * each note (tempo 120: 1875 / 120 = 15 ms per unit). At 8000 Hz one millisecond is 8 frames,
 * so every event lands on an exact frame.
 *
 * The piano voice (EXEN_VOICE_PIANO) is checked for pitch, decay and release, and for not
 * changing the sequencing: switching voices mid-note keeps every note onset, and the switch is
 * a crossfade between the two voices' own outputs. A save state round trip uses the vendored
 * catalog.exn only as the "loaded game" a state needs. */
#include <math.h>
#include <stdlib.h>

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

/* A copy of the state without its PIAN chunk, as a core from before the piano voice wrote it. */
static uint8_t *strip_piano(const uint8_t *st, size_t n, size_t *out_n)
{
    uint8_t *o = malloc(n);
    if (!o)
        return NULL;
    memcpy(o, st, 40);
    size_t w = 40, r = 40, end = 40 + get32(st + 28);
    int found = 0;
    while (r + 8 <= end) {
        uint32_t len = get32(st + r + 4);
        if (!memcmp(st + r, "PIAN", 4))
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
    uint8_t *old = strip_piano(st, n, &on);
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
    TEST_MAIN_END();
}
