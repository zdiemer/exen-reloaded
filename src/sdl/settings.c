/* settings.ini (settings.h). */
#include "settings.h"

#include <SDL3/SDL.h>

#include "exen.h"
#include "paths.h"

void settings_defaults(Settings *s)
{
    SDL_zerop(s);
    s->scale = 4;
    s->voice = EXEN_VOICE_RINGTONE;
    s->volume = 100;
    s->sms = EXEN_SMS_SERVER;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + SDL_strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static int parse_int(const char *v, int lo, int hi, int *out)
{
    char *end;
    long n = SDL_strtol(v, &end, 10);
    if (end == v || *end || n < lo || n > hi)
        return 0;
    *out = (int)n;
    return 1;
}

static int parse_size(const char *v, int *w, int *h)
{
    char *end;
    long a = SDL_strtol(v, &end, 10);
    if (end == v || (*end != 'x' && *end != 'X'))
        return 0;
    const char *p = end + 1;
    long b = SDL_strtol(p, &end, 10);
    if (end == p || *end || a < 16 || a > 1024 || b < 16 || b > 1024)
        return 0;
    *w = (int)a;
    *h = (int)b;
    return 1;
}

static const char *const voice_names[] = {"ringtone", "gm"};
static const char *const sms_names[] = {"server", "delivered", "offline"};

static int parse_name(const char *v, const char *const *names, int n, int *out)
{
    for (int i = 0; i < n; i++)
        if (!SDL_strcasecmp(v, names[i])) {
            *out = i;
            return 1;
        }
    return 0;
}

int settings_parse(Settings *s, const char *text)
{
    enum { NONE, VIDEO, SOUND, SMS, FILES, HANDSET, OTHER } sec = NONE;
    int errors = 0, lineno = 0, cleared_recent = 0;
    char *copy = SDL_strdup(text ? text : ""), *next = copy;
    while (next) {
        char *line = next;
        next = SDL_strchr(line, '\n');
        if (next)
            *next++ = 0;
        lineno++;
        line = trim(line);
        if (!*line || *line == ';' || *line == '#')
            continue;
        if (*line == '[') {
            char *close = SDL_strchr(line, ']');
            if (close)
                *close = 0;
            const char *name = trim(line + 1);
            sec = !SDL_strcasecmp(name, "video")     ? VIDEO
                  : !SDL_strcasecmp(name, "sound")   ? SOUND
                  : !SDL_strcasecmp(name, "sms")     ? SMS
                  : !SDL_strcasecmp(name, "files")   ? FILES
                  : !SDL_strcasecmp(name, "handset") ? HANDSET
                                                     : OTHER;
            continue;
        }
        char *eq = SDL_strchr(line, '=');
        if (!eq) {
            SDL_Log("settings.ini:%d: expected 'key = value', ignored: %s", lineno, line);
            errors++;
            continue;
        }
        *eq = 0;
        const char *k = trim(line), *v = trim(eq + 1);
        int ok = 1, w, h;
        switch (sec) {
        case VIDEO:
            if (!SDL_strcasecmp(k, "scale"))
                ok = parse_int(v, 1, 6, &s->scale);
            else if (!SDL_strcasecmp(k, "fullscreen"))
                ok = parse_int(v, 0, 1, &s->fullscreen);
            break;
        case SOUND:
            if (!SDL_strcasecmp(k, "voice"))
                ok = parse_name(v, voice_names, 2, &s->voice);
            else if (!SDL_strcasecmp(k, "volume"))
                ok = parse_int(v, 0, 100, &s->volume);
            else if (!SDL_strcasecmp(k, "mute"))
                ok = parse_int(v, 0, 1, &s->mute);
            break;
        case SMS:
            if (!SDL_strcasecmp(k, "mode"))
                ok = parse_name(v, sms_names, 3, &s->sms);
            break;
        case FILES:
            if (!SDL_strcasecmp(k, "last-dir")) {
                SDL_strlcpy(s->last_dir, v, sizeof s->last_dir);
            } else if (!SDL_strcasecmp(k, "recent")) {
                if (!cleared_recent++) /* the file's list replaces the current one */
                    s->nrecent = 0;
                if (*v && s->nrecent < SETTINGS_MAX_RECENT)
                    SDL_strlcpy(s->recent[s->nrecent++], v, sizeof s->recent[0]);
            }
            break;
        case HANDSET:
            if ((ok = parse_size(v, &w, &h)) && *k)
                settings_set_handset(s, k, w, h);
            break;
        default:
            break;
        }
        if (!ok) {
            SDL_Log("settings.ini:%d: invalid value for %s, ignored: %s", lineno, k, v);
            errors++;
        }
    }
    SDL_free(copy);
    return errors;
}

typedef struct {
    char *s;
    size_t len, cap;
    int failed;
} Buf;

static void put(Buf *b, const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(2);
static void put(Buf *b, const char *fmt, ...)
{
    if (b->failed)
        return;
    for (;;) {
        if (b->cap - b->len < 2) {
            size_t cap = b->cap ? b->cap * 2 : 1024;
            char *n = SDL_realloc(b->s, cap);
            if (!n) {
                b->failed = 1;
                return;
            }
            b->s = n;
            b->cap = cap;
        }
        va_list ap;
        va_start(ap, fmt);
        int n = SDL_vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->failed = 1;
            return;
        }
        if (b->len + (size_t)n < b->cap) {
            b->len += (size_t)n;
            return;
        }
        b->s[b->len] = 0;
        size_t cap = b->cap * 2;
        while (cap <= b->len + (size_t)n)
            cap *= 2;
        char *s2 = SDL_realloc(b->s, cap);
        if (!s2) {
            b->failed = 1;
            return;
        }
        b->s = s2;
        b->cap = cap;
    }
}

char *settings_format(const Settings *s)
{
    Buf b = {0};
    put(&b, "; exen-reloaded settings, written by the app when a setting changes in its menus.\n"
            "; Command-line options override them for one run. Delete this file to reset them.\n");
    put(&b, "\n[video]\nscale = %d\nfullscreen = %d\n", s->scale, s->fullscreen ? 1 : 0);
    put(&b, "\n[sound]\n; voice: ringtone or gm (General MIDI)\nvoice = %s\nvolume = %d\nmute = %d\n",
        voice_names[s->voice == EXEN_VOICE_PIANO], s->volume, s->mute ? 1 : 0);
    put(&b, "\n[sms]\n; mode: server, delivered or offline\nmode = %s\n",
        sms_names[s->sms >= 0 && s->sms <= 2 ? s->sms : 0]);
    put(&b, "\n[files]\nlast-dir = %s\n", s->last_dir);
    for (int i = 0; i < s->nrecent; i++)
        put(&b, "recent = %s\n", s->recent[i]);
    put(&b, "\n[handset]\n; per-game screen size, by the game's save file name\n");
    for (int i = 0; i < s->nhandset; i++)
        put(&b, "%s = %dx%d\n", s->handset[i].game, s->handset[i].w, s->handset[i].h);
    if (b.failed) {
        SDL_free(b.s);
        return NULL;
    }
    return b.s;
}

int settings_load(Settings *s, const char *path)
{
    settings_defaults(s);
    size_t len;
    char *text = SDL_LoadFile(path, &len);
    if (!text)
        return 0;
    settings_parse(s, text);
    SDL_free(text);
    return 1;
}

int settings_save(const Settings *s, const char *path)
{
    char *text = settings_format(s);
    if (!text)
        return -1;
    int r = exen_write_file(path, text, SDL_strlen(text));
    SDL_free(text);
    return r;
}

static int same_path(const char *a, const char *b)
{
#ifdef SDL_PLATFORM_WINDOWS
    for (; *a && *b; a++, b++) {
        int x = *a == '/' ? '\\' : SDL_tolower((unsigned char)*a);
        int y = *b == '/' ? '\\' : SDL_tolower((unsigned char)*b);
        if (x != y)
            return 0;
    }
    return *a == *b;
#else
    return !SDL_strcmp(a, b);
#endif
}

void settings_add_recent(Settings *s, const char *path)
{
    if (!path || !*path)
        return;
    char keep[SETTINGS_PATH_MAX];
    SDL_strlcpy(keep, path, sizeof keep); /* `path` may point into the list itself */
    int n = 0;
    char (*list)[SETTINGS_PATH_MAX] = s->recent;
    for (int i = 0; i < s->nrecent; i++)
        if (!same_path(list[i], keep) && n < SETTINGS_MAX_RECENT - 1) {
            if (n != i)
                SDL_memmove(list[n], list[i], sizeof list[0]);
            n++;
        }
    SDL_memmove(list[1], list[0], (size_t)n * sizeof list[0]);
    SDL_strlcpy(list[0], keep, sizeof list[0]);
    s->nrecent = n + 1;
}

void settings_remove_recent(Settings *s, int index)
{
    if (index < 0 || index >= s->nrecent)
        return;
    SDL_memmove(s->recent[index], s->recent[index + 1],
                (size_t)(s->nrecent - index - 1) * sizeof s->recent[0]);
    s->nrecent--;
}

int settings_get_handset(const Settings *s, const char *game, int *w, int *h)
{
    for (int i = 0; i < s->nhandset; i++)
        if (!SDL_strcmp(s->handset[i].game, game)) {
            *w = s->handset[i].w;
            *h = s->handset[i].h;
            return 1;
        }
    return 0;
}

void settings_set_handset(Settings *s, const char *game, int w, int h)
{
    int i = 0;
    while (i < s->nhandset && SDL_strcmp(s->handset[i].game, game))
        i++;
    if (w <= 0 || h <= 0) {
        if (i < s->nhandset) {
            s->handset[i] = s->handset[s->nhandset - 1];
            s->nhandset--;
        }
        return;
    }
    if (i == s->nhandset) {
        if (s->nhandset == SETTINGS_MAX_HANDSETS)
            return;
        s->nhandset++;
        SDL_strlcpy(s->handset[i].game, game, sizeof s->handset[i].game);
    }
    s->handset[i].w = w;
    s->handset[i].h = h;
}
