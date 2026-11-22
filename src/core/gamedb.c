/* Per-game defaults (data/gamedb.tsv): which handset screen each known game targets. */
#include <string.h>

#include "vm.h"

typedef struct {
    uint32_t crc;
    const char *main_class, *title;
    int width, height;
} GameDbEntry;

#include "gamedb_gen.h"

#define NGAMES (sizeof gamedb / sizeof gamedb[0])

static void apply(ExenGameInfo *out, const GameDbEntry *e, int match)
{
    out->match = match;
    out->width = e->width;
    out->height = e->height;
    strncpy(out->title, e->title, sizeof out->title - 1);
}

int exen_game_info(const uint8_t *exn, size_t len, ExenGameInfo *out)
{
    memset(out, 0, sizeof *out);
    out->width = EXEN_DEFAULT_WIDTH;
    out->height = EXEN_DEFAULT_HEIGHT;
    if (len < 0x34 || memcmp(exn, "NEXE", 4) != 0)
        return EXEN_GAME_UNKNOWN;
    memcpy(out->main_class, exn + 0x14, 32);
    out->main_class[32] = 0;
    strncpy(out->title, out->main_class, sizeof out->title - 1);

    uint32_t crc = exen_crc32(exn, len);
    for (size_t i = 0; i < NGAMES; i++)
        if (gamedb[i].crc == crc) {
            apply(out, &gamedb[i], EXEN_GAME_EXACT);
            return out->match;
        }
    /* Another dump of a known game: trust the main class only if every entry with that class
     * agrees on the screen (e.g. the three Tomb Raider titles share one class name). */
    const GameDbEntry *first = NULL;
    for (size_t i = 0; i < NGAMES; i++) {
        const GameDbEntry *e = &gamedb[i];
        if (strcmp(e->main_class, out->main_class))
            continue;
        if (first && (e->width != first->width || e->height != first->height))
            return EXEN_GAME_UNKNOWN;
        if (!first)
            first = e;
    }
    if (first)
        apply(out, first, EXEN_GAME_SAME_MAIN_CLASS);
    return out->match;
}
