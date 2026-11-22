# In-Fusio packed-data codecs and the "ExEn PNG" image container

The five decompressors in `gensimu.exe` that unpack image data (PNG `IDAT`) and packed resources
(type `0x81`, `Resource.readBytes`), and the image container around them. The portable C port is
`src/core/codec.{h,c}`. `tools/imgdump` uses it to turn every image in a game into a real PNG.

**Status:** all five codecs are decompiled and ported. The port is byte-identical to the
simulator's own code on every image and packed resource in the corpus (47 games plus the 2 vendored
flash files) and on about 12,000 synthetic streams. Codec 4 has no user in the corpus and is
validated only on synthetic streams.

Confidence: **high** for everything below unless marked otherwise. Addresses are VAs (image base
`0x400000`).

## Validation method

The reference is the simulator's own machine code, run under the Unicorn emulator (Python
`unicorn` 2.1.4). The harness maps `gensimu.exe`'s sections at their VAs and stubs `malloc`
`0x403fd0` and `free` `0x4040a4` with a bump allocator over zeroed memory. It calls
`codecCreate` `0x432e50(data, len)`, then the returned decoder's `vtable[0](codec, out, outSize)`,
exactly as `decodePng` and `Resource.readBytes` do. Every output was then compared byte for byte
with `exen_unpack`.

| Test set | Streams | Result |
|---|---|---|
| Corpus image IDATs (47 games + `vendor/gensimu/flash/*.exn`) | 2084 (c1 159, c2 47, c3 542, c5 1336) | all identical |
| Corpus type-0x81 resources (stream at offset 4) | 9 (c1 1, c3 8) | all identical |
| Random valid-header streams, all 5 codecs, random parameters (including odd row counts with K = 2, symbol widths > 25 bits, `n == 0`, short and long `outSize`) | 11,331 (c1 3149, c2 1078, c3 3008, c4 2940, c5 1156) | all identical |
| Random garbage through `exen_unpack` and `exen_image_decode` (C only) | 300,000 | no crash, no hang, never writes past `dst_cap` |

Fuzz cases where the original never terminates were left out of the comparison: codec 2 with a
zero unit, and codec 4 with endless zero-length runs. The port returns
`EXEN_CODEC_ERR_CORRUPT` for these instead of hanging.

`imgdump` over the corpus (`imgdump -q <out> $EXEN_GAMES_DIR/*.exn vendor/gensimu/flash/*.exn`):

```
49 files
images (type 0x0d):
  codec 1:    159 ok, 0 failed
  codec 2:     47 ok, 0 failed
  codec 3:    542 ok, 0 failed
  codec 4:      0 ok, 0 failed
  codec 5:   1336 ok, 0 failed
  raw 1bpp:     4 ok, 0 failed
  not decodable: 5          (empty placeholders, see below)
packed 1bpp bitmaps (type 0x81):
  codec 1:      1 ok, 0 failed
  codec 3:      8 ok, 0 failed
```

For every image, IHDR w×h equals the resource header's u16 w×h, and the header's unpacked size
equals `pitch * h` (pitch = `(w+1)/2` at 4 bpp, `w` at 8 bpp). For codec 5 that size comes from
bytes 9..11; for codecs 1–3 it is `((A+7)&~7)*B/8`. Every 0x81 stream unpacks to exactly
`((w+7)>>3)*h` bytes. Contact sheets of samples from 25+ games, covering every codec, bit depth 4
and 8, and the 0x81 bitmaps, show clean sprites, backgrounds, fonts and logos.

## Common stream header

All five codecs start with the same 68 bits, read MSB first:

| Bits | Field |
|---|---|
| 0..3 | codec number (`data[0] >> 4`): 1..5. Anything else makes `codecCreate` return NULL. |
| 4..35 | **A**: row width in bits |
| 36..67 | **B**: row count |

The stream unpacks to `((A + 7) & ~7) * B` bits (`0x432e03` → `0x432d20`, 32-bit arithmetic). In
images B is 1 and A = `pitch * 8 * h`, except for 28 codec-3 images with B = 2 and A = half of
that. Codec-specific parameters follow from bit 68.

`codecCreate` `0x432e50` mallocs a 0x14-byte object `{decode, destroy, len, data, state}` and
dispatches on the nibble through the jump table at `0x432f17`. The create routine fills in
`decode`/`destroy` and replaces `len` with the unpacked byte count.

| Codec | Create | Decode | Destroy | State size |
|---|---|---|---|---|
| 1 | `0x431750` | `0x4318f6` | `0x4318a0` | 0x41c (+ N×2 cache) |
| 2 | `0x42f560` (and `0x42f646`, see codec 3) | `0x42f724` | `0x42f6ed` | 0x418 |
| 3 | `0x42d500` | `0x42d960` | `0x42d90a` | 0x444 (+ 0x14 dictionary) |
| 4 | `0x432180` | `0x43228b` | `0x43225e` | 0x418 |
| 5 | `0x432a00` | `0x432a76` | `0x432a6c` (no-op) | none |

### Bit I/O

- `getBits(buf, &pos, n)` `0x412ea0` reads n bits MSB first through a 1-, 2- or 4-byte big-endian
  window. Every codec inlines the same code. When `(pos & 7) + n > 32` the result is wrong: the
  bits from the second word are masked away and the first part is shifted left by 2. The port
  keeps this. No real stream reaches it, since fields are at most 16 bits wide.
- `putBits(buf, &pos, value, n)` `0x413071` does a read-modify-write of the same window, and the
  codecs inline it too. **The value is not masked to n bits**, so its extra high bits spill into
  the field's left neighbours within the window. Codec 4 depends on this.
- Shift counts follow x86 rules (mod 32), so `1 << 32` is 1. The port's `shl`/`shr` helpers copy
  that.

### Resumable, in-place decoders

Codecs 1–4 can resume: when `outSize` ends inside a unit, they write the unit's top bits and save
the rest (`state+4` count, `state+6/8` value) for another decode call. Every call site decodes
exactly once, so the port writes the top bits and stops.

All decoders are written for **in-place** use: the caller stages the packed data at the tail of
the output buffer. Before each output write they check
`out + (outpos + unit)/8 >= in + rowstart/8`. When fewer than 0x400 input bytes remain past the
row start, they copy those bytes into a stash inside the state (`state+0x40` codec 3, `+0x13`
codec 2, `+0x18` codec 1, `+0x11` codec 4; codec 5 uses a 0x400-byte stack buffer when
`out == in`) and read from the stash from then on. Decoding into a separate buffer, as the port
does, never changes the result. Staging in place sometimes does; see
[Simulator bug: in-place staging corrupts some codec-3 images](#simulator-bug-in-place-staging-corrupts-some-codec-3-images).

## Codec 1: move-to-front cache (`0x431750` / `0x4318f6`)

Parameters after the header: **N** (5 bits) is the cache size, and **W** (5 bits) is the symbol
width. The cache is a malloc'd `u16[N]` (`state+0x14`), zeroed. While at least W bits of the
stream remain:

```
if getBits(1) == 1:  idx = getBits(5); sym = cache[idx]           # hit
else:                sym = getBits(W) (read as 8-bit chunks + rest); idx = N - 1   # literal
for i = idx down to 1: cache[i] = cache[i-1]                       # u8 loop counter
cache[0] = (u16)sym
remain -= W; putBits(sym, W)                                       # sym unmasked (u32)
```

Leftover bits (fewer than W) are copied 8 at a time, then as one last `getBits(remain)` written
with `putBits(v, outBits)`. Hits are move-to-front; a literal pushes out the oldest entry.
`idx >= N` reads and writes past the original's table; the port's cache has 256 entries and so
behaves as the original does with zeroed memory after the table. Corpus: N = 31 always, and
W = 8 (57 images) or 12 (102 images, three 4-bit pixels).

## Codec 2: repeat-previous RLE (`0x42f560` / `0x42f724`)

Parameter: **U** (4 bits) is the unit width. The data starts at bit 72. `prev` starts at 0. While
at least U bits remain: `if getBits(1) == 1: prev = getBits(U)`, then emit `prev` in U bits.
Leftover bits are copied raw (8 at a time, then `putBits(getBits(r), r)`). If
`outSize*8 > total bits` the decoder **returns without writing anything** (the port returns
`EXEN_CODEC_ERR_SIZE`). Corpus: U = 4 (31), 8 (15), 12 (1).

`0x42f646(codec, bitpos, U, totalBits)` creates the same decoder at an arbitrary bit position.
Codec 3 uses it.

## Codec 3: dictionary of 10 symbols, multi-pass rows (`0x42d500` / `0x42d960`)

This codec has the most users after codec 5 (43 games). Parameters: **U** (4 bits) is the unit
width and **K** (4 bits) is the number of units per symbol, so a symbol is S = U·K bits. Then
comes the dictionary: 10 entries of S bits, stored as `u16`. Each entry is read as 8-bit chunks
plus a remainder, with 16-bit shifts. The row stream starts after the dictionary.

```
R = ((A+7)>>3)<<3                       # row bits
remain = R*B;  if (B odd && K == 2) remain -= R     # last row goes to codec 2
pass = 1; rowstart = pos; rowleft = R
while remain > 0:
    if rowleft >= U:
        code = getBits(2)
        sym = code<=1 ? dict[code] : code==2 ? dict[2 + getBits(3)] : getBits(S)
        val = (sym >> (U*(K-pass))) & (2^U - 1);  remain -= U; rowleft -= U; u = U
    else:                               # row ends inside a unit
        sym = getBits(rowleft*K); val = (sym >> (rowleft*(K-pass))) & (2^rowleft-1)
        remain -= rowleft; u = rowleft; rowleft = 0
    putBits(val, u)
    if rowleft == 0:
        rowleft = R
        if ++pass > K: pass = 1; rowstart = pos     # next row
        else:          pos = rowstart               # decode the same row again
if B > 1 and output not full:
    codec 2 at the current pos, U = 8, total = the remaining R bits    # 0x42f447
```

Each encoded row is decoded K times. Pass p emits unit p of every symbol, most significant first.
With K = 2 one stream row of 16-bit symbols therefore holds two output rows: the high units, then
the low units. The 28 images with B = 2 use U = 4, K = 2: an 8-bit symbol pairs a nibble from the
top half of the image with the nibble at the same offset in the bottom half. Codes `00`, `01`,
`10xxx` and `11`+literal amount to a small fixed prefix code over a 10-entry dictionary of
frequent symbols.

Corpus parameters: (U, K, B) = (8, 1, 1) in 500 images, (4, 1, 1) in 14, (4, 2, 2) in 28. Type
0x81: (8,1,1) ×4, (4,2,2) ×2, (8,1,19), and (4,2,55) in TheTerminator. The last has an odd B with
K = 2, so it exercises the codec-2 tail; it matches the oracle.

Quirk: when `outSize` ends inside a unit, the original writes the top bits but does not reduce its
output counter. It then keeps decoding until the rows run out, writing past its buffer. Nothing
inside the buffer changes, so the port stops there.

## Codec 4: bit-run RLE for 1-bit data (`0x432180` / `0x43228b`)

Parameter: **L** (4 bits). Then come runs of L bits each: `run = getBits(L) & 0xff`. If
`run > 2^(L-1) - 1` the run is of ones (fill = 0xff) and `run &= 2^(L-1) - 1`; otherwise it is
of zeros. Each run is emitted as `putBits(fill, 8)` for every whole 8 bits, then
`putBits(fill, rest)`. Because `putBits` does not mask, a short run of ones written as 0xff also
sets the bits to its left within the window. That is the original's behaviour and the port keeps
it. The decoder refuses (writes nothing) when `outSize*8 > total bits`. The original never
finishes on `L == 0` or on zero-length runs past the data; the port returns
`EXEN_CODEC_ERR_CORRUPT` instead.

**No corpus user.** The type-0x81 resources that looked like codec 4 in earlier surveys were the
`u16 w, h` prefix (for example `48 03 08 00` = 840×8) being read as the codec nibble.
Validated on 2,940 synthetic streams.

## Codec 5: LZ77 (`0x432a00` / `0x432a76`)

Bytes 9..11 hold the unpacked size (24-bit LE). The decoder uses this size and **ignores
`outSize`**. From byte 13 the stream is groups of a 32-bit BE control word followed by 30 items.
The control bits are taken from the MSB down; the low 2 bits are `c`, which is not a flag.

- bit 0: one literal byte.
- bit 1: a u16 BE match word: `len = (w >> (14-c)) + 3`, `dist = (w & (0x3fff >> c)) + 1`, copy
  byte by byte (overlapping), clamped to the end.

Decoding stops as soon as the output reaches the size. `natives/image_decode.c.txt` has the C
form. The port clamps the output to `dst_cap` and returns `EXEN_CODEC_ERR_SIZE` if the declared
size is larger (the original would overrun). Codec 5 is used by 1336 images and appears in all
49 files. For comparison, codec 3 appears in 44 files (43 games plus flash), codec 1 in 31 and
codec 2 in 7.

## Image container ("ExEn PNG", type 0x0d)

`Resource.readImage(keepPalette)` (framework bytecode, `exen.Resource` class block @0x1cc)
reads `u16 w, u16 h` and throws if either is 0 or fewer than 4 bytes remain. Then:

- **Type 0x01/0x81** (`isABitmap`: `type & 0x7f == 1`): `readBytes(((w+7)>>3)*h)` (unpacking a
  0x81 resource) → `Image.createImageFromRawData(w, h, byte[])`, a 1-bit bitmap. So a 0x81
  resource is `u16 w, u16 h, packed stream`.
- **Otherwise:** reads one more u16, which is the byte length of the PNG stream that follows (the
  resource itself is padded to 4 bytes). Then `TransformBitmapFromResExed(resource)` decodes at
  the cursor (offset 6): PNG if the signature is there, raw 1-bit bitmap if not.

The PNG stream is the 8-byte signature followed by `[u32 BE len][type][data]` chunks with no CRC
(`decodePng` `0x41dacd`; full pseudo-code in `natives/image_decode.c.txt`):

- IHDR is always bit depth 4 or 8 (1623 and 461 images), colour type 3, compression byte 1.
- PLTE has `len/3` entries.
- tRNS: per-entry alpha. The simulator keys transparency on the last alpha-0 entry and has the
  alpha-into-blue bug documented in NATIVES.md.
- The first IDAT is one packed stream; decoding stops there. There is no IEND.
- **No per-row filter bytes:** the codec output is exactly `pitch * h` bytes, with
  `pitch = (w+1)/2` at depth 4 (left pixel in the high nibble) and `w` otherwise. For bit depths 1
  and 2 the simulator also unpacks `w*h` bytes (one per pixel). No game uses them.

`exen_image_decode` returns the IHDR fields, the packed buffer (`raw`, as the simulator holds it
with keepPalette = true), one index byte per pixel (`pixels`), the palette, the tRNS alphas, the
simulator's colour key (last alpha-0 index) and the number of alpha-0 entries, which selects
blit mode 0x50 or 0x30.

### Non-PNG and empty type-0x0d resources

- **Raw 1-bit bitmaps:** SexyBreakerBikini and SexyManga resources 3 and 33 (8×8). What follows
  the header is actually a codec-1 stream (`10 00 00 04 …`) that unpacks to an all-white tile.
  The simulator does not unpack it: `decodeBitmap` `0x418d0a` reads the first 8 bytes as raw
  bitmap rows. `exen_image_decode` does the same and returns codec 0.
- **Empty placeholders**, which `readImage` would throw on or read past the resource end for:
  AgeOfEmpiresMobile 26 (0 bytes), Charmed 161 (8 zero bytes, 0×0), FootballFans 14 (header says
  200×25, no data; the simulator would read the next resource's bytes), TombRaider2 63 and 64
  (0 bytes). The port returns an error for these.

## Simulator bug: in-place staging corrupts some codec-3 images

`decodePngDriver` `0x41e45f` loads the resource into the **tail of the pixel buffer** when
`Resource._len < cap` and decodes in place. Here `cap = pitch*h` with keepPalette at 4 bpp, and
`w*h` otherwise. It reads `_len` bytes from the cursor, which runs 6 bytes past the resource.
The original was emulated with exactly this staging (`OUT + cap - _len`). Codecs 1, 2 and 5 come
out identical, but **26 codec-3 decodes differ** from the clean decode by 1–425 bytes: 22 with
keepPalette and 4 without. They differ because codec 3 checks for stash overlap against the *row
start*. When more than 0x400 bytes of input remain at the moment the output reaches the row
start, it never stashes, and later output overwrites input that has not been read yet.

Examples are 007Ice-Racer 8/29/30, BanjoKazooie 32, Charmed 118/119, MotoGp 6, Reversi 15/16,
ShadoFighter 17/29, Spyro 45, TourDeFrance 32, Worms 37 and ZooTycoon2 22/38/57. The visible
effect is stray pixels. For ShadoFighter 29 (the flame), the bottom ~15 rows become
transparent. The clean decode is evidently what the artists drew. **The port decodes into a
separate buffer and so does not reproduce this.** A pixel-exact match with the simulator would
need its staging layout and its heap contents past the buffer; it is left as an open question.

## Open items

- Codec 4 has never been seen in a real game.
- Whether to reproduce the in-place corruption above (current choice: no).
- Bit depths 1 and 2 in PNG IHDR and truecolour images (the simulator draws "True color PNG not
  supported" into them) do not occur in the corpus. The port returns
  `EXEN_CODEC_ERR_UNSUPPORTED` for non-indexed images.
