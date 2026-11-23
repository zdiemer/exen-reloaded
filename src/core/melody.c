/* Melody sequencer and synthesizer. See melody.h and docs/re/NATIVES.md §3.2.
 *
 * Sequencer: a step-by-step port of the original timer proc (0x43ad57). The original arms a
 * periodic Win32 timer (SetTimer id 10) and every firing handles one event; commands chain
 * into the next pair in the same firing. Here the "timer" is a countdown on the audio clock.
 *
 * Synthesizer: the original sends the notes to the default MIDI device (General MIDI piano at
 * velocity 64). The core renders them itself, with voice pools that always run side by side
 * (every note event goes to each pool) while the host setting picks what is heard:
 *
 * - Ring tone (default): a PolyBLEP band-limited square wave with a 3 ms attack, a gentle
 *   decay to 60 % and a 12 ms linear release, through a one-pole low-pass.
 * - General MIDI, with a SoundFont (exen_audio_set_soundfont): the MIDI events the original
 *   sent (note-on at velocity 64, note-off, program changes, drums on channel 9) played by
 *   TinySoundFont (gm.c). The 0x84 / 0x85 device volume is the output gain, as for the others.
 * - General MIDI without a SoundFont: a procedural piano, an approximation of the GM acoustic
 *   grand (program 0) as Windows' synth played it, so the setting still sounds like a piano.
 *   Additive: up to PIANO_PARTIALS slightly inharmonic partials (f_n = n f0 sqrt(1 + B n^2)),
 *   each on two strings detuned by +-0.7 cents (slow beating, the "chorus" of a piano
 *   unison). Partial levels follow a struck string: |sin(pi n x0)| / n for a hammer at 1/8.3
 *   of the string, a felt low-pass (a medium blow; the velocity is always 64) and a soundboard
 *   that radiates little below ~120 Hz, so bass notes are mostly overtones. Each partial
 *   decays in two stages (prompt sound, then aftersound), faster for higher partials and
 *   higher notes. Note-off drops the damper: an exponential release of 40-120 ms, so legato
 *   note changes overlap briefly as on a MIDI synth rather than cutting. A 1.5 ms attack ramp
 *   and a short band-passed noise burst give the hammer knock. Partials stop below
 *   min(0.4 * rate, 10 kHz) with a taper, so nothing aliases, and every note is normalised to
 *   the same loudness. Program changes are tracked but every program plays the piano.
 *
 * Channel 9 (selected by program change 0) plays the SoundFont's drum kit; in the ring tone
 * and the procedural piano it plays filtered noise bursts. The player is monophonic (each
 * event first turns the previous note off), so a few voices per pool cover the overlapping
 * release tails. A voice switch crossfades over MIX_S; every pool keeps running either way, so
 * the sequencer and the note state never depend on the voice. Only the oscillators of an
 * unheard procedural pool are skipped (its phases and levels still advance); the SoundFont
 * synth always renders while it has voices.
 *
 * The ring tone and the procedural piano use no maths library (m_exp, m_sqrt and a polynomial
 * sine are here), so they are bit-identical on every platform. TinySoundFont uses libm.
 */
#include <string.h>

#include "gm.h"
#include "platform.h"

/* Duration table (on the stack of 0x43ad57): an index b % 14 gives units of 1/32 beat.
 * Even indices are dotted values: 0xc0 dotted whole, 0x80 whole, ... 2 = 1/64. */
static const uint8_t DUR[14] = {0xc0, 0x80, 0x60, 0x40, 0x30, 0x20, 0x18,
                                0x10, 0x0c, 8,    6,    4,    3,    2};
/* [0x45caac] = 120 and nothing writes it, so a unit is 1875 / 120 = 15 ms (integer division,
 * i.e. 125 bpm rather than the nominal 120). */
#define MEL_TEMPO 120
#define MEL_UNIT_MS (1875 / MEL_TEMPO)
#define USER_TIMER_MINIMUM 10 /* SetTimer clamps shorter periods (and 0) to 10 ms */
#define MEL_VELOCITY 0x40

/* Frequencies of MIDI notes 0..11 (C-1 .. B-1), equal temperament with A4 = 440 Hz. */
static const float SEMI[12] = {8.1757989f,  8.6619572f,  9.1770240f,  9.7227182f,
                               10.3008612f, 10.9133822f, 11.5623257f, 12.2498574f,
                               12.9782718f, 13.7500000f, 14.5676175f, 15.4338532f};

#define ATTACK_S 0.003f
#define DECAY_S 0.250f
#define SUSTAIN 0.6f
#define RELEASE_S 0.012f
#define DRUM_DECAY_S 0.060f
#define VOICE_GAIN 0.5f /* peak per channel at MIDI volume 0xff; 0x80 gives about -12 dBFS */
#define MIX_S 0.030f    /* voice switch crossfade */
/* SoundFont voice: TinySoundFont's output to the scale of the other voices. At velocity 64
 * TimGM6mb's piano comes out at an RMS of about 0.09, so this makes game melodies about as loud
 * as the procedural piano (median RMS within 30 % over six games' title music) */
#define GM_GAIN 6.0f
#define GM_BLOCK 256    /* frames rendered by TinySoundFont at a time */

/* piano voice (see the top of the file) */
#define PIANO_RMS 0.65f        /* RMS of a note at its onset, before the output gains */
#define PIANO_ATTACK_S 0.0015f
#define PIANO_PROMPT 0.55f     /* share of the prompt sound in each partial */
#define PIANO_NOISE 0.12f      /* hammer noise level */
#define PIANO_NOISE_S 0.006f
#define PIANO_SILENT 1e-4f     /* a released note is freed below this summed level */
#define PIANO_DETUNE 0.0004    /* the two strings of a unison: +-0.7 cents */
#define PIANO_STRING_A 0.55f
#define PIANO_STRING_B 0.45f

static Melody *mel(struct ExenVM *vm) { return &vm->plat->melody; }

/* ------------------------------------------------------------------ maths */

/* e^x, to about 1e-15 relative; 0 below -700 */
static double m_exp(double x)
{
    if (x < -700.0)
        return 0.0;
    if (x > 700.0)
        x = 700.0;
    double kf = x * 1.4426950408889634;
    int k = (int)(kf < 0.0 ? kf - 0.5 : kf + 0.5);
    double r = x - (double)k * 0.6931471805599453; /* |r| <= 0.35 */
    double p = 1.0, t = 1.0;
    for (int i = 1; i <= 13; i++) {
        t *= r / (double)i;
        p += t;
    }
    uint64_t bits = (uint64_t)(k + 1023) << 52; /* 2^k */
    double sc;
    memcpy(&sc, &bits, sizeof sc);
    return p * sc;
}

/* square root of x >= 0 (Newton's method from above) */
static double m_sqrt(double x)
{
    if (x <= 0.0)
        return 0.0;
    double y = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 100; i++) {
        double n = 0.5 * (y + x / y);
        if (n >= y)
            break;
        y = n;
    }
    return y;
}

/* sin(2 pi p) for p in [0, 1): odd polynomial on [-pi/2, pi/2], error below 4e-6 */
static float sin1(float p)
{
    float x = p < 0.25f ? p : p < 0.75f ? 0.5f - p : p - 1.0f;
    float y = x * 6.28318531f, y2 = y * y;
    return y * (1.0f + y2 * (-1.0f / 6.0f + y2 * (1.0f / 120.0f +
                y2 * (-1.0f / 5040.0f + y2 * (1.0f / 362880.0f)))));
}

static double note_freq(unsigned note)
{
    return (double)SEMI[note % 12] * (double)(1u << (note / 12));
}

void melody_init(Melody *m, int rate)
{
    memset(m, 0, sizeof *m);
    m->rate = rate >= 8000 && rate <= 192000 ? rate : 44100;
    m->volume = 100;
    m->timbre = EXEN_VOICE_RINGTONE;
    m->vol_l = m->vol_r = 0x80;
    m->prev = 0xffff;
}

/* ------------------------------------------------------------------ piano voice */

/* Per-note coefficients, derived from the note and the output rate. */
typedef struct PianoParams {
    int n;                                  /* partials in use */
    float inc[PIANO_PARTIALS][2];           /* phase increments of the two strings */
    float d1[PIANO_PARTIALS], d2[PIANO_PARTIALS]; /* per-sample prompt / aftersound decay */
    float dr[PIANO_PARTIALS];               /* extra per-sample decay once released */
    float amp[PIANO_PARTIALS];              /* level at the strike */
    float att;                              /* attack ramp step */
    float nd, nk1, nk2;                     /* hammer noise decay and band-pass */
} PianoParams;

/* per-sample factor of an exponential decay with time constant tau seconds */
static float decay_factor(double tau, double rate) { return (float)m_exp(-1.0 / (tau * rate)); }

static void piano_params(const Melody *m, unsigned note, PianoParams *pp)
{
    double rate = (double)m->rate;
    double f0 = note_freq(note);
    double oct = ((double)note - 60.0) / 12.0;            /* octaves from middle C */
    double B = 0.00012 * m_exp(0.40 * oct);               /* inharmonicity */
    double fmax = 0.40 * rate < 10000.0 ? 0.40 * rate : 10000.0;
    double tbase = 3.0 * m_exp(-0.35 * oct);              /* aftersound of the fundamental */
    tbase = tbase < 0.8 ? 0.8 : tbase > 5.0 ? 5.0 : tbase;
    double trel = 0.07 * m_exp(-0.20 * oct);              /* damper */
    trel = trel < 0.04 ? 0.04 : trel > 0.12 ? 0.12 : trel;
    double sum2 = 0.0, peak = 0.0;
    int n = 0;
    for (int k = 0; k < PIANO_PARTIALS; k++) {
        double h = (double)(k + 1);
        double fn = h * f0 * m_sqrt(1.0 + B * h * h);
        if (fn >= fmax)
            break;
        /* struck string, hammer at 1/8.3 of its length: |sin(pi h x0)| / h */
        double cyc = h / (2.0 * 8.3);
        double a = sin1((float)(cyc - (double)(int)cyc));
        a = (a < 0.0 ? -a : a) / h;
        a /= 1.0 + (fn / 2400.0) * (fn / 2400.0);         /* felt: a medium blow */
        a *= fn * fn / (fn * fn + 120.0 * 120.0);         /* soundboard */
        double taper = (fmax - fn) / (0.2 * fmax);        /* fade out towards the band edge */
        if (taper < 1.0)
            a *= taper;
        double t2 = tbase / (1.0 + fn / 800.0 + (fn / 3000.0) * (fn / 3000.0));
        pp->inc[k][0] = (float)(fn * (1.0 + PIANO_DETUNE) / rate);
        pp->inc[k][1] = (float)(fn * (1.0 - PIANO_DETUNE) / rate);
        pp->d1[k] = decay_factor(0.3 * t2, rate);
        pp->d2[k] = decay_factor(t2, rate);
        pp->dr[k] = decay_factor(trel / (1.0 + fn / 2500.0), rate);
        pp->amp[k] = (float)a;
        sum2 += a * a;
        if (a > peak)
            peak = a;
        n = k + 1;
    }
    /* drop the inaudible top partials, then normalise the loudness */
    while (n > 1 && pp->amp[n - 1] < 0.002 * peak)
        n--;
    double g = sum2 > 0.0 ? PIANO_RMS / m_sqrt(0.5 * sum2) : 0.0;
    for (int k = 0; k < n; k++)
        pp->amp[k] = (float)(pp->amp[k] * g);
    pp->n = n;
    pp->att = (float)(1.0 / (PIANO_ATTACK_S * rate));
    pp->nd = decay_factor(PIANO_NOISE_S, rate);
    double fhi = 3500.0 < 0.35 * rate ? 3500.0 : 0.35 * rate;
    pp->nk1 = (float)(1.0 - m_exp(-6.283185307179586 * fhi / rate));
    pp->nk2 = (float)(1.0 - m_exp(-6.283185307179586 * 400.0 / rate));
}

static float piano_level(const PianoVoice *v)
{
    float l = v->ne;
    for (int k = 0; k < PIANO_PARTIALS; k++)
        l += v->e1[k] + v->e2[k];
    return l;
}

static void piano_off(Melody *m, int ch, unsigned note)
{
    for (int i = 0; i < PIANO_VOICES; i++) {
        PianoVoice *v = &m->piano[i];
        if (v->state == 1 && v->ch == ch && v->note == note)
            v->state = 2;
    }
}

static void piano_all_off(Melody *m)
{
    for (int i = 0; i < PIANO_VOICES; i++)
        if (m->piano[i].state == 1)
            m->piano[i].state = 2;
}

static void piano_on(Melody *m, int ch, unsigned note)
{
    if (ch == 9) /* drums: the noise drums of the ring-tone pool play in both voices */
        return;
    piano_off(m, ch, note); /* a restrike: the old note rings out under the new one */
    PianoVoice *v = NULL;
    for (int i = 0; i < PIANO_VOICES && !v; i++)
        if (m->piano[i].state == 0)
            v = &m->piano[i];
    if (!v) { /* steal the quietest released note (else the quietest one) */
        float best = 0.0f;
        for (int i = 0; i < PIANO_VOICES; i++) {
            PianoVoice *c = &m->piano[i];
            float l = piano_level(c) + (c->state == 1 ? 1e6f : 0.0f);
            if (!v || l < best) {
                v = c;
                best = l;
            }
        }
    }
    PianoParams pp;
    piano_params(m, note, &pp);
    memset(v, 0, sizeof *v);
    v->state = 1;
    v->note = (uint8_t)note;
    v->ch = (uint8_t)ch;
    for (int k = 0; k < pp.n; k++) {
        v->e1[k] = pp.amp[k] * PIANO_PROMPT;
        v->e2[k] = pp.amp[k] * (1.0f - PIANO_PROMPT);
        /* Schroeder phases: the same spectrum with a low crest factor, so the note can be as
         * loud as the ring tone without peaking */
        uint32_t q = (uint32_t)(k * (k + 1)) % (uint32_t)(2 * pp.n);
        v->ph[k][0] = v->ph[k][1] = (float)q / (float)(2 * pp.n);
    }
    v->noise = 0x1d3bu ^ (note * 0x9e5u);
    if (!v->noise)
        v->noise = 1;
    v->ne = PIANO_NOISE;
}

/* One sample of a piano note. Levels and phases always advance; the oscillators are only
 * evaluated when the result is heard. */
static float piano_sample(PianoVoice *v, const PianoParams *pp, int audible)
{
    int rel = v->state == 2;
    float s = 0.0f, lvl = 0.0f;
    for (int k = 0; k < pp->n; k++) {
        float e1 = v->e1[k] * pp->d1[k], e2 = v->e2[k] * pp->d2[k];
        if (rel) {
            e1 *= pp->dr[k];
            e2 *= pp->dr[k];
        }
        if (e1 < 1e-9f)
            e1 = 0.0f; /* no denormals */
        if (e2 < 1e-9f)
            e2 = 0.0f;
        v->e1[k] = e1;
        v->e2[k] = e2;
        float a = v->ph[k][0] + pp->inc[k][0], b = v->ph[k][1] + pp->inc[k][1];
        if (a >= 1.0f)
            a -= 1.0f;
        if (b >= 1.0f)
            b -= 1.0f;
        v->ph[k][0] = a;
        v->ph[k][1] = b;
        float e = e1 + e2;
        lvl += e;
        if (audible && e > 0.0f)
            s += e * (PIANO_STRING_A * sin1(a) + PIANO_STRING_B * sin1(b));
    }
    if (v->ne > 0.0f) { /* hammer knock: a band-passed noise burst */
        v->noise = (v->noise >> 1) ^ ((0u - (v->noise & 1u)) & 0xb400u);
        float x = (v->noise & 1u) ? 1.0f : -1.0f;
        v->n1 += (x - v->n1) * pp->nk1;
        v->n2 += (x - v->n2) * pp->nk2;
        s += v->ne * (v->n1 - v->n2);
        v->ne *= pp->nd;
        if (v->ne < 1e-6f)
            v->ne = 0.0f;
        lvl += v->ne;
    }
    if (v->att < 1.0f) {
        v->att += pp->att;
        if (v->att > 1.0f)
            v->att = 1.0f;
        s *= v->att;
    }
    if (rel && lvl < PIANO_SILENT)
        memset(v, 0, sizeof *v);
    return audible ? s : 0.0f;
}

/* ------------------------------------------------------------------ voices */

static void voice_release(Melody *m, MelodyVoice *v)
{
    if (v->state == 0 || v->state == 3)
        return;
    v->state = 3;
    v->rel_step = v->env / (RELEASE_S * (float)m->rate);
    if (v->rel_step <= 0.0f)
        v->state = 0;
}

static void note_off(Melody *m, int ch, unsigned note)
{
    for (int i = 0; i < MELODY_VOICES; i++) {
        MelodyVoice *v = &m->voice[i];
        if (v->state && v->state != 3 && v->ch == ch && v->note == note)
            voice_release(m, v);
    }
    piano_off(m, ch, note);
    if (m->gm) /* 0x80|ch, note, 0 */
        gm_note_off(m->gm, ch, (int)note);
}

static void all_off(Melody *m)
{
    for (int i = 0; i < MELODY_VOICES; i++)
        voice_release(m, &m->voice[i]);
    piano_all_off(m);
    if (m->gm)
        gm_all_off(m->gm);
}

/* the program table changed: the SoundFont channels follow it */
static void programs_changed(Melody *m)
{
    if (m->gm)
        gm_set_programs(m->gm, m->program);
}

static void note_on(Melody *m, int ch, unsigned note)
{
    /* the same note again on the same channel retriggers (MIDI would layer it) */
    note_off(m, ch, note);
    MelodyVoice *v = NULL;
    for (int i = 0; i < MELODY_VOICES && !v; i++)
        if (m->voice[i].state == 0)
            v = &m->voice[i];
    if (!v) { /* steal the quietest releasing voice, else the first */
        v = &m->voice[0];
        for (int i = 1; i < MELODY_VOICES; i++)
            if (m->voice[i].state == 3 && (v->state != 3 || m->voice[i].env < v->env))
                v = &m->voice[i];
    }
    float f = SEMI[note % 12] * (float)(1u << (note / 12));
    v->state = 1;
    v->note = (uint8_t)note;
    v->ch = (uint8_t)ch;
    v->drum = ch == 9;
    v->phase = 0.0f;
    v->inc = f / (float)m->rate;
    v->env = 0.0f;
    v->noise = 0xace1u + note;
    v->lp = 0.0f;
    piano_on(m, ch, note);
    if (m->gm) /* 0x90|ch, note, 0x40 */
        gm_note_on(m->gm, ch, (int)note);
}

/* ------------------------------------------------------------------ sequencer */

static void set_volume(Melody *m, uint8_t v) /* 0x43ac4e */
{
    if (m->vol_l == m->vol_r)
        m->vol_l = m->vol_r = v;
    else if (m->vol_l)
        m->vol_l = v, m->vol_r = 0;
    else
        m->vol_l = 0, m->vol_r = v;
}

static void set_balance(Melody *m, uint8_t b) /* command 0x85 */
{
    if (b < 0x80) {         /* everything to the left */
        m->vol_l |= m->vol_r;
        m->vol_r = 0;
    } else if (b > 0x80) {  /* everything to the right */
        m->vol_r |= m->vol_l;
        m->vol_l = 0;
    } else if (m->vol_r == 0) {
        m->vol_r = m->vol_l;
    } else {
        m->vol_l |= m->vol_r;
    }
}

static void seq_end(Melody *m)
{
    m->playing = 0;
}

/* One firing of the player timer (0x43ad57). The recursion of the original for commands is a
 * loop here. */
static void seq_fire(Melody *m)
{
    for (;;) {
        if (m->prev >= 1) {
            if (m->pos + 1 < m->len)
                m->cur_dur = DUR[m->data[m->pos + 1] % 14];
            note_off(m, m->ch, m->prev);
        }
        /* The original compares the event count with the *byte* length, so without a 0
         * terminator it reads past the array. We stop at the end of the array instead. */
        if (++m->events >= m->len || m->pos + 2 > m->len) {
            seq_end(m);
            return;
        }
        uint8_t a = m->data[m->pos], b = m->data[m->pos + 1];
        m->prev = a;
        if (a > 0x7f) {
            m->cur_dur = 0;
            switch (a - 0x80) {
            case 0: m->ch = b & 0x0f; break;
            case 1:
                if (b == 0) {
                    m->ch = 9; /* drums, program 0 */
                    m->program[9] = 0;
                } else if (b <= 0x80) {
                    m->program[m->ch] = b;
                }
                programs_changed(m);
                break;
            case 2:
                /* "Rest": the original sets the duration but neither advances past the pair nor
                 * re-arms the timer, so every later firing re-reads this 0x82 pair until the
                 * event count runs out. The melody therefore goes silent here for good. */
                m->cur_dur = DUR[b % 14];
                seq_end(m);
                return;
            case 3:
                /* retime: SetTimer(1875 / b). Always overridden by the next note's re-arm, or
                 * moot because the melody ends; b == 0 would divide by zero in the original. */
                if (b)
                    m->period_ms = 1875 / b;
                break;
            case 4: set_volume(m, b); break;
            case 5: set_balance(m, b); break;
            default: break; /* 0x86..0xff: ignored, the next pair follows */
            }
            m->pos += 2;
            continue;
        }
        if (a == 0) { /* end marker; with a single pair it is just silence */
            if (m->len > 2) { /* melody_stop: all off, system reset */
                all_off(m);
                memset(m->program, 0, sizeof m->program);
                programs_changed(m);
            }
            seq_end(m);
            return;
        }
        note_on(m, m->ch, a);
        m->pos += 2;
        m->period_ms = MEL_UNIT_MS * m->cur_dur;
        return;
    }
}

void melody_play(struct ExenVM *vm, const uint8_t *data, uint32_t len)
{
    Melody *m = mel(vm);
    if (len > MELODY_MAX) {
        vm_log(vm, "playMelody: %u-byte melody cut to %d bytes", len, MELODY_MAX);
        len = MELODY_MAX;
    }
    /* The original re-opens the MIDI device and restarts; a note still sounding on the old
     * handle is never turned off. We release it. */
    all_off(m);
    memcpy(m->data, data, len);
    m->len = len;
    m->pos = 0;
    m->events = 0;
    m->vol_l = m->vol_r = 0x80; /* midiOutSetVolume(0x80808080) */
    m->prev = 0xffff;
    m->period_ms = USER_TIMER_MINIMUM; /* SetTimer(hwnd, 10, 0, proc) */
    m->wait = (int64_t)m->period_ms * m->rate;
    m->playing = 1;
    m->melodies++;
    if (vm->host.melody_play)
        vm->host.melody_play(vm->host.user, data, (int)len);
}

void melody_stop(struct ExenVM *vm)
{
    Melody *m = mel(vm);
    /* KillTimer, system reset (0xff), midiOutClose: the channel byte is a global and stays */
    m->playing = 0;
    all_off(m);
    memset(m->program, 0, sizeof m->program);
    programs_changed(m);
    if (vm->host.melody_stop)
        vm->host.melody_stop(vm->host.user);
}

/* ------------------------------------------------------------------ rendering */

static float polyblep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static float voice_sample(MelodyVoice *v, float att, float dec, float ddec)
{
    switch (v->state) {
    case 1:
        v->env += att;
        if (v->env >= 1.0f) {
            v->env = 1.0f;
            v->state = 2;
        }
        break;
    case 2:
        if (v->drum)
            v->env *= ddec;
        else
            v->env = SUSTAIN + (v->env - SUSTAIN) * dec;
        break;
    case 3:
        v->env -= v->rel_step;
        if (v->env <= 0.0f) {
            v->env = 0.0f;
            v->state = 0;
            return 0.0f;
        }
        break;
    default:
        return 0.0f;
    }
    float s;
    if (v->drum) {
        /* 16-bit Galois LFSR noise, low-passed more for lower drum notes */
        v->noise = (v->noise >> 1) ^ ((0u - (v->noise & 1u)) & 0xb400u);
        float n = (v->noise & 1u) ? 1.0f : -1.0f;
        float k = 0.15f + (float)(v->note & 0x7f) * 0.008f;
        v->lp += (n - v->lp) * (k > 1.0f ? 1.0f : k);
        s = v->lp;
    } else {
        float t = v->phase, dt = v->inc;
        s = t < 0.5f ? 1.0f : -1.0f;
        s += polyblep(t, dt);
        float t2 = t + 0.5f;
        if (t2 >= 1.0f)
            t2 -= 1.0f;
        s -= polyblep(t2, dt);
        v->phase = t + dt;
        if (v->phase >= 1.0f)
            v->phase -= 1.0f;
    }
    return s * v->env * ((float)MEL_VELOCITY / 127.0f * 2.0f);
}

static int16_t clip16(float x)
{
    if (x > 32767.0f)
        return 32767;
    if (x < -32768.0f)
        return -32768;
    return (int16_t)x;
}

static void synth(Melody *m, int16_t *out, int frames)
{
    float rate = (float)m->rate;
    float att = 1.0f / (ATTACK_S * rate);
    float dec = 1.0f - 1.0f / (DECAY_S * rate);
    float ddec = 1.0f - 1.0f / (DRUM_DECAY_S * rate);
    float hv = (float)m->volume / 100.0f;
    hv *= hv; /* loudness control: square law */
    float tl = VOICE_GAIN * hv * (float)m->vol_l / 255.0f * 32767.0f;
    float tr = VOICE_GAIN * hv * (float)m->vol_r / 255.0f * 32767.0f;
    float smooth = 1.0f / (0.005f * rate); /* gain changes glide over ~5 ms */
    float lpk = 0.45f;                     /* one-pole low-pass, about 4.5 kHz at 44.1 kHz */
    if (m->rate > 44100)
        lpk = 0.45f * 44100.0f / rate;
    int any = 0, anyp = 0, gm = m->gm != NULL, anyg = gm && gm_active(m->gm);
    for (int i = 0; i < MELODY_VOICES; i++)
        any |= m->voice[i].state != 0;
    /* piano coefficients of the sounding notes, for this block */
    PianoParams pp[PIANO_VOICES];
    for (int i = 0; i < PIANO_VOICES; i++)
        if (m->piano[i].state) {
            piano_params(m, m->piano[i].note, &pp[i]);
            anyp = 1;
        }
    float target = m->timbre == EXEN_VOICE_PIANO ? 1.0f : 0.0f;
    float mstep = 1.0f / (MIX_S * rate);
    if (!any && !anyp && !anyg)
        m->mix = target; /* nothing sounds: switch at once */
    float gbuf[2 * GM_BLOCK]; /* SoundFont voice, this block */
    for (int f0 = 0; f0 < frames; f0 += GM_BLOCK) {
        int nb = frames - f0 < GM_BLOCK ? frames - f0 : GM_BLOCK;
        if (anyg) /* always rendered while it has voices, so it keeps time when unheard */
            gm_render(m->gm, gbuf, nb);
        for (int j = 0; j < nb; j++) {
            int f = f0 + j;
            float s = 0.0f, drum = 0.0f, p = 0.0f, gl = 0.0f, gr = 0.0f;
            if (any)
                for (int i = 0; i < MELODY_VOICES; i++) {
                    MelodyVoice *v = &m->voice[i];
                    if (v->drum)
                        drum += voice_sample(v, att, dec, ddec);
                    else
                        s += voice_sample(v, att, dec, ddec);
                }
            if (m->mix != target) {
                m->mix += m->mix < target ? mstep : -mstep;
                if ((target - m->mix) * (target > 0.5f ? 1.0f : -1.0f) <= 0.0f)
                    m->mix = target;
            }
            float mix = m->mix;
            /* the procedural piano is heard only without a SoundFont */
            if (anyp)
                for (int i = 0; i < PIANO_VOICES; i++)
                    if (m->piano[i].state)
                        p += piano_sample(&m->piano[i], &pp[i], mix > 0.0f && !gm);
            if (anyg && mix > 0.0f) {
                gl = gbuf[2 * j] * (GM_GAIN * mix);
                gr = gbuf[2 * j + 1] * (GM_GAIN * mix);
            }
            /* The ring tone and the noise drums go through the output low-pass; the piano and
             * the SoundFont are band-limited already. With a SoundFont, its drum kit replaces
             * the noise drums. */
            if (mix > 0.0f) {
                s *= 1.0f - mix;
                if (gm)
                    drum *= 1.0f - mix;
            }
            s += drum;
            p *= mix;
            m->gain_l += (tl - m->gain_l) * smooth;
            m->gain_r += (tr - m->gain_r) * smooth;
            m->lp_l += (s * m->gain_l - m->lp_l) * lpk;
            m->lp_r += (s * m->gain_r - m->lp_r) * lpk;
            out[2 * f] = clip16(m->lp_l + (p + gl) * m->gain_l);
            out[2 * f + 1] = clip16(m->lp_r + (p + gr) * m->gain_r);
        }
    }
}

int exen_audio_render(ExenVM *vm, int16_t *out, int frames)
{
    Melody *m = mel(vm);
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        while (m->playing && m->wait <= 0) {
            seq_fire(m);
            if (m->period_ms < USER_TIMER_MINIMUM)
                m->period_ms = USER_TIMER_MINIMUM;
            m->wait += (int64_t)m->period_ms * m->rate; /* periodic: re-arms from this firing */
        }
        if (m->playing) {
            int64_t until = (m->wait + 999) / 1000;
            if (until < n)
                n = (int)until;
        }
        synth(m, out + 2 * done, n);
        if (m->playing)
            m->wait -= (int64_t)n * 1000;
        done += n;
    }
    return frames;
}

int exen_audio_active(ExenVM *vm)
{
    Melody *m = mel(vm);
    if (m->playing)
        return 1;
    for (int i = 0; i < MELODY_VOICES; i++)
        if (m->voice[i].state)
            return 1;
    /* piano and SoundFont tails count only while heard: they ring out while the host renders
     * anyway */
    if (m->mix > 0.0f || m->timbre == EXEN_VOICE_PIANO) {
        if (m->gm)
            return gm_active(m->gm) || m->lp_l > 1.0f || m->lp_l < -1.0f || m->lp_r > 1.0f ||
                   m->lp_r < -1.0f;
        for (int i = 0; i < PIANO_VOICES; i++)
            if (m->piano[i].state)
                return 1;
    }
    /* let the output filter settle to silence */
    return m->lp_l > 1.0f || m->lp_l < -1.0f || m->lp_r > 1.0f || m->lp_r < -1.0f;
}

void exen_audio_set_volume(ExenVM *vm, int volume)
{
    mel(vm)->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

void exen_audio_set_voice(ExenVM *vm, int voice)
{
    Melody *m = mel(vm);
    m->timbre = voice == EXEN_VOICE_PIANO ? EXEN_VOICE_PIANO : EXEN_VOICE_RINGTONE;
    int sounding = 0;
    for (int i = 0; i < MELODY_VOICES; i++)
        sounding |= m->voice[i].state != 0;
    for (int i = 0; i < PIANO_VOICES; i++)
        sounding |= m->piano[i].state != 0;
    if (m->gm)
        sounding |= gm_active(m->gm);
    if (!sounding) /* nothing to crossfade */
        m->mix = m->timbre == EXEN_VOICE_PIANO ? 1.0f : 0.0f;
}

int exen_audio_voice(ExenVM *vm) { return mel(vm)->timbre; }

/* The SoundFont voices from silence, on the current programs, with the note the sequencer has
 * sounding (if any) struck again: what a MIDI synth reset while the melody plays would do. */
static void gm_restart(Melody *m)
{
    gm_silence(m->gm);
    gm_set_programs(m->gm, m->program);
    if (m->playing && m->prev >= 1 && m->prev <= 0x7f)
        gm_note_on(m->gm, m->ch, m->prev);
}

int exen_audio_set_soundfont(ExenVM *vm, const void *sf2, size_t len)
{
    Melody *m = mel(vm);
    if (!sf2 || !len) {
        gm_free(m->gm);
        m->gm = NULL;
        return 0;
    }
    GmSynth *g = gm_create(sf2, len, m->rate);
    if (!g)
        return -1; /* not a usable SoundFont: the previous one (if any) stays */
    gm_free(m->gm);
    m->gm = g;
    gm_restart(m);
    return 0;
}

int exen_audio_soundfont(ExenVM *vm) { return mel(vm)->gm != NULL; }

int melody_gm_restore(Melody *m, const uint8_t *rec, size_t len)
{
    if (!m->gm)
        return 0;
    if (rec && gm_load(m->gm, rec, len) == 0) {
        gm_set_programs(m->gm, m->program);
        return 1;
    }
    gm_restart(m);
    return 0;
}

void melody_free(Melody *m)
{
    gm_free(m->gm);
    m->gm = NULL;
}

int exen_audio_rate(ExenVM *vm) { return mel(vm)->rate; }
