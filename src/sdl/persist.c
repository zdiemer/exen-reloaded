/* EEPROM files and save state slots for the SDL frontend (persist.h). */
#include "persist.h"

#include "paths.h"

#define STATUS_MS 2500

static ExenVM *g_vm;
static char g_key[80];      /* per-game file stem */
static char g_eep[1024];    /* saves/<key>.eep */
static int g_slot;
static char g_status[128];
static uint64_t g_status_until; /* SDL_GetTicksNS deadline, 0 = no status */

void persist_status(const char *fmt, ...)
{
    char msg[100];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SDL_snprintf(g_status, sizeof g_status, " [%s]", msg);
    g_status_until = SDL_GetTicksNS() + (uint64_t)STATUS_MS * 1000000u;
}

const char *persist_title(void)
{
    if (!g_status_until || SDL_GetTicksNS() >= g_status_until)
        return "";
    return g_status;
}

int persist_title_wait_ms(void)
{
    if (!g_status_until)
        return -1;
    uint64_t now = SDL_GetTicksNS();
    if (now >= g_status_until) {
        g_status_until = 0;
        return 0;
    }
    return (int)((g_status_until - now + 999999u) / 1000000u);
}

static void slot_path(char *buf, size_t n, int slot, const char *ext)
{
    SDL_snprintf(buf, n, "%sstates/%s/slot%d.%s", exen_data_dir(), g_key, slot, ext);
}

void persist_game_key(const uint8_t *exn, size_t len, char *out, size_t n)
{
    ExenGameInfo gi;
    int m = exen_game_info(exn, len, &gi);
    size_t k = 0;
    if (m == EXEN_GAME_EXACT)
        for (const char *s = gi.title; *s && k + 1 < n; s++) {
            char c = *s;
            int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == ' ';
            out[k++] = ok ? c : '_';
        }
    while (k && (out[k - 1] == ' ' || out[k - 1] == '.'))
        k--; /* Windows drops trailing dots and spaces */
    out[k] = 0;
    if (!k || out[0] == '.')
        SDL_snprintf(out, n, "%08x", (unsigned)SDL_crc32(0, exn, len));
}

void persist_open(ExenVM *vm, const char *sim, const uint8_t *exn, size_t len)
{
    g_vm = vm;
    persist_game_key(exn, len, g_key, sizeof g_key);
    SDL_snprintf(g_eep, sizeof g_eep, "%ssaves/%s.eep", exen_data_dir(), g_key);
    size_t n = 0;
    void *img = SDL_LoadFile(g_eep, &n);
    if (img && n == EXEN_EEPROM_SIZE) {
        SDL_memcpy(exen_eeprom(vm), img, EXEN_EEPROM_SIZE);
        SDL_free(img);
        return;
    }
    if (img)
        SDL_Log("%s: not a %d-byte EEPROM image; starting from the defaults", g_eep, EXEN_EEPROM_SIZE);
    SDL_free(img);
    /* defaults: flash/eeprom.dat beside gensimu.exe */
    char path[1100];
    path[0] = 0;
    if (sim) {
        char dir[1024];
        SDL_strlcpy(dir, sim, sizeof dir);
        dir[exen_path_basename(dir) - dir] = 0;
        SDL_snprintf(path, sizeof path, "%sflash/eeprom.dat", dir);
    }
    void *def = path[0] ? SDL_LoadFile(path, &n) : NULL;
    if (!def)
        SDL_Log("no %s: the EEPROM starts out blank", path[0] ? path : "flash/eeprom.dat");
    exen_eeprom_reset(vm, def, def ? n : 0); /* calls eeprom_changed, which writes the file */
    SDL_free(def);
}

void persist_eeprom_changed(void *user)
{
    (void)user;
    if (g_vm && g_eep[0])
        exen_write_file(g_eep, exen_eeprom(g_vm), EXEN_EEPROM_SIZE);
}

void persist_close(void)
{
    if (!g_vm)
        return;
    persist_eeprom_changed(NULL);
    g_vm = NULL;
    g_key[0] = 0;
    g_eep[0] = 0;
}

int persist_slot(void)
{
    return g_slot;
}

void persist_select_slot(int slot)
{
    g_slot = ((slot % PERSIST_NSLOTS) + PERSIST_NSLOTS) % PERSIST_NSLOTS;
    if (!g_vm) {
        persist_status("slot %d", g_slot);
        return;
    }
    PersistSlotInfo info;
    persist_status("slot %d%s", g_slot, persist_slot_info(g_slot, &info) ? "" : " (empty)");
}

static void save_thumb(int slot, const uint8_t *px, int w, int h)
{
    char path[1200];
    slot_path(path, sizeof path, slot, "thumb");
    if (!px || w <= 0 || h <= 0 || w > 0xffff || h > 0xffff) {
        SDL_RemovePath(path);
        return;
    }
    size_t n = 8 + (size_t)w * (size_t)h;
    uint8_t *b = SDL_malloc(n);
    if (!b)
        return;
    SDL_memcpy(b, "EXTH", 4);
    b[4] = (uint8_t)w;
    b[5] = (uint8_t)(w >> 8);
    b[6] = (uint8_t)h;
    b[7] = (uint8_t)(h >> 8);
    SDL_memcpy(b + 8, px, n - 8);
    exen_write_file(path, b, n);
    SDL_free(b);
}

int persist_save(int slot, const uint8_t *thumb, int w, int h)
{
    if (!g_vm)
        return 0;
    if (exen_halted(g_vm)) {
        persist_status("slot %d: not saved, the game has stopped", slot);
        return 0;
    }
    size_t n = exen_state_size(g_vm);
    void *b = n ? SDL_malloc(n) : NULL;
    int r = b ? exen_state_save(g_vm, b, n) : -1;
    char path[1200];
    slot_path(path, sizeof path, slot, "exst");
    int ok = 0;
    if (r < 0) {
        persist_status("slot %d: save failed", slot);
        SDL_Log("save state: %s", exen_error(g_vm));
    } else if (exen_write_file(path, b, (size_t)r) < 0) {
        persist_status("slot %d: cannot write the file", slot);
    } else {
        save_thumb(slot, thumb, w, h);
        persist_status("slot %d: saved", slot);
        ok = 1;
    }
    SDL_free(b);
    return ok;
}

int persist_load(int slot)
{
    if (!g_vm)
        return 0;
    char path[1200];
    slot_path(path, sizeof path, slot, "exst");
    size_t n = 0;
    void *b = SDL_LoadFile(path, &n);
    if (!b) {
        persist_status("slot %d: empty", slot);
        return 0;
    }
    int r = exen_state_load(g_vm, b, n);
    SDL_free(b);
    if (r < 0) {
        SDL_Log("%s: %s", path, exen_error(g_vm));
        persist_status("slot %d: cannot load (see log)", slot);
        return 0;
    }
    persist_status("slot %d: loaded", slot);
    return 1;
}

int persist_delete(int slot)
{
    if (!g_vm)
        return 0;
    char path[1200];
    slot_path(path, sizeof path, slot, "thumb");
    SDL_RemovePath(path);
    slot_path(path, sizeof path, slot, "exst");
    int had = SDL_GetPathInfo(path, NULL);
    if (had && !SDL_RemovePath(path)) {
        persist_status("slot %d: cannot delete", slot);
        return 0;
    }
    persist_status("slot %d: %s", slot, had ? "deleted" : "empty");
    return had;
}

int persist_slot_info(int slot, PersistSlotInfo *info)
{
    SDL_zerop(info);
    if (!g_vm)
        return 0;
    char path[1200];
    slot_path(path, sizeof path, slot, "exst");
    SDL_PathInfo pi;
    if (SDL_GetPathInfo(path, &pi) && pi.type == SDL_PATHTYPE_FILE) {
        info->exists = 1;
        info->mtime = pi.modify_time;
    }
    return info->exists;
}

uint8_t *persist_load_thumb(int slot, int *w, int *h)
{
    if (!g_vm)
        return NULL;
    char path[1200];
    slot_path(path, sizeof path, slot, "thumb");
    size_t n = 0;
    uint8_t *b = SDL_LoadFile(path, &n);
    if (!b)
        return NULL;
    int tw = n >= 8 ? b[4] | b[5] << 8 : 0, th = n >= 8 ? b[6] | b[7] << 8 : 0;
    if (n < 8 || SDL_memcmp(b, "EXTH", 4) || !tw || !th || n != 8 + (size_t)tw * (size_t)th) {
        SDL_free(b);
        return NULL;
    }
    SDL_memmove(b, b + 8, n - 8);
    *w = tw;
    *h = th;
    return b;
}
