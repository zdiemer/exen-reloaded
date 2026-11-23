# Vendored: TimGM6mb General MIDI SoundFont

A small, complete General MIDI SoundFont (5.7 MiB): 136 presets, the 128 melodic programs of
bank 0 and the 8 GS drum kits of bank 128 (Standard, Room, Power, Electronic, TR 808, Jazz,
Brush, Orchestra). The General MIDI melody voice plays the melodies with it, the way the original
simulator sent them to Windows' General MIDI synth (README.md, Sound;
docs/re/NATIVES.md §3.2). Vendored unmodified.

- File: `TimGM6mb.sf2`, 5 969 788 bytes. RIFF `sfbk`, INFO name `TimGM6mb1.sf2`, made with
  Awave Studio v8.5 for EMU8000.
- SHA-256: `c5378b62028c920cb11e4803327983fee2f2cdff5dc89c708e39da417e51c854`
- Authors: Tim Brechbill (2004, <https://www.timbrechbill.com>), with fixes by David Bolton
  (2010) for MuseScore.
- Source: MuseScore 1.x, which shipped it as its default SoundFont:
  <https://github.com/musescore/musescore-old/raw/90c33ef9d87b3f5ff92efd3b07d89eb455fb1fef/mscore/share/sound/TimGM6mb.sf2>
  (commit `90c33ef9`, 2010-08-22, "Fix SoundFont (missing E in Alto Sax)"). Debian's upstream
  source for the `timgm6mb-soundfont` package is the same commit.
- Cross-check: the file is byte-identical to Debian's `timgm6mb-soundfont` 1.3-5 (bullseye to
  sid): <https://sources.debian.org/src/timgm6mb-soundfont/1.3-5/TimGM6mb.sf2/> reports the
  same SHA-256.

## Licence: GPL-2

The SoundFont is under the GNU General Public License version 2, the same licence as
exen-reloaded; the full text is in `COPYING` beside it. Evidence:

- Debian's machine-readable copyright file for the package
  (<https://metadata.ftp-master.debian.org/changelogs/main/t/timgm6mb-soundfont/stable_copyright>):

  ```
  Files: TimGM6mb.sf2
  Copyright: 2004 Tim Brechbill <https://www.timbrechbill.com>
    2010 David Bolton <davidkbolton@gmail.com>
  License: GPL-2
  ```

  with the comment, quoting a MuseScore forum post by "Thomas" of 2009-12-06
  (<https://musescore.org/en/node/1463#comment-9168>): Tim came back with more information
  about the SoundFont: "I created some of the samples myself and all others are either public
  domain or fall under the terms of the GNU General Public License."
- MuseScore (GPL-2) distributed it in its source tree and installers from 2009 to the 1.x
  series.

So it can be redistributed in this repository and in the release archives, provided its
licence text goes with it (`COPYING`, packaged beside the `.sf2`).

## Where the frontends look for it

- SDL frontend: `--soundfont PATH`, else `soundfont/TimGM6mb.sf2` or `TimGM6mb.sf2` beside
  the program, else `vendor/soundfont/TimGM6mb.sf2` in a source tree.
- libretro core: `TimGM6mb.sf2` in the frontend's system directory or its `exen/` subfolder
  (docs/LIBRETRO.md).
- `exen-run`: `--soundfont PATH`, else the same places as the SDL frontend.

Any other General MIDI SoundFont 2 file works too (`--soundfont`); TimGM6mb is the one
shipped because its licence allows it and it is small.
