/* Persistence for the SDL frontend: the game's EEPROM image and save state slots.
 *
 * Files live in the data directory (paths.h, exen_data_dir: beside the executable if it is
 * writable, else the per-user directory):
 *   saves/<game>.eep            the 2048-byte EEPROM image (Gamelet.saveCtx data, settings)
 *   states/<game>/slotN.exst    save state slots 0-9 (exen_state_save)
 *   states/<game>/slotN.thumb   the screen when the slot was saved, for the state manager:
 *                               "EXTH", width and height (u16 little-endian), then
 *                               width*height RGB332 pixels
 * <game> is the title from the game database for a known game, else the CRC-32 of the .exn
 * file in hex (persist_game_key).
 *
 * The EEPROM is written whenever the core reports a change (ExenHost.eeprom_changed: after
 * every Gamelet.saveCtx, like the original's flash/EEPROM.DAT, and after a state load) and when
 * the game is closed. A game without an image starts from the simulator's flash/eeprom.dat
 * (found beside gensimu.exe) for the config area (nickname, currency, SMS prices) with an empty
 * game record (exen_eeprom_reset), or from zeros if that file is missing.
 *
 * Save, load and slot changes report their outcome as a short status that the window title
 * shows for a few seconds (persist_title). Files are written to a temporary name and renamed,
 * so a crash never leaves a half-written slot.
 *
 * One game at a time: the module keeps its state in file-scope variables. persist_close
 * flushes the EEPROM and forgets the game; persist_open may then be called for another one.
 * The current slot is kept across games.
 */
#ifndef EXEN_SDL_PERSIST_H
#define EXEN_SDL_PERSIST_H

#include <SDL3/SDL.h>

#include "exen.h"

#define PERSIST_NSLOTS 10

/* The game's file stem: its database title made file-name safe, else its CRC-32 in hex. */
void persist_game_key(const uint8_t *exn, size_t len, char *out, size_t n);

/* After exen_load_game, before exen_boot: pick the game's file names and load (or create) its
 * EEPROM. `sim` is the path of gensimu.exe (for flash/eeprom.dat), `exn` the game file. */
void persist_open(ExenVM *vm, const char *sim, const uint8_t *exn, size_t len);

/* ExenHost.eeprom_changed: writes the EEPROM file. `user` is ignored. */
void persist_eeprom_changed(void *user);

/* Write the EEPROM and forget the game; call before exen_destroy. Does nothing if no game is
 * open. */
void persist_close(void);

/* The current slot (0-9), used by quick save and quick load. Selecting one reports it in the
 * title, with "(empty)" if it holds no state. */
int persist_slot(void);
void persist_select_slot(int slot);

/* Save the game's state to `slot`, with the screen `thumb` (w*h RGB332, may be NULL) for the
 * state manager. Returns 1 if saved. */
int persist_save(int slot, const uint8_t *thumb, int w, int h);
/* Load `slot`. Returns 1 if a state was loaded. */
int persist_load(int slot);
/* Delete `slot` and its thumbnail. Returns 1 if it held a state. */
int persist_delete(int slot);

typedef struct {
    int exists;
    SDL_Time mtime; /* when it was saved */
} PersistSlotInfo;
/* What `slot` holds for the open game. Returns info->exists. */
int persist_slot_info(int slot, PersistSlotInfo *info);
/* The slot's thumbnail (SDL_malloc'd w*h RGB332; SDL_free it), or NULL if it has none. */
uint8_t *persist_load_thumb(int slot, int *w, int *h);

/* Show a status in the window title for a few seconds (" [slot 3: saved]"). */
void persist_status(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
/* Status for the window title (" [slot 3: saved]"), "" when there is none. */
const char *persist_title(void);
/* Milliseconds until the status expires (-1: none shown). When it reaches 0, the caller
 * refreshes the title and persist_title() returns "". */
int persist_title_wait_ms(void);

#endif
