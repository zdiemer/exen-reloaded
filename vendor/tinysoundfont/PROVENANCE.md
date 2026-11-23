# Vendored: TinySoundFont

A single-header SoundFont 2 synthesizer in C, used by the core's General MIDI melody voice
(`src/core/gm.c`, see README.md, Sound). Vendored unmodified.

- Upstream: <https://github.com/schellingb/TinySoundFont> by Bernhard Schelling
- File: `tsf.h` (v0.9) from commit `a3621a19c6504885cd9583e6e26c35bc954fc8ec` (2026-11-16).
  The last commit that changed `tsf.h` is `790a219810cb0fca5defa8cdbd88e2487e5efc7a`
  (2025-06-05, "Work around GCC wrongfully warn about use after free"); the file is identical
  at both commits.
- Download: `https://raw.githubusercontent.com/schellingb/TinySoundFont/a3621a19c6504885cd9583e6e26c35bc954fc8ec/tsf.h`
  and `.../LICENSE` from the same commit.
- Licence: MIT (`LICENSE`, and the same text in the header of `tsf.h`). Compatible with the
  project's GPL-2.0.

| File | SHA-256 |
|---|---|
| `tsf.h` | `70d55963c98f60ebb81518eaa1f25d46888d5180eb5f5289fd6b74ffc177d197` |
| `LICENSE` | `ef10cb839de73fb6d5d32ea7151979e87809a6df70dc7e7ee8457ae1c9e7aa7e` |

`tsf.h` keeps upstream's CRLF line endings (`.gitattributes` marks it `-text`), so the hash
above matches the upstream file.

## How it is built

Only `src/core/gm.c` includes it, with `TSF_IMPLEMENTATION`, `TSF_STATIC` (no exported
symbols) and `TSF_NO_STDIO` (the core does no file I/O: the frontend hands it the `.sf2`
bytes). The warnings the header triggers under `-Wall -Wextra` (GCC: unused static
functions; Clang also `-Wnull-pointer-subtraction`) are silenced for the include only. `gm.c`
parses the SoundFont with `tsf_load` and a stream of its own rather than `tsf_load_memory`,
whose casts of its reader functions UBSan's `-fsanitize=function` reports. It needs
`malloc`/`realloc`/`free`, `memcpy`/`memset` and `<math.h>` (libm).

`gm.c` reads and writes TinySoundFont's voice structs directly to save and restore them in
save states (src/core/state.c, `GMSF` chunk), so updating `tsf.h` means checking that
`struct tsf_voice` and its members still match `gm_save` / `gm_load`.
