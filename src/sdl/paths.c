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
