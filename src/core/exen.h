/* exen-reloaded core: public API.
 *
 * The core is single-threaded and platform-free. A frontend creates a VM, loads the framework
 * (from the original simulator executable) and a game, boots it, then drives it with timer
 * ticks and key events. Every call into the VM runs to completion, as on the original
 * hardware; the frontend owns pacing and sleeps between events.
 */
#ifndef EXEN_H
#define EXEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ExenVM ExenVM;

/* Phone key codes as delivered to Gamelet.onKeyPress (docs/re/NATIVES.md, keys). */
enum {
    EXEN_KEY_0 = '0', EXEN_KEY_1, EXEN_KEY_2, EXEN_KEY_3, EXEN_KEY_4,
    EXEN_KEY_5, EXEN_KEY_6, EXEN_KEY_7, EXEN_KEY_8, EXEN_KEY_9,
    EXEN_KEY_STAR = '*', EXEN_KEY_POUND = '#',
    EXEN_KEY_CLEAR = -7, EXEN_KEY_OK = -8,
    /* Gamelet.KEY_PAD_*, for handsets with a joypad. gensimu never sends these: it maps the
     * PC arrow keys to '4' '2' '6' '8' (docs/re/NATIVES.md §4.2). */
    EXEN_KEY_PAD_UP = -1, EXEN_KEY_PAD_LEFT = -2, EXEN_KEY_PAD_RIGHT = -5, EXEN_KEY_PAD_DOWN = -6,
};

typedef struct ExenHost {
    void *user;
    /* The game finished a frame: `pixels` is width*height RGB332 bytes. */
    void (*present)(void *user, const uint8_t *pixels, int width, int height);
    /* Start the periodic game tick (period in ms), or stop it (period < 0). */
    void (*set_timer)(void *user, int period_ms);
    /* Milliseconds since the game started. */
    uint32_t (*ticks_ms)(void *user);
    /* Optional notifications: a melody started (data in the ExEn melody format,
     * docs/re/NATIVES.md §3.2) or was stopped. The core renders the audio itself
     * (exen_audio_render); these are for diagnostics. */
    void (*melody_play)(void *user, const uint8_t *data, int len);
    void (*melody_stop)(void *user);
    /* Diagnostic text from the VM (Debug natives, warnings). */
    void (*log)(void *user, const char *msg);
} ExenHost;

typedef struct ExenConfig {
    int screen_width, screen_height; /* device profile; 0 = 101x80 */
    uint32_t heap_bytes;             /* 0 = default */
    int audio_rate;                  /* output sample rate in Hz (8000..192000); 0 = 44100 */
    ExenHost host;
} ExenConfig;

/* Lifecycle. Functions returning int give 0 on success, negative on error; exen_error()
 * describes the last failure. */
ExenVM *exen_create(const ExenConfig *cfg);
void exen_destroy(ExenVM *vm);
const char *exen_error(ExenVM *vm);

/* Load the framework classes out of gensimu.exe, then the game. */
int exen_load_framework(ExenVM *vm, const uint8_t *gensimu_exe, size_t len);
int exen_load_game(ExenVM *vm, const uint8_t *exn, size_t len);
/* Run vm.sys.Bootstrap.init(main class), i.e. construct the Gamelet and call onInit(). */
int exen_boot(ExenVM *vm);

/* Events. Each runs the corresponding Bootstrap entry point to completion. */
int exen_tick(ExenVM *vm);
int exen_key(ExenVM *vm, int keycode, int pressed);

/* Non-zero once the game has exited or the VM has stopped on a fatal error. */
int exen_halted(ExenVM *vm);

/* Audio. The core sequences and synthesizes the game's melodies (Gamelet.playMelody) and the
 * host pulls the result as PCM: interleaved stereo int16 (left, right) at the configured rate.
 * The sequencer runs on the audio clock, so the host renders in step with real time (or with
 * its virtual clock): audio rendered after a tick or key event plays what that event started.
 * exen_audio_render always fills `frames` frames (silence when idle) and returns `frames`.
 * Like every other call it must not run concurrently with the rest of the API. */
int exen_audio_render(ExenVM *vm, int16_t *out, int frames);
/* Non-zero while a melody plays or a note is still fading out. When it is zero, the host may
 * stop rendering until the next event; rendering then resumes where the game started. */
int exen_audio_active(ExenVM *vm);
/* Output volume 0..100 (default 100, 0 mutes); applied as a loudness (square-law) curve.
 * Muting keeps the sequencer running, so melodies stay in time. */
void exen_audio_set_volume(ExenVM *vm, int volume);
int exen_audio_rate(ExenVM *vm);

/* Persistent storage (the 2048-byte EEPROM image the original keeps in flash/eeprom.dat). */
uint8_t *exen_eeprom(ExenVM *vm);

/* Diagnostics: number of calls made to each native index so far. */
uint32_t exen_native_calls(ExenVM *vm, int index, const char **name);

#ifdef __cplusplus
}
#endif

#endif
