/* Remappable keyboard and gamepad input for the SDL frontend.
 *
 * Physical inputs (keyboard keys, gamepad buttons, gamepad axis directions) are bound to phone
 * keys: the ExEn key codes '0'..'9', '*', '#', OK (-8, Gamelet.KEY_VAL), CLEAR (-7) and the
 * optional joypad codes KEY_PAD_* (exen.h). Several inputs may share one phone key, and one
 * input may drive several phone keys. A phone key is pressed when the first of its inputs goes
 * down and released when the last one goes up, across all keyboards and gamepads, so the VM
 * never sees a duplicate press or release.
 *
 * Bindings come from a small INI-like text file (see input_format_config for the format);
 * anything it does not mention keeps the defaults.
 */
#ifndef EXEN_SDL_INPUT_H
#define EXEN_SDL_INPUT_H

#include <SDL3/SDL.h>
#include <stddef.h>

enum { INPUT_KEY = 1, INPUT_BUTTON, INPUT_AXIS };

#define INPUT_MAX_BINDINGS 256
#define INPUT_MAX_ACTIVE 64
#define INPUT_MAX_PADS 16
#define INPUT_NPHONE 18

typedef struct {
    int kind;  /* INPUT_* */
    int code;  /* SDL_Keycode, SDL_GamepadButton or SDL_GamepadAxis */
    int dir;   /* INPUT_AXIS: +1 or -1, otherwise 0 */
    int phone; /* ExEn key code */
} InputBinding;

typedef struct {
    int kind, dev, code, dir;
} InputSource;

/* Receives every phone key transition. */
typedef void (*InputKeyFn)(void *user, int code, int pressed);

typedef struct {
    InputBinding bind[INPUT_MAX_BINDINGS];
    int nbind;
    /* inputs currently down; dev is 0 for the keyboard (all keyboards count as one), else the
     * gamepad's SDL_JoystickID */
    InputSource active[INPUT_MAX_ACTIVE];
    int nactive;
    int held[INPUT_NPHONE]; /* number of active inputs holding each phone key */
    int axis_press, axis_release; /* thresholds, 0..32767, release <= press */
    InputKeyFn sink;
    void *user;
    SDL_Gamepad *pads[INPUT_MAX_PADS];
    int npads;
} Input;

/* Default bindings, no gamepads open. */
void input_init(Input *in, InputKeyFn sink, void *user);

/* Apply a config text over the current bindings. Each section that appears replaces that
 * device's bindings. Invalid entries are logged (SDL_Log, prefixed by `origin`) and skipped.
 * Returns the number of invalid entries. */
int input_parse_config(Input *in, const char *text, const char *origin);

/* Load `path` if it exists. Returns 1 if loaded, 0 if there is no such file. */
int input_load_config(Input *in, const char *path);

/* Load `path`; if it does not exist and `create` is set, write the current bindings (the
 * defaults) there. Returns 1 if loaded, 2 if created, 0 if the defaults are in use. */
int input_load_or_create(Input *in, const char *path, int create);

/* The current bindings as config text (SDL_malloc'd; SDL_free it). `header` adds the
 * explanatory comment block written at the top of a new config file. */
char *input_format_config(const Input *in, int header);

/* Phone key names, as used in the config file ("5", "star", "ok", ...). */
const char *input_phone_name(int code);
int input_phone_from_name(const char *name); /* 0 if unknown */

/* --- editing, for the controls panel (ui.c) --- */

/* The phone key at `index` (0..INPUT_NPHONE-1, keypad order), 0 if out of range. */
int input_phone_at(int index);
/* A label for people: "2 (up)", "* (star)", "OK (select)", ... */
const char *input_phone_label(int code);
/* The bindings of phone key `code` on the keyboard (pad = 0) or on the gamepads (pad = 1) as
 * text in the config syntax ("Up, Keypad 8, W"); "" if there are none. */
void input_describe(const Input *in, int code, int pad, char *buf, size_t n);
/* One input as text in the config syntax ("Keypad 8", "south", "lefty-"). */
void input_describe_source(int kind, int code, int dir, char *buf, size_t n);
/* Bind an input to phone key `code`. Returns 1 if added, 0 if it already was bound to it or
 * the table is full. Releases every held key first, as do input_unbind and
 * input_reset_defaults. */
int input_bind(Input *in, int kind, int code, int dir, int phone);
/* Remove the bindings of phone key `code` on the keyboard (pad = 0) or the gamepads (pad = 1). */
void input_unbind(Input *in, int code, int pad);
/* Back to the built-in bindings and stick thresholds. */
void input_reset_defaults(Input *in);
/* Write the bindings to `path` with the explanatory header (atomically). Returns 0 or -1. */
int input_save_config(const Input *in, const char *path);

/* Feed an SDL event. Handles key (auto-repeat ignored), gamepad button, axis and hotplug
 * events. Returns 1 if the event was a bound input or a gamepad event. */
int input_handle_event(Input *in, const SDL_Event *ev);

/* Lower-level entry points (used by input_handle_event and the tests). */
void input_source(Input *in, int kind, int dev, int code, int dir, int down);
void input_axis(Input *in, int dev, int axis, int value);
void input_release_device(Input *in, int kind, int dev); /* kind 0: any kind */
void input_release_all(Input *in);

/* Start gamepad support (SDL_INIT_GAMEPAD); pads then arrive as SDL_EVENT_GAMEPAD_ADDED. */
void input_open_gamepads(Input *in);
void input_close_gamepads(Input *in);

#endif
