# Vendored: microui

[microui](https://github.com/rxi/microui) by rxi, a tiny immediate-mode UI library in ANSI C.
The SDL frontend uses it for the in-window menu strip and panels (`src/sdl/ui.c`), drawn with
the SDL renderer and an embedded 8x8 bitmap font.

- Version: 2.02 (`MU_VERSION`), from the `master` branch at commit
  `0850aba860959c3e75fb3e97120ca92957f9d057`
- Licence: MIT, see [`LICENSE`](LICENSE) (also at the top of `microui.c`)
- Files: `src/microui.c`, `src/microui.h` and `LICENSE` from that commit, unmodified

| File | SHA-256 |
|---|---|
| `microui.c` | `880ce7e017fe307553586c9ba8291e9d9f56cdb20bc4188df072f1e8bd9af6dd` |
| `microui.h` | `9aa08e7f58c2152dbfcb4d7deb1f5d79ab1bf0c2809d87f3ff89dc34e24d0c63` |
| `LICENSE` | `470601a025efe1d098dcce9b86b4e5b3fbdb9341c1f9ec285cf9435f6caa0a38` |

To update, replace the three files with those of a newer commit and update this note.
