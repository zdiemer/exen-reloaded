#include "paths.h"

#include <SDL3/SDL.h>

/* Can we create a file in `dir`? Probed by creating and removing a scratch file, which is the
 * only reliable test (access() ignores ACLs and read-only media on some platforms). */
static int dir_writable(const char *dir)
{
    char probe[1024];
    SDL_snprintf(probe, sizeof probe, "%s.exen-write-test", dir);
    SDL_IOStream *f = SDL_IOFromFile(probe, "wb");
    if (!f)
        return 0;
    SDL_CloseIO(f);
    SDL_RemovePath(probe);
    return 1;
}

const char *exen_data_dir(void)
{
    static char dir[1024];
    static int done;
    if (done)
        return dir;
    done = 1;
    const char *base = SDL_GetBasePath();
    if (base && dir_writable(base)) {
        SDL_strlcpy(dir, base, sizeof dir);
        return dir;
    }
    char *pref = SDL_GetPrefPath("exen-reloaded", "exen-reloaded"); /* creates it */
    if (pref) {
        SDL_strlcpy(dir, pref, sizeof dir);
        SDL_free(pref);
    } else {
        dir[0] = 0;
    }
    return dir;
}

int exen_write_file(const char *path, const void *data, size_t len)
{
    char dir[1024], tmp[1100];
    SDL_strlcpy(dir, path, sizeof dir);
    char *cut = (char *)exen_path_basename(dir);
    if (cut > dir) {
        cut--;
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

const char *exen_path_basename(const char *path)
{
    const char *slash = SDL_strrchr(path, '/'), *bslash = SDL_strrchr(path, '\\');
    if (bslash > slash)
        slash = bslash;
    return slash ? slash + 1 : path;
}
