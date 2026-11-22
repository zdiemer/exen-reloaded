# ExEn file formats

Working notes on the formats exen-reloaded has to read. Everything here was learned from the
games themselves and from the vendored simulator (`vendor/gensimu/gensimu.exe`); mark guesses as
guesses. `tools/exn.py` is the executable version of this document. When the two disagree, fix
whichever is wrong.

## `.exn` game container (`NEXE`)

All integers little-endian.

| Offset | Type | Meaning |
|---|---|---|
| `0x00` | `char[4]` | magic `NEXE` |
| `0x04` | `u32` | `0x80` in every known game (purpose unknown) |
| `0x08` | `u16[6]` | `512, 8192, 1024, 12288, 2048, 24576` in every known game. Guess: three (a, b) memory pairs, one per display depth (1/4/8 bit). |
| `0x14` | `char[32]` | main class name, NUL padded, e.g. `Worms.GameTopLevel`, `tombraider.TombRaiderMIDlet` |
| `0x34` | `u32` | `N`, the resource count |
| `0x38` | `u32[N+1]` | absolute offsets. Resource `i` spans `offsets[i]..offsets[i+1]`. `offsets[N]` is the code chunk. |
| … | `u8[N]` | resource type tags |
| … | | resource data, then the `4CVP` code chunk to end of file |

### Resource types (partial)

| Tag | Seen as | Notes |
|---|---|---|
| `0x0d` | image | `u16 w, u16 h, u16 ?` then a standard PNG stream (palettised) |
| `0x06` | ? | most common after images; guess: animation / tile data |
| `0x0e` | ? | |
| `0x03` | ? | |
| `0x0f` | ? | usually exactly one per game |
| `0x04` | melody? | BombJack, Tomb Raider: arrays of `(note, duration)`-looking u8 pairs |
| `0xff` | empty | zero-length placeholder |
| `0xd0`–`0xda`, `0x81` | ? | only in the later In-Fusio titles (IFSki*, Pikubi*, MonkeyBusiness, IFSudoku) |

The authoritative decoder is `exen.Resource` in the framework (`getResourceType`, `readImage`,
`readAnimation`, `readPlayField`, `readSound`, `getString`); fill this table in from it.

Text resources use `0xff` as a line separator (e.g. `CONTROLS\xff\xffLeft:\xff…`).

## `4CVP` code chunks

The code is a prelinked class image. All symbolic names are stripped from game code; calls into
the framework go by index. The framework itself is 58 `4CVP` class blocks inside `gensimu.exe`
(`.rdata`), and those *do* carry names: `exen.Gamelet`, `exen.Graphics`, `exen.Image`,
`exen.Palette`, `exen.Resource`, `exen.Animation`, `exen.AnimBitmap`, `exen.PlayField`,
`exen.Sprite`, `exen.AnimFlash`, `exen.Command`, `exen.Displayable`, `java.lang.*`,
`vm.sys.{Runtime,Application,Bootstrap}`.

Header (first bytes of every chunk seen so far): `"4CVP"`, then a `u32` that looks like a
checksum or id, then size fields. Name references inside the tables look like 32-bit hashes
(runs of `xxxx916e`). To be decoded.
