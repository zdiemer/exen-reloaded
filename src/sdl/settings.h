/* The SDL frontend's settings: settings.ini in the data directory, beside keys.ini.
 *
 *   ; exen-reloaded settings (written by the app)
 *   [video]
 *   scale = 4                 ; window scale 1-6
 *   fullscreen = 0
 *   [sound]
 *   voice = ringtone          ; or gm (General MIDI)
 *   volume = 100              ; 0-100
 *   mute = 0
 *   [sms]
 *   mode = server             ; server, delivered or offline
 *   [files]
 *   last-dir = C:\Games\ExEn\
 *   recent = C:\Games\ExEn\Worms.exn      ; most recent first, up to 10
 *   [handset]
 *   Worms = 128x128           ; per-game screen override, by the game's save file stem
 *
 * The app writes the file whenever a setting changes from the menus. Unknown sections and keys
 * are ignored, so a file from a newer version still loads. Command-line options override the
 * file for one run and are not written back unless changed from the menus.
 */
#ifndef EXEN_SDL_SETTINGS_H
#define EXEN_SDL_SETTINGS_H

#include <stddef.h>

#define SETTINGS_MAX_RECENT 10
#define SETTINGS_MAX_HANDSETS 64
#define SETTINGS_PATH_MAX 1024

typedef struct {
    char game[80]; /* persist_game_key */
    int w, h;
} SettingsHandset;

typedef struct {
    int scale;      /* 1-6 */
    int fullscreen;
    int voice;      /* EXEN_VOICE_* */
    int volume;     /* 0-100 */
    int mute;
    int sms;        /* EXEN_SMS_* */
    char last_dir[SETTINGS_PATH_MAX];
    char recent[SETTINGS_MAX_RECENT][SETTINGS_PATH_MAX];
    int nrecent;
    SettingsHandset handset[SETTINGS_MAX_HANDSETS];
    int nhandset;
} Settings;

void settings_defaults(Settings *s);

/* Apply settings text over `s`. Returns the number of invalid lines (logged, skipped). */
int settings_parse(Settings *s, const char *text);
/* The settings as text (SDL_malloc'd; SDL_free it). */
char *settings_format(const Settings *s);

/* Load `path` over the defaults. Returns 1 if the file was read, 0 if it does not exist. */
int settings_load(Settings *s, const char *path);
/* Write `path`. Returns 0, or -1 on error (logged). */
int settings_save(const Settings *s, const char *path);

/* Put `path` at the top of the recent list, removing an older entry for the same file and
 * dropping the oldest beyond SETTINGS_MAX_RECENT. */
void settings_add_recent(Settings *s, const char *path);
void settings_remove_recent(Settings *s, int index);

/* Per-game handset screen override: returns 1 and the size if `game` has one. Setting w or h to
 * 0 removes the override. */
int settings_get_handset(const Settings *s, const char *game, int *w, int *h);
void settings_set_handset(Settings *s, const char *game, int w, int h);

#endif
