/* Where the SDL frontend keeps its files (key bindings, save states, ...). */
#ifndef EXEN_SDL_PATHS_H
#define EXEN_SDL_PATHS_H

/* The directory for exen-reloaded's own files, ending with a path separator: the directory of
 * the executable (SDL_GetBasePath) if it is writable, so a portable install keeps everything
 * beside the program, otherwise the per-user directory (SDL_GetPrefPath), created if needed.
 * Returns "" (the current directory) if neither is available. The result is cached. */
const char *exen_data_dir(void);

#endif
