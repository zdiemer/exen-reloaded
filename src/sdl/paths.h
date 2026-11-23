/* Where the SDL frontend keeps its files (key bindings, settings, save states, ...). */
#ifndef EXEN_SDL_PATHS_H
#define EXEN_SDL_PATHS_H

#include <stddef.h>

/* The directory for exen-reloaded's own files, ending with a path separator: the directory of
 * the executable (SDL_GetBasePath) if it is writable, so a portable install keeps everything
 * beside the program, otherwise the per-user directory (SDL_GetPrefPath), created if needed.
 * Returns "" (the current directory) if neither is available. The result is cached. */
const char *exen_data_dir(void);

/* Write `len` bytes to `path` through a temporary file that is then renamed over it, so a
 * crash never leaves a half-written file. Creates the directory (and its parents) if needed.
 * Returns 0, or -1 after logging the error. */
int exen_write_file(const char *path, const void *data, size_t len);

/* The file name part of `path` (after the last / or \). */
const char *exen_path_basename(const char *path);

#endif
