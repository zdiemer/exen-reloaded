/* Melody sequencer and synthesizer (Gamelet.playMelody / stopMelody).
 *
 * The sequencer reproduces the original simulator's player (gensimu.exe 0x43b192 / 0x43ad57,
 * docs/re/NATIVES.md §3.2, docs/re/natives/melody.c.txt): one MIDI-style note stream of
 * (note, duration) byte pairs, timed by a periodic timer that each event re-arms. Instead of
 * sending MIDI, the core renders the notes itself, so every frontend produces the same audio.
 * There are two voices (exen_audio_set_voice): a band-limited square "ring tone" (the default,
 * close to a 2003 handset) and a procedural piano approximating the General MIDI grand piano
 * the original simulator played through Windows' synth. Every note event goes to both voice
 * pools, which always run; the host setting only picks (and crossfades) what is heard, so
 * switching never touches the timing.
 *
 * All timing runs on the audio clock: the sequencer only advances while the host renders
 * samples (exen_audio_render). The state is plain data (the melody bytes are copied in, no
 * pointers), so a save state can copy the struct as is.
 */
#ifndef EXEN_MELODY_H
#define EXEN_MELODY_H

#include <stdint.h>

/* Longest melody kept, in bytes. The longest in the corpus is 224; longer arrays are cut. */
#define MELODY_MAX 4096
#define MELODY_VOICES 3
/* Piano voice: a monophonic melody needs the sounding note plus the release tails of the
 * last few (legato note changes overlap like on a MIDI synth). Partials per note, at most. */
#define PIANO_VOICES 4
#define PIANO_PARTIALS 16

typedef struct MelodyVoice {
    uint8_t state;          /* 0 free, 1 attack, 2 sustain/decay, 3 release */
    uint8_t note, ch, drum;
    float phase, inc;       /* oscillator phase and increment, in cycles */
    float env, rel_step;    /* envelope level and per-sample release step */
    uint32_t noise;         /* drum LFSR */
    float lp;               /* drum noise filter state */
} MelodyVoice;

/* One piano note: additive partials, each struck on two slightly detuned strings. Coefficients
 * (frequencies, decay rates) are derived from the note and the output rate when rendering, so
 * the state holds only levels and phases. */
typedef struct PianoVoice {
    uint8_t state;          /* 0 free, 1 key down, 2 released (damper on) */
    uint8_t note, ch, pad;
    float att;              /* attack ramp 0..1 */
    float ph[PIANO_PARTIALS][2];  /* string phases, in cycles */
    float e1[PIANO_PARTIALS];     /* partial level, prompt-sound component (fast decay) */
    float e2[PIANO_PARTIALS];     /* partial level, aftersound component (slow decay) */
    uint32_t noise;         /* hammer noise LFSR */
    float ne;               /* hammer noise level */
    float n1, n2;           /* hammer noise band-pass state */
} PianoVoice;

typedef struct Melody {
    /* sequencer state, mirroring the globals of the original player */
    uint8_t data[MELODY_MAX];
    uint32_t len;           /* melody length in bytes (mel_size 0x519fe4) */
    uint32_t pos;           /* byte offset of the next pair (mel_ptr 0x519fe0) */
    uint32_t events;        /* ev_count 0x4c658c */
    uint16_t prev;          /* last event byte, 0xffff before the first (0x4c6594) */
    uint8_t ch;             /* MIDI channel (0x4c6596); survives stop, as in the original */
    uint8_t cur_dur;        /* duration units of the current pair (0x519fec) */
    uint8_t playing;        /* the player timer is armed and the melody can still sound */
    uint8_t vol_l, vol_r;   /* midiOutSetVolume high bytes, 0x80 at every start */
    uint8_t program[16];    /* program per channel (tracked, not used by the timbre) */
    int32_t period_ms;      /* timer period (0x4c6598), at least 10 ms like a USER timer */
    int64_t wait;           /* time until the timer fires, in 1/1000 sample */

    /* synthesizer */
    MelodyVoice voice[MELODY_VOICES];
    float gain_l, gain_r;   /* smoothed output gains */
    float lp_l, lp_r;       /* output low-pass state (ring-tone voice and drums) */
    PianoVoice piano[PIANO_VOICES];

    /* host settings (not game state) */
    int32_t rate;           /* output sample rate, Hz */
    int32_t volume;         /* 0..100 */
    int32_t timbre;         /* EXEN_VOICE_*: which voice is heard */
    float mix;              /* 0 ring tone .. 1 piano, glides towards `timbre` */
    uint32_t melodies;      /* melodies started (diagnostics) */
} Melody;

struct ExenVM;

void melody_init(Melody *m, int rate);
/* Start `data` (copied), replacing any melody that is playing. */
void melody_play(struct ExenVM *vm, const uint8_t *data, uint32_t len);
/* EXManufMelodyStop: stop the sequencer and release the sounding note. */
void melody_stop(struct ExenVM *vm);

#endif
