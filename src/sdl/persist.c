/* EEPROM files and save state slots for the SDL frontend (persist.h). */
#include "persist.h"

#include "paths.h"

#define STATUS_MS 2500
#define NSLOTS 10

static ExenVM *g_vm;
static char g_key[80];      /* per-game file stem */
static char g_eep[1024];    /* saves/<key>.eep */
static int g_slot;
static char g_status[96];
static uint64_t g_status_until; /* SDL_GetTicksNS deadline, 0 = no status */

static void status(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void status(const char *fmt, ...)
{
    char msg[80];
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

/* Write `len` bytes to `path` through a temporary file, creating the directory. */
static int write_file(const char *path, const void *data, size_t len)
{
    char dir[1024], tmp[1100];
    SDL_strlcpy(dir, path, sizeof dir);
    char *cut = SDL_strrchr(dir, '/'), *cut2 = SDL_strrchr(dir, '\\');
    if (cut2 > cut)
        cut = cut2;
    if (cut) {
        *cut = 0;
        SDL_CreateDirectory(dir); /* with any missing parents; fails harmlessly if it exists */
    }
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (!SDL_SaveFile(tmp, data, len)) {
        SDL_Log("cannot write %s: %s", tmp, SDL_GetError());
        return -1;
    }
    if (!SDL_RenamePath(tmp, path)) {
        SDL_Log("cannot rename %s: %s", tmp, SDL_GetError());
        SDL_RemovePath(tmp);
        return -1;
    }
    return 0;
}

static void slot_path(char *buf, size_t n, int slot)
{
    SDL_snprintf(buf, n, "%sstates/%s/slot%d.exst", exen_data_dir(), g_key, slot);
}

/* The game's file stem: its database title made file-name safe, else its CRC-32. */
static void make_key(const uint8_t *exn, size_t len)
{
    ExenGameInfo gi;
    int m = exen_game_info(exn, len, &gi);
    size_t k = 0;
    if (m == EXEN_GAME_EXACT)
        for (const char *s = gi.title; *s && k + 1 < sizeof g_key; s++) {
            char c = *s;
            int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == ' ';
            g_key[k++] = ok ? c : '_';
        }
    while (k && (g_key[k - 1] == ' ' || g_key[k - 1] == '.'))
        k--; /* Windows drops trailing dots and spaces */
    g_key[k] = 0;
    if (!k || g_key[0] == '.')
        SDL_snprintf(g_key, sizeof g_key, "%08x", (unsigned)SDL_crc32(0, exn, len));
}

void persist_open(ExenVM *vm, const char *sim, const uint8_t *exn, size_t len)
{
    g_vm = vm;
    make_key(exn, len);
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
        char *cut = SDL_strrchr(dir, '/'), *cut2 = SDL_strrchr(dir, '\\');
        if (cut2 > cut)
            cut = cut2;
        if (cut)
            cut[1] = 0;
        else
            dir[0] = 0;
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
        write_file(g_eep, exen_eeprom(g_vm), EXEN_EEPROM_SIZE);
}

void persist_close(void)
{
    persist_eeprom_changed(NULL);
    g_vm = NULL;
}

static void save_slot(void)
{
    if (exen_halted(g_vm)) {
        status("slot %d: not saved, the game has stopped", g_slot);
        return;
    }
    size_t n = exen_state_size(g_vm);
    void *b = n ? SDL_malloc(n) : NULL;
    int r = b ? exen_state_save(g_vm, b, n) : -1;
    char path[1200];
    slot_path(path, sizeof path, g_slot);
    if (r < 0)
        status("slot %d: save failed", g_slot);
    else if (write_file(path, b, (size_t)r) < 0)
        status("slot %d: cannot write the file", g_slot);
    else
        status("slot %d: saved", g_slot);
    if (r < 0)
        SDL_Log("save state: %s", exen_error(g_vm));
    SDL_free(b);
}

int persist_load_slot(void)
{
    char path[1200];
    slot_path(path, sizeof path, g_slot);
    size_t n = 0;
    void *b = SDL_LoadFile(path, &n);
    if (!b) {
        status("slot %d: empty", g_slot);
        return 0;
    }
    int r = exen_state_load(g_vm, b, n);
    SDL_free(b);
    if (r < 0) {
        SDL_Log("%s: %s", path, exen_error(g_vm));
        status("slot %d: cannot load (see log)", g_slot);
        return 0;
    }
    status("slot %d: loaded", g_slot);
    return 1;
}

int persist_hotkey(const SDL_Event *ev)
{
    if (ev->type != SDL_EVENT_KEY_DOWN && ev->type != SDL_EVENT_KEY_UP)
        return PERSIST_NONE;
    SDL_Keycode k = ev->key.key;
    if (k != SDLK_F5 && k != SDLK_F6 && k != SDLK_F7 && k != SDLK_F8)
        return PERSIST_NONE;
    if (ev->type != SDL_EVENT_KEY_DOWN || ev->key.repeat || !g_vm)
        return PERSIST_HANDLED; /* ours, nothing to do */
    switch (k) {
    case SDLK_F5:
        save_slot();
        return PERSIST_HANDLED;
    case SDLK_F6:
    case SDLK_F7: {
        g_slot = (g_slot + (k == SDLK_F7 ? 1 : NSLOTS - 1)) % NSLOTS;
        char path[1200];
        slot_path(path, sizeof path, g_slot);
        status("slot %d%s", g_slot, SDL_GetPathInfo(path, NULL) ? "" : " (empty)");
        return PERSIST_HANDLED;
    }
    default:
        return PERSIST_LOADING;
    }
}
