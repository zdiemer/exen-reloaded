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

Fully specified in [`docs/re/CVP.md`](re/CVP.md); parsed by `tools/cvp.py`. In brief:

- The code chunk is a run of blocks, **one per class** (2–24 classes per game). Each block starts
  `"4CVP"`, `u16 size`, and is padded to 4 bytes.
- Classes are numbered globally: the 54 framework classes in `gensimu.exe` are 0–53 in their
  `.rdata` order, and game classes follow from 54. Class references in game code are these indices,
  so the framework order is part of the ABI.
- Game code has no symbol names. Members are resolved at run time by a 32-bit hash:
  `crc16(descriptor) << 16 | crc16(name)` (CRC-16/MCRF4XX). Class hashes are CRC-32 of the dotted
  name. The framework blocks keep names in debug lists, which is how game overrides such as
  `onInit` and `paint` can be identified.
- There is no constant pool: constants, strings and refs sit inline after each method's code.

The instruction set is in [`docs/re/OPCODES.md`](re/OPCODES.md) and disassembled by `tools/disasm.py`.
