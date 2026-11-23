# libretro core

`exen_reloaded_libretro` runs ExEn games in RetroArch and other libretro frontends. It embeds the
same portable core as the SDL frontend (`src/core/`) and leaves video, audio, input, save files
and save states to the frontend. The source is `src/libretro/libretro.c`.

## Build

The core is off by default. Turn it on with `EXEN_BUILD_LIBRETRO`:

```sh
cmake -S . -B build -G Ninja -DEXEN_BUILD_LIBRETRO=ON
cmake --build build
```

This builds `exen_reloaded_libretro.dll` (Windows), `.so` (Linux) or `.dylib` (macOS) and copies
`exen_reloaded_libretro.info` next to it. The library exports only the `retro_*` entry points
(a version script on Linux, an export list on macOS, `dllexport` on Windows); the core is
compiled position-independent for it. A MinGW build links libgcc statically, so the DLL needs
only `KERNEL32.dll` and `msvcrt.dll`. `-DEXEN_BUILD_SDL=OFF` skips the SDL frontend if only the
core is wanted.

`src/libretro/libretro.h` is the canonical header from
[libretro-common](https://github.com/libretro/libretro-common/blob/master/include/libretro.h),
vendored unmodified (upstream commit `ae22debd`, 2026-11-21; MIT licence in its header).

## Install

1. Copy the core into the frontend's cores directory and `exen_reloaded_libretro.info` into its
   core info directory (in RetroArch: Settings > Directory > Cores / Core Info).
2. Copy the system files from this repository's `vendor/gensimu/` into the frontend's system
   directory (RetroArch: Settings > Directory > System/BIOS):

   | File | Required | What for |
   |---|---|---|
   | `gensimu.exe` | yes | the original In-Fusio simulator; the ExEn framework classes, the math table and the font are loaded from it |
   | `flash/eeprom.dat` | no | the simulator's EEPROM defaults (nickname, currency, SMS prices) that a new save starts from |

   Both may also go in an `exen/` subfolder of the system directory
   (`<system>/exen/gensimu.exe`, `<system>/exen/flash/eeprom.dat`). `eeprom.dat` is looked for
   beside whichever `gensimu.exe` was found, as `flash/eeprom.dat` or `eeprom.dat`.

   Without `gensimu.exe` the game does not load, and the frontend shows a message naming the
   paths that were tried.
3. Load a `.exn` file with the core. Games are read from memory (`need_fullpath` is false), so
   they also load from inside archives.

## Controls

The core maps the RetroPad onto the phone keypad like the SDL frontend's defaults. Face
buttons go by position: the bottom button (RetroPad B) is 5.

| Phone key | RetroPad | Keyboard (frontend keyboard events) |
|---|---|---|
| 2 / 4 / 6 / 8 (up / left / right / down) | d-pad, left stick | `2 4 6 8`, numpad 8 / 4 / 6 / 2 |
| 5 (fire / select) | B (bottom) | `5`, numpad 5 |
| 1 / 3 | L / R | `1 3`, numpad 7 / 9 |
| 7 / 9 | L2 / R2 | `7 9`, numpad 1 / 3 |
| 0 | L3 / R3 | `0`, numpad 0 |
| * / # | Y (left) / X (top) | `[` / `]`, numpad * / numpad / |
| OK / select (soft key, code -8) | Start | Enter, numpad Enter |
| Clear / back (soft key, code -7) | A (right), Select | Backspace |

The numpad follows the phone layout, so numpad 7 is phone 1. The stick presses a direction past
50% of its travel and releases it below 35%. A phone key reached through several buttons (say
d-pad up and the stick) is pressed when the first goes down and released when the last comes
up, so the game never sees a duplicate press or release. Only the first port is read. The
keyboard keys come through `RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK`; letters and arrows are
left to the frontend, which by default maps them to the RetroPad. Use the frontend's remapping
for anything else.

The core reports these bindings as input descriptors, so RetroArch's Controls menu names each
button after its phone key.

## Core options

| Option | Values | Effect |
|---|---|---|
| Screen size (`exen_screen_size`) | Auto, 101x80, 112x112, 120x143, 128x128, 128x141, 128x160, 132x176 | The handset screen the game runs on. Auto takes it from the built-in game database, or 132x176 (the simulator's default handset) for games it does not know. Takes effect when the game is restarted (Restart / `retro_reset`, or the next load); the core then updates the frontend's geometry. |
| Sound (`exen_audio`) | enabled, disabled | Disabled mutes the output. The melodies keep running silently, so they are in time when sound comes back. Takes effect at once. |
| Melody voice (`exen_melody_voice`) | Ring tone (`ringtone`), Piano (like the 2003 simulator) (`piano`) | Ring tone is a band-limited square wave, close to a 2003 handset. Piano approximates what the original simulator played: it sent the notes to Windows' General MIDI synth, whose default instrument is the acoustic grand piano (see README.md, Sound). Takes effect at once, with a short crossfade; the melodies keep their timing. Save states do not store the voice. |
| SMS (`exen_sms`) | Mock server, Delivered no reply, Offline (`server`, `delivered`, `offline`) | What happens to the SMS games send (rankings, "Your opinion", continues, unlocks; see [SMS.md](re/SMS.md)). Mock server (default): a stand-in for In-Fusio's SMS server reports them as sent and answers the requests that expect an answer, with only what you sent (your own scores and votes). Delivered: sent, never answered. Offline: not sent, as with no network. Takes effect at once. The server's records and an answer in flight are part of save states. Rankings carry the server's default date (2004-01-01): the core never reads the host clock, so runs stay deterministic. |

The options use the v2 interface, falling back to v1 or plain variables on older frontends.

## Timing

The core runs at a fixed 60 frames per second on a virtual clock and never reads the host
clock, sleeps or starts threads. Each `retro_run`:

1. delivers the frame's key changes (releases, then presses) at the frame's start;
2. runs every game tick whose deadline falls in the frame. Games set their tick period through
   the timer native (75 to 200 ms in the known corpus), so a game ticks every 4.5 to 12 frames;
   the clock is set to each tick's deadline while it runs, which is what
   `getTimerTickCount` reads. At most 8 ticks run per frame; a game further behind drops the
   missed ticks rather than bursting, as in the original and the SDL frontend;
3. renders the frame's audio, up to each tick's deadline before the tick runs (so a melody the
   tick starts begins on the right sample) and to the end of the frame after the last one:
   exactly 735 stereo frames at 44100 Hz, in one `audio_sample_batch` call;
4. sends the frame, or a dupe (`NULL`) when the game drew nothing new and the frontend allows
   dupes.

The same inputs therefore always give the same frames and audio, which rewind, run-ahead and
netplay rely on. Video is XRGB8888 (RGB565 if the frontend refuses it), converted from the
core's RGB332 with the original simulator's colour table (`src/core/rgb332.h`, shared with the
SDL frontend). The geometry is the game's handset screen, with room up to 176x220 for screen
size changes.

## Save RAM

The frontend's save file (`.srm` in RetroArch) is the phone's 2048-byte EEPROM, where games keep
high scores and progress (`Gamelet.saveCtx`), in the same layout as the SDL frontend's
`saves/<game>.eep` and the simulator's `flash/eeprom.dat`; the files are interchangeable. A
game without a save file starts from `flash/eeprom.dat`'s settings with an empty save record.

Frontends load the save file after the game is loaded, so the core boots the game on the first
`retro_run` (or the first `retro_serialize`), once the save RAM is in place. The save RAM buffer
belongs to the core and stays at one address for the whole session, including across Restart;
the core copies it into the VM at boot and back whenever the game saves.

## Save states

Save states, rewind and run-ahead work through the core's own state format
(`exen_state_save`, `src/core/state.c`), followed by a small trailer with the libretro side's
clock, next tick deadline, timer period and the keys held. A state belongs to one game file and
one `gensimu.exe`; loading it into anything else is refused. It also restores the EEPROM as it
was when the state was taken.

libretro needs one fixed state size for the whole session. The core reports the worst case for
the loaded game (`exen_state_max_size`): about 7 MB, almost all of it the bound for a full 4 MiB
heap. Real states are 40-250 KB; the rest of the buffer is zeros, which compress to nearly
nothing in RetroArch's state files and rewind buffer. The core declares the
`ENDIAN_DEPENDENT` serialization quirk, because the VM's heap is stored in host byte order.

## Testing

`exen-libretro-test` (built with the core, source in `src/libretro/test_frontend.c`) is a
headless frontend that loads the core as a shared library, as RetroArch would, and checks each
game it is given: a video frame (or dupe) and 735 audio frames per `retro_run`, a frame that is
not a single colour, the save RAM (2048 bytes, defaults from `flash/eeprom.dat`, writes after the
load reaching the game, a stable pointer across reset), a constant `retro_serialize_size`, a
state round trip that replays the same frames and audio and re-serializes byte-identical, a
second load that reproduces the first run, and reset. It also checks that a load without
`gensimu.exe` fails with a message and that a file that is not a game is refused. Input is
scripted (menu keys, directions, the stick, and keyboard events overlapping RetroPad buttons).

```sh
build/exen-libretro-test --frames 600 "$EXEN_GAMES_DIR"/*.exn
build/exen-libretro-test --dump frames/ --dump-every 60 --expect-audio game.exn
build/exen-libretro-test --option exen_screen_size=128x128 --rgb565 game.exn
```

By default it loads the core it was built with and uses `vendor/gensimu/` as the system
directory (`--core`, `--system` override them). CTest runs it on the vendored
`vendor/gensimu/flash/download1.exn` (test `libretro_core`), which needs no game files.

## Known limits

- States are reported as about 7 MB (see above). Rewind needs a rewind buffer of at least twice
  that; RetroArch's default of 20 MB is enough.
- A screen size change applies on Restart, not immediately.
- Only the first controller port is read; keyboard bindings are fixed (remap the RetroPad in
  the frontend instead).
- As in the SDL frontend, an SMS send reports failure (there is no network) and the nickname
  edit box does not open (src/core/sms.c).
- Tested on 64-bit Windows (MinGW) against RetroArch 1.22. The Linux and macOS link options
  follow the usual libretro conventions but have not been run yet.
