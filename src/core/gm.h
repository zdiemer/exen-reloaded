/* General MIDI melody voice: the melody's MIDI events played from a SoundFont with
 * TinySoundFont (vendor/tinysoundfont/tsf.h), like the original simulator's notes on Windows'
 * General MIDI synth. See gm.c and docs/re/NATIVES.md §3.2.
 *
 * The SoundFont is host data (exen_audio_set_soundfont): the synth keeps its own parsed copy.
 * Its voices are game state like the other melody voices; gm_save / gm_load store them in save
 * states (state.c, GMSF chunk) and restore them exactly when the loading VM has the same
 * SoundFont at the same output rate.
 */
#ifndef EXEN_GM_H
#define EXEN_GM_H

#include <stddef.h>
#include <stdint.h>

typedef struct GmSynth GmSynth;

/* Voices: the sounding note and the release tails of the last few (a GM preset can use more
 * than one voice per note). When all are busy, the voice furthest into its release is taken. */
#define GM_VOICES 16
/* Size of the gm_save record. */
#define GM_STATE_BYTES (6 * 4 + GM_VOICES * 264)

/* Parse the SoundFont 2 file `sf2` (copied) for output at `rate` Hz; NULL if it is not a usable
 * SoundFont or memory runs out. All 16 channels start on program 0, channel 9 on the drum kit. */
GmSynth *gm_create(const void *sf2, size_t len, int rate);
void gm_free(GmSynth *g);

/* MIDI events. The velocity is the player's, fixed at 0x40. */
void gm_note_on(GmSynth *g, int ch, int note);
void gm_note_off(GmSynth *g, int ch, int note);
void gm_all_off(GmSynth *g);            /* every sounding note to its release */
void gm_silence(GmSynth *g);            /* every voice cut at once */
/* Each channel's instrument from the melody's program table: program[ch] & 0x7f on bank 0,
 * channel 9 the drum kit of that number (bank 128). Missing presets fall back to program 0. */
void gm_set_programs(GmSynth *g, const uint8_t program[16]);

int gm_active(const GmSynth *g);        /* any voice sounding */
uint32_t gm_notes(const GmSynth *g);    /* note-ons received since the last gm_silence (tests) */
/* Render `frames` interleaved stereo frames into lr (overwritten), about -1..1. */
void gm_render(GmSynth *g, float *lr, int frames);

/* Save states: a GM_STATE_BYTES record of the voices. gm_load returns 0, or < 0 and leaves the
 * synth unchanged if the record is malformed or was saved with another SoundFont or rate. */
size_t gm_save(const GmSynth *g, uint8_t *out, size_t cap);
int gm_load(GmSynth *g, const uint8_t *in, size_t len);

#endif
