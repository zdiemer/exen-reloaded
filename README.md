# exen-reloaded

**Play In-Fusio ExEn mobile phone games on a modern PC, at full speed.**

ExEn ("Execution Engine") was In-Fusio's games platform for European feature phones in
2002–2004, on handsets from Panasonic, Sagem, Philips, Alcatel and Trium. Games shipped as `.exn`
files: a compact Java-like bytecode program plus palettised images, tile maps and melodies.

The only way to run them today is In-Fusio's own SDK simulator, `gensimu.exe`. It is a 2003 Win32
program that busy-waits (it keeps a CPU core near 100% while sitting in a menu), runs many games
at a crawl, and crashes on others. exen-reloaded is a from-scratch, portable reimplementation of
the ExEn virtual machine and platform, built to replace it.

> **Status: early.** The repository holds the reverse-engineering groundwork. Nothing runs games yet.

## Goals

- Every known `.exn` runs correctly and at the speed the hardware intended.
- Portable C core with no Windows dependency: Windows, Linux and macOS builds.
- SDL2 frontend with remappable keyboard and gamepad controls.
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

## Legal

exen-reloaded is an independent reimplementation written for interoperability and game
preservation. It is not affiliated with In-Fusio. ExEn and the game titles are trademarks of
their respective owners.
