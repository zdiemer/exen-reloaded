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
    /* Melody playback (data in the ExEn melody format, docs/re/NATIVES.md §3.2). */
    void (*melody_play)(void *user, const uint8_t *data, int len);
    void (*melody_stop)(void *user);
    /* Diagnostic text from the VM (Debug natives, warnings). */
    void (*log)(void *user, const char *msg);
} ExenHost;

typedef struct ExenConfig {
    int screen_width, screen_height; /* device profile; 0 = 101x80 */
    uint32_t heap_bytes;             /* 0 = default */
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

/* Persistent storage (the 2048-byte EEPROM image the original keeps in flash/eeprom.dat). */
uint8_t *exen_eeprom(ExenVM *vm);

/* Diagnostics: number of calls made to each native index so far. */
uint32_t exen_native_calls(ExenVM *vm, int index, const char **name);

#ifdef __cplusplus
}
#endif

#endif
