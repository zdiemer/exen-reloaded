/* General MIDI melody voice. See gm.h.
 *
 * The original simulator sent the player's events to the default MIDI device (on Windows the
 * Microsoft GS Wavetable synth): note-on at velocity 0x40, note-off before every next event,
 * program changes (0x81; 0 selects the drum channel 9) and the device volume (0x84 / 0x85,
 * applied by melody.c as the output gains). This file plays the same events with
 * TinySoundFont from a General MIDI SoundFont, so the timbre is a real GM instrument (program 0,
 * the acoustic grand piano, unless the melody changes it) instead of an approximation.
 *
 * TinySoundFont is compiled here only, with static linkage and without stdio (the core does no
 * I/O). Its voices are plain data apart from the region pointer, which is stored as an index,
 * so a state stores them field by field and restores them bit for bit. The channel table is
 * not stored: it is derived from the melody's program table (gm_set_programs).
 */
#include <stdlib.h>
#include <string.h>

#include "gm.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
/* the API functions the core does not call (TSF_STATIC makes them static) */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#if defined(__clang__) && defined(__has_warning)
#if __has_warning("-Wnull-pointer-subtraction") /* its hydra readers' offsetof-style macro */
#pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#endif
#endif
#define TSF_IMPLEMENTATION
#define TSF_STATIC
#define TSF_NO_STDIO
#include "tsf.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define GM_VELOCITY (64.0f / 127.0f) /* note-on 0x90|ch, note, 0x40 */
#define GM_STATE_VERSION 1u
#define GM_VOICE_BYTES 264

struct GmSynth {
    tsf *f;
    uint32_t font_crc, font_len;
    int rate;
};

/* CRC-32 (zlib) of the SoundFont, to recognise it in a save state */
static uint32_t crc32_sf(const uint8_t *p, size_t n)
{
    uint32_t t[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
        t[i] = c;
    }
    uint32_t c = 0xffffffffu;
    while (n--)
        c = t[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/* The SoundFont bytes as a tsf_stream. tsf_load_memory does the same through casts of its
 * reader functions to the stream's function types, which UBSan's -fsanitize=function rejects;
 * these have the exact types. */
typedef struct {
    const uint8_t *p;
    size_t len, pos;
} MemStream;

static int ms_read(void *data, void *ptr, unsigned int size)
{
    MemStream *m = data;
    if (size > m->len - m->pos)
        size = (unsigned int)(m->len - m->pos);
    if (size)
        memcpy(ptr, m->p + m->pos, size);
    m->pos += size;
    return (int)size;
}

static int ms_skip(void *data, unsigned int count)
{
    MemStream *m = data;
    if (count > m->len - m->pos)
        return 0;
    m->pos += count;
    return 1;
}

static void clear_voices(tsf *f)
{
    memset(f->voices, 0, (size_t)f->voiceNum * sizeof *f->voices);
    for (int i = 0; i < f->voiceNum; i++)
        f->voices[i].playingPreset = -1;
    f->voicePlayIndex = 0;
}

GmSynth *gm_create(const void *sf2, size_t len, int rate)
{
    if (!sf2 || len < 12 || len > 0x7fffffffu || rate <= 0)
        return NULL;
    GmSynth *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    MemStream ms = {sf2, len, 0};
    struct tsf_stream stream = {&ms, ms_read, ms_skip};
    g->f = tsf_load(&stream);
    if (!g->f || g->f->presetNum <= 0 || !tsf_set_max_voices(g->f, GM_VOICES) ||
        g->f->voiceNum != GM_VOICES || !tsf_channel_set_bank(g->f, 15, 0)) {
        gm_free(g);
        return NULL;
    }
    tsf_set_output(g->f, TSF_STEREO_INTERLEAVED, rate, 0.0f);
    clear_voices(g->f);
    g->font_crc = crc32_sf(sf2, len);
    g->font_len = (uint32_t)len;
    g->rate = rate;
    static const uint8_t zero[16];
    gm_set_programs(g, zero);
    return g;
}

void gm_free(GmSynth *g)
{
    if (!g)
        return;
    if (g->f)
        tsf_close(g->f);
    free(g);
}

void gm_note_on(GmSynth *g, int ch, int note)
{
    tsf_channel_note_on(g->f, ch & 15, note & 0x7f, GM_VELOCITY);
}

void gm_note_off(GmSynth *g, int ch, int note) { tsf_channel_note_off(g->f, ch & 15, note & 0x7f); }

void gm_all_off(GmSynth *g) { tsf_note_off_all(g->f); }

void gm_silence(GmSynth *g) { clear_voices(g->f); }

void gm_set_programs(GmSynth *g, const uint8_t program[16])
{
    for (int ch = 0; ch < 16; ch++) {
        int drums = ch == 9;
        /* 0x81 sends 1..0x80 as the program byte; 0x80 is not a valid MIDI data byte and is
         * taken as program 0 (unverified; no game sends program changes) */
        if (!tsf_channel_set_presetnumber(g->f, ch, program[ch] & 0x7f, drums) &&
            !tsf_channel_set_presetnumber(g->f, ch, 0, drums))
            tsf_channel_set_presetindex(g->f, ch, 0);
    }
}

int gm_active(const GmSynth *g)
{
    for (int i = 0; i < g->f->voiceNum; i++)
        if (g->f->voices[i].playingPreset != -1)
            return 1;
    return 0;
}

uint32_t gm_notes(const GmSynth *g) { return g->f->voicePlayIndex; }

void gm_render(GmSynth *g, float *lr, int frames)
{
    if (frames <= 0)
        return;
    tsf_render_float(g->f, lr, frames, 0);
    /* Never let a broken voice (say, from a hand-made save state) reach the output. */
    for (int i = 0; i < 2 * frames; i++)
        if (!(lr[i] >= -64.0f && lr[i] <= 64.0f)) {
            memset(lr, 0, (size_t)frames * 2 * sizeof *lr);
            clear_voices(g->f);
            return;
        }
}

/* ------------------------------------------------------------------ save states */

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static uint8_t *putf(uint8_t *p, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return put32(p, u);
}

static uint8_t *putd(uint8_t *p, double d)
{
    uint64_t u;
    memcpy(&u, &d, 8);
    p = put32(p, (uint32_t)u);
    return put32(p, (uint32_t)(u >> 32));
}

static uint32_t get32(const uint8_t **p)
{
    const uint8_t *b = *p;
    *p += 4;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static float getf(const uint8_t **p)
{
    uint32_t u = get32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static double getd(const uint8_t **p)
{
    uint64_t lo = get32(p), hi = get32(p);
    uint64_t u = lo | hi << 32;
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static uint8_t *put_env(uint8_t *p, const struct tsf_voice_envelope *e)
{
    p = put32(p, e->segment);
    p = put32(p, (uint32_t)e->segmentIsExponential | (uint32_t)e->isAmpEnv << 1);
    p = put32(p, (uint32_t)(int32_t)e->midiVelocity);
    p = putf(p, e->level);
    p = putf(p, e->slope);
    p = put32(p, (uint32_t)e->samplesUntilNextSegment);
    const struct tsf_envelope *q = &e->parameters;
    const float v[8] = {q->delay,   q->attack,  q->hold,         q->decay,
                        q->sustain, q->release, q->keynumToHold, q->keynumToDecay};
    for (int i = 0; i < 8; i++)
        p = putf(p, v[i]);
    return p;
}

static uint8_t *put_lfo(uint8_t *p, const struct tsf_voice_lfo *l)
{
    p = put32(p, (uint32_t)l->samplesUntil);
    p = putf(p, l->level);
    return putf(p, l->delta);
}

size_t gm_save(const GmSynth *g, uint8_t *out, size_t cap)
{
    if (cap < GM_STATE_BYTES)
        return 0;
    const tsf *f = g->f;
    uint8_t *p = out;
    p = put32(p, GM_STATE_VERSION);
    p = put32(p, g->font_crc);
    p = put32(p, g->font_len);
    p = put32(p, (uint32_t)g->rate);
    p = put32(p, GM_VOICES);
    p = put32(p, f->voicePlayIndex);
    for (int i = 0; i < GM_VOICES; i++) {
        const struct tsf_voice *v = &f->voices[i];
        uint8_t *rec = p;
        memset(rec, 0, GM_VOICE_BYTES);
        p = put32(p, (uint32_t)v->playingPreset);
        if (v->playingPreset != -1) { /* a free voice is just -1 and zeros */
            p = put32(p, (uint32_t)v->playingKey);
            p = put32(p, (uint32_t)v->playingChannel);
            p = put32(p, (uint32_t)v->heldSustain);
            p = put32(p, (uint32_t)(v->region - f->presets[v->playingPreset].regions));
            p = putd(p, v->pitchInputTimecents);
            p = putd(p, v->pitchOutputFactor);
            p = putd(p, v->sourceSamplePosition);
            p = putf(p, v->noteGainDB);
            p = putf(p, v->panFactorLeft);
            p = putf(p, v->panFactorRight);
            p = put32(p, v->playIndex);
            p = put32(p, v->loopStart);
            p = put32(p, v->loopEnd);
            p = put_env(p, &v->ampenv);
            p = put_env(p, &v->modenv);
            const struct tsf_voice_lowpass *lp = &v->lowpass;
            const double c[7] = {lp->QInv, lp->a0, lp->a1, lp->b1, lp->b2, lp->z1, lp->z2};
            for (int k = 0; k < 7; k++)
                p = putd(p, c[k]);
            p = put32(p, lp->active);
            p = put_lfo(p, &v->modlfo);
            p = put_lfo(p, &v->viblfo);
        }
        p = rec + GM_VOICE_BYTES;
    }
    return (size_t)(p - out);
}

/* finite and within +-lim */
static int fin(double x, double lim) { return x >= -lim && x <= lim; }

static int get_env(const uint8_t **p, struct tsf_voice_envelope *e)
{
    uint32_t seg = get32(p), flags = get32(p);
    int32_t vel = (int32_t)get32(p);
    e->segment = (unsigned char)seg;
    e->segmentIsExponential = flags & 1;
    e->isAmpEnv = (flags >> 1) & 1;
    e->midiVelocity = (short)vel;
    e->level = getf(p);
    e->slope = getf(p);
    e->samplesUntilNextSegment = (int32_t)get32(p);
    float v[8];
    for (int i = 0; i < 8; i++)
        v[i] = getf(p);
    struct tsf_envelope *q = &e->parameters;
    q->delay = v[0], q->attack = v[1], q->hold = v[2], q->decay = v[3];
    q->sustain = v[4], q->release = v[5], q->keynumToHold = v[6], q->keynumToDecay = v[7];
    /* bounds that keep the envelope code's float-to-int conversions and counters in range */
    int ok = seg <= TSF_SEGMENT_DONE && flags <= 3 && vel >= 0 && vel <= 127 &&
             fin(e->level, 16.0) && fin(e->slope, 16.0) &&
             e->samplesUntilNextSegment >= -(1 << 20) &&
             (q->sustain == 0.0f || (q->sustain >= 1e-12f && q->sustain <= 1.0f));
    for (int i = 0; i < 8; i++)
        ok &= i == 4 || fin(v[i], i >= 6 ? 12800.0 : 200.0);
    return ok;
}

static int get_lfo(const uint8_t **p, struct tsf_voice_lfo *l)
{
    l->samplesUntil = (int32_t)get32(p);
    l->level = getf(p);
    l->delta = getf(p);
    return fin(l->level, 16.0) && fin(l->delta, 16.0);
}

int gm_load(GmSynth *g, const uint8_t *in, size_t len)
{
    tsf *f = g->f;
    if (len != GM_STATE_BYTES)
        return -1;
    const uint8_t *p = in;
    if (get32(&p) != GM_STATE_VERSION || get32(&p) != g->font_crc || get32(&p) != g->font_len ||
        get32(&p) != (uint32_t)g->rate || get32(&p) != GM_VOICES || f->voiceNum != GM_VOICES)
        return -1;
    unsigned int play_index = get32(&p);
    struct tsf_voice vs[GM_VOICES];
    memset(vs, 0, sizeof vs);
    for (int i = 0; i < GM_VOICES; i++) {
        struct tsf_voice *v = &vs[i];
        const uint8_t *rec = p;
        v->playingPreset = (int32_t)get32(&p);
        if (v->playingPreset != -1) {
            if (v->playingPreset < 0 || v->playingPreset >= f->presetNum)
                return -1;
            const struct tsf_preset *pr = &f->presets[v->playingPreset];
            v->playingKey = (int32_t)get32(&p);
            v->playingChannel = (int32_t)get32(&p);
            v->heldSustain = (int32_t)get32(&p);
            uint32_t ri = get32(&p);
            if (v->playingKey < 0 || v->playingKey > 127 || v->playingChannel < 0 ||
                v->playingChannel > 15 || (v->heldSustain & ~1) || ri >= (uint32_t)pr->regionNum)
                return -1;
            struct tsf_region *r = &pr->regions[ri];
            v->region = r;
            v->pitchInputTimecents = getd(&p);
            v->pitchOutputFactor = getd(&p);
            v->sourceSamplePosition = getd(&p);
            v->noteGainDB = getf(&p);
            v->panFactorLeft = getf(&p);
            v->panFactorRight = getf(&p);
            v->playIndex = get32(&p);
            v->loopStart = get32(&p);
            v->loopEnd = get32(&p);
            /* positions within the region's samples; the loop as note-on and note-off set it */
            int loop_ok = (v->loopStart == 0 && v->loopEnd == 0) ||
                          (v->loopStart == r->loop_start &&
                           (v->loopEnd == r->loop_end || v->loopEnd == v->loopStart));
            if (!fin(v->pitchInputTimecents, 50000.0) || !fin(v->pitchOutputFactor, 1e6) ||
                !(v->pitchOutputFactor > 0.0) || !(v->sourceSamplePosition >= 0.0) ||
                !(v->sourceSamplePosition <= (double)r->end) || !fin(v->noteGainDB, 1000.0) ||
                !fin(v->panFactorLeft, 2.0) || !fin(v->panFactorRight, 2.0) || !loop_ok)
                return -1;
            if (!get_env(&p, &v->ampenv) || !get_env(&p, &v->modenv))
                return -1;
            struct tsf_voice_lowpass *lp = &v->lowpass;
            double c[7];
            for (int k = 0; k < 7; k++)
                if (!fin(c[k] = getd(&p), 1e6))
                    return -1;
            lp->QInv = c[0], lp->a0 = c[1], lp->a1 = c[2], lp->b1 = c[3], lp->b2 = c[4];
            lp->z1 = c[5], lp->z2 = c[6];
            uint32_t act = get32(&p);
            if (act > 1)
                return -1;
            lp->active = (TSF_BOOL)act;
            if (!get_lfo(&p, &v->modlfo) || !get_lfo(&p, &v->viblfo))
                return -1;
        }
        p = rec + GM_VOICE_BYTES;
    }
    memcpy(f->voices, vs, sizeof vs);
    f->voicePlayIndex = play_index;
    return 0;
}
