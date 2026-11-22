/* Melody sequencer and synthesizer. See melody.h and docs/re/NATIVES.md §3.2.
 *
 * Sequencer: a step-by-step port of the original timer proc (0x43ad57). The original arms a
 * periodic Win32 timer (SetTimer id 10) and every firing handles one event; commands chain
 * into the next pair in the same firing. Here the "timer" is a countdown on the audio clock.
 *
 * Synthesizer: the original sends the notes to the default MIDI device (General MIDI piano at
 * velocity 64). We render a ring-tone style voice instead: a PolyBLEP band-limited square
 * wave with a 3 ms attack, a gentle decay to 60 % and a 12 ms linear release, through a
 * one-pole low-pass. Channel 9 (selected by program change 0) plays filtered noise bursts.
 * The player is monophonic (each event first turns the previous note off), so a few voices
 * cover the overlapping release tails.
 */
#include <string.h>

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

static Melody *mel(struct ExenVM *vm) { return &vm->plat->melody; }

void melody_init(Melody *m, int rate)
{
    memset(m, 0, sizeof *m);
    m->rate = rate >= 8000 && rate <= 192000 ? rate : 44100;
    m->volume = 100;
    m->vol_l = m->vol_r = 0x80;
    m->prev = 0xffff;
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
}

static void all_off(Melody *m)
{
    for (int i = 0; i < MELODY_VOICES; i++)
        voice_release(m, &m->voice[i]);
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
    int any = 0;
    for (int i = 0; i < MELODY_VOICES; i++)
        any |= m->voice[i].state != 0;
    for (int f = 0; f < frames; f++) {
        float s = 0.0f;
        if (any)
            for (int i = 0; i < MELODY_VOICES; i++)
                s += voice_sample(&m->voice[i], att, dec, ddec);
        m->gain_l += (tl - m->gain_l) * smooth;
        m->gain_r += (tr - m->gain_r) * smooth;
        m->lp_l += (s * m->gain_l - m->lp_l) * lpk;
        m->lp_r += (s * m->gain_r - m->lp_r) * lpk;
        out[2 * f] = clip16(m->lp_l);
        out[2 * f + 1] = clip16(m->lp_r);
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
    /* let the output filter settle to silence */
    return m->lp_l > 1.0f || m->lp_l < -1.0f || m->lp_r > 1.0f || m->lp_r < -1.0f;
}

void exen_audio_set_volume(ExenVM *vm, int volume)
{
    mel(vm)->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

int exen_audio_rate(ExenVM *vm) { return mel(vm)->rate; }
