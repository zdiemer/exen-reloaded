# exen-reloaded

**Play In-Fusio ExEn mobile phone games on a modern PC, at full speed.**

ExEn ("Execution Engine") was In-Fusio's games platform for European feature phones in
2002–2004, on handsets from Panasonic, Sagem, Philips, Alcatel and Trium. Games shipped as `.exn`
files: a compact Java-like bytecode program plus palettised images, tile maps and melodies.

The only way to run them today is In-Fusio's own SDK simulator, `gensimu.exe`. It is a 2003 Win32
program that busy-waits (it keeps a CPU core near 100% while sitting in a menu), runs many games
at a crawl, and crashes on others. exen-reloaded is a from-scratch, portable reimplementation of
the ExEn virtual machine and platform, built to replace it.

> **Status: early.** The VM boots real games headlessly; graphics and the SDL3 frontend are in
> progress. Progress is tracked in [issues](../../issues) and [milestones](../../milestones).

## Goals

- Every known `.exn` runs correctly and at the speed the hardware intended.
- Portable C core with no Windows dependency: Windows, Linux and macOS builds.
- SDL3 frontend with remappable keyboard and gamepad controls.
- Save states.
- A libretro core.
- A headless harness that sweeps a directory of games and publishes a compatibility list.

## How it works

ExEn games are bytecode for a small Java-like VM. The ExEn framework (`exen.Gamelet`,
`exen.Graphics`, `exen.Sprite`, `exen.PlayField`, …) is bytecode for that same VM, sitting on a
thin layer of native functions for drawing, timers, keys, sound and persistent storage.
exen-reloaded implements the VM and that native layer. It runs the framework bytecode taken from
the original simulator, so the framework behaves exactly as In-Fusio wrote it.

The original simulator is vendored in [`vendor/gensimu/`](vendor/gensimu/PROVENANCE.md) for
preservation and as the reference behaviour. It is abandoned freeware.

## Repository layout

```
docs/          format notes and architecture (start with docs/FORMAT.md)
src/           the emulator (VM, native layer, frontends)
tools/         Python helpers; tools/exn.py inspects .exn files
vendor/gensimu the original In-Fusio simulator, unmodified
```

## Games

No games are included; supply your own `.exn` files. To point the tooling at a local collection,
copy `local.env.example` to `local.env` (gitignored) and set `EXEN_GAMES_DIR`.

```sh
python tools/exn.py info "$EXEN_GAMES_DIR"/*.exn
```

## Controls

The SDL3 frontend (`exen-reloaded game.exn`) maps the keyboard and any number of gamepads onto
the phone's keypad. Games were written for a phone: most move with 2/4/6/8 and fire or select
with 5 or OK, and some use 1/3/7/9 for diagonals. The defaults keep the original simulator's
keys (arrows = 2/4/6/8, numpad = keypad, Enter = OK, Esc = clear) and add the following:

| Phone key | Keyboard | Gamepad |
|---|---|---|
| 2 / 4 / 6 / 8 (up / left / right / down) | arrows, W A D S, `2 4 6 8`, numpad 8 4 6 2 | d-pad, left stick |
| 5 (fire / select) | Space, `5`, numpad 5 | A (bottom face button) |
| 1 / 3 | Q / E, `1 3`, numpad 7 / 9 | LB / RB |
| 7 / 9 | Z / C, `7 9`, numpad 1 / 3 | LT / RT |
| 0 | X, `0`, numpad 0 | stick clicks |
| * / # | `[` / `]`, numpad * / numpad / | X / Y (left / top face buttons) |
| OK / select (soft key, code -8) | Enter, numpad Enter | Start |
| Clear / back (soft key, code -7) | Esc, Backspace | B (right face button), Back |

The numpad follows the phone layout, so numpad 7 is phone 1. Hotkeys: M mutes, F11 toggles
fullscreen, F5 / F8 save / load a state, F6 / F7 pick the state slot (0-9). Keys held on several devices at once count as one press. Auto-repeat is ignored, as
in the original. Gamepads can be plugged in and out while a game runs.

To remap, edit `keys.ini`. It is written with the defaults on first run, beside the program, or
in the per-user data directory if the program's directory is not writable (for example
`%APPDATA%\exen-reloaded\exen-reloaded\` on Windows or `~/.local/share/exen-reloaded/exen-reloaded/`
on Linux). `--keys-config PATH` uses another file, and `--print-bindings` prints the controls
in effect. Each line binds a phone key (`0`-`9`, `star`, `pound`, `ok`, `clear`, or the joypad codes
`pad-up`, `pad-down`, `pad-left`, `pad-right`, which the original never sends) to SDL key names
or gamepad inputs:

```ini
; comments start with ; or #
[keyboard]
5 = Space, Left Ctrl, Keypad 5
ok = Return, Right Shift
up = Up, W

[gamepad]
stick-press = 50
stick-release = 35
5 = south
ok = start, west
2 = dpup, lefty-
```

`up`, `down`, `left`, `right` and `fire` are aliases for 2, 8, 4, 6 and 5. Gamepad face buttons
are named by position (`south`, `east`, `west`, `north`; SDL's `a`, `b`, `x`, `y` also work),
stick and trigger directions are `leftx-`, `lefty+`, `righttrigger` and so on, and
`stick-press` / `stick-release` are the percentages of travel that press and release a key.
A section in the file replaces the defaults for that device; a missing section keeps them.
Invalid entries are logged and skipped. The file's header comment lists every name.

## Saves and save states

Games keep their high scores and progress in the phone's EEPROM (`Gamelet.saveCtx`). The SDL
frontend keeps one 2048-byte image per game in `saves/<game>.eep` and writes it every time the
game saves, as the original writes `flash/EEPROM.DAT`. A game seen for the first time starts from
the settings in the simulator's `flash/eeprom.dat` (nickname, currency, SMS prices) with an empty
save record.

Save states capture the whole machine. F5 saves to the current slot, F8 loads it, F6 / F7 select
the slot; the window title shows what happened. Slots are files
`states/<game>/slot0.exst` .. `slot9.exst`. A state belongs to one game file and one
`gensimu.exe`; loading one into anything else is refused. A state includes the EEPROM as it was
when the state was saved, so loading an older state also brings back its saves.

Both directories live beside the program, or in the per-user data directory (as `keys.ini`
above). `<game>` is the title from the game database, else the CRC-32 of the `.exn` file.

The headless runner can test both: `exen-run --eeprom-out FILE` writes the EEPROM back,
`--save-at N FILE` and `--load FILE` save and resume states, and `--selftest-state` checks that
a state round trip reproduces the same frames, audio and final state.

## libretro core

exen-reloaded also builds as a libretro core for RetroArch and other frontends
(`-DEXEN_BUILD_LIBRETRO=ON`, giving `exen_reloaded_libretro`). Copy `vendor/gensimu/gensimu.exe`
(and optionally `vendor/gensimu/flash/eeprom.dat`, as `flash/eeprom.dat`) into the frontend's
system directory and load a `.exn`. It supports save files (the EEPROM), save states, rewind,
run-ahead, core options for the screen size and sound, and the same pad layout as above. See
[docs/LIBRETRO.md](docs/LIBRETRO.md).

## Legal

exen-reloaded is an independent reimplementation written for interoperability and game
preservation. It is not affiliated with In-Fusio. ExEn and the game titles are trademarks of
their respective owners.
