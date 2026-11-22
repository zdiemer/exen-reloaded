/* Persistence for the SDL frontend: the game's EEPROM image and save state slots.
 *
 * Files live in the data directory (paths.h, exen_data_dir: beside the executable if it is
 * writable, else the per-user directory):
 *   saves/<game>.eep            the 2048-byte EEPROM image (Gamelet.saveCtx data, settings)
 *   states/<game>/slotN.exst    save state slots 0-9 (exen_state_save)
 * <game> is the title from the game database for a known game, else the CRC-32 of the .exn
 * file in hex.
 *
 * The EEPROM is written whenever the core reports a change (ExenHost.eeprom_changed: after
 * every Gamelet.saveCtx, like the original's flash/EEPROM.DAT, and after a state load) and on
 * exit. A game without an image starts from the simulator's flash/eeprom.dat (found beside
 * gensimu.exe) for the config area (nickname, currency, SMS prices) with an empty game record
 * (exen_eeprom_reset), or from zeros if that file is missing.
 *
 * Hotkeys: F5 saves to the current slot, F8 loads it, F6/F7 select the previous/next slot.
 * The outcome shows in the window title for a few seconds. Files are written to a temporary
 * name and renamed, so a crash never leaves a half-written slot.
 *
 * One game per process: the module keeps its state in file-scope variables.
 */
#ifndef EXEN_SDL_PERSIST_H
#define EXEN_SDL_PERSIST_H

#include <SDL3/SDL.h>

#include "exen.h"

/* After exen_load_game, before exen_boot: pick the game's file names and load (or create) its
 * EEPROM. `sim` is the path of gensimu.exe (for flash/eeprom.dat), `exn` the game file. */
void persist_open(ExenVM *vm, const char *sim, const uint8_t *exn, size_t len);

/* ExenHost.eeprom_changed: writes the EEPROM file. `user` is ignored. */
void persist_eeprom_changed(void *user);

/* Write the EEPROM; call before exen_destroy. */
void persist_close(void);

enum { PERSIST_NONE = 0, PERSIST_HANDLED, PERSIST_LOADING };
/* Handle F5-F8. Returns PERSIST_NONE for any other event; PERSIST_LOADING for an F8 press
 * that is about to load a state (the caller then releases held keys and calls
 * persist_load_slot), else PERSIST_HANDLED (the title text changed). */
int persist_hotkey(const SDL_Event *ev);

/* Load the current slot. Returns 1 if a state was loaded. */
int persist_load_slot(void);

/* Status for the window title (" [slot 3: saved]"), "" when there is none. */
const char *persist_title(void);
/* Milliseconds until the status expires (-1: none shown). When it reaches 0, the caller
 * refreshes the title and persist_title() returns "". */
int persist_title_wait_ms(void);

#endif
