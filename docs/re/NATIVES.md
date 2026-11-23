# gensimu.exe: the native layer under the bytecode

This document covers everything in `vendor/gensimu/gensimu.exe` (EXEN-V2 Generic Simulator, SDK 2.0.7,
built 2003-05-15) that runs below the 4CVP bytecode:

- how a `native` method is bound to C code, and every native method
- the EXManuf* porting layer
- the Win32 host loop and its timing, including why it burns CPU and why games run slowly
- the device profiles

It is written for reimplementing the layer in portable C.

All addresses are virtual addresses in gensimu.exe (image base `0x400000`). Disassembly was done with
capstone. Confidence tags: **[H]** read directly from code and/or confirmed by running; **[M]** read from
code with some inference; **[L]** a guess.

Companion material:
- `docs/re/natives/*.c.txt` holds C-like decompilations of the complex routines (rasterisers, image
  decode, PlayField, Resource, Math, SMS, the melody player, ...).
- The 4CVP class format is in `docs/re/CVP.md` and the interpreter in `docs/re/OPCODES.md` (other
  agents).

The binary was clearly built **without optimisation** (every local round-trips through `[ebp-x]`,
there is no register allocation, and frame pointers are everywhere). Expect every routine below to be
3 to 10 times slower than the same C compiled with `-O2`.

---

## 1. Native-method binding [H]

### 1.1 Method table in a 4CVP class

Each class block has a u16 at block offset `+0x20` that gives the offset of its method table:

```
u16 count; (pad to 4)
struct { u32 hash; u16 access; u16 nargs; u16 off; u16 _ } method[count];
u16 sigoff[count];   // starts at (&method[count]) - 2, i.e. overlaps the last entry's final u16
```

- `access` uses the standard Java ACC flags: `0x0001` public, `0x0002` private, `0x0004` protected,
  `0x0008` static, `0x0010` final, `0x0100` **native**, `0x0400` abstract.
- `nargs` is the argument word count, with long/double counting as 2 and `this` not counted.
- `sigoff[k]` points at the signature record of method k: `u16 nameoff; u16 0; type ret; u16 n;
  type args[n]`. `nameoff` points at a `u16 len, char name[len]` (padded to even).
- Type codes: `0x00` V, `0x59` I, `0x15` S, `0x90` B, `0xd5` Z, `0x55` C, `0x6e` J. `0x99` is
  followed by a u16 offset to a u32 *global class index*, giving L<class>. `|0x100` marks an array.
- Class hashes are CRC-32 of the dotted name (`zlib.crc32("exen.Graphics") == 0xc6ed8e2a`). The table is
  at `0x4548a0` and the function at `0x4105f5`.
- Method hashes are not plain CRC-32 of the name. They seem to be derived from the signature in some way
  (methods with the same signature share the high 16 bits). They were not needed here.

The parser in the scratch tooling recovered every framework method with this layout.

### 1.2 Native index and dispatch

For a method whose `access & 0x100` is set, `off` points at a **u32 native index** inside the class
block. The index selects a slot in the **native function table at `0x457c08`**: 185 `u32` function
pointers, indices 0..184, terminated by a 0 at index 185.

- **Invoke** (`0x40e02c`): if `method.access & 0x100`, the VM reads `idx = *(u32*)(classblock +
  method.off)` and calls `0x407a94(idx, sp_args, obj)`. That in turn calls `table[idx](u32 *args,
  Object *self)`.
- The native returns a `u16` count of result words. The VM advances its stack pointer by
  `count*4` (`0x40e081..0x40e09e`).
- After the call the VM checks the exception and abort flags (`[0x51f900]+0x14` and `+0x1c`).

The native ABI (confirmed across dozens of natives):

```c
typedef uint16_t (*native_fn)(uint32_t *args, Object *self);
// args[0] = this (instance natives) or the first parameter (static natives); args[1..] = params
// long/double: two words, low word first
// result is written to args[0]; return value = number of result words (0 = void, 1 = int/ref/bool)
// object refs in args[] are raw object pointers; instance fields start at obj+0x18, 4 bytes each
// arrays: u16 count at +0x12, data at +0x14 (char[] is 1 byte/elem)
```

Fatal errors call `0x407a13(code)` ("non catcheable interrupt"), which aborts the VM. Java exceptions
are thrown with `0x410198(crc32(classname))`.

There is also a link-time variant, `0x410f92` → `0x407aae`. It looks up `{u32 methodhash, u32
classhash, fn}` triples in a table, intended for classes loaded at run time that carry native methods
(`vm.sys.Runtime.createTempClass`). It searches the same address `0x457c08` using a 12-byte stride,
which cannot match the plain pointer table. It never succeeds in practice: dead or broken code
[M]. The framework only ever uses the prelinked indices.

**Native surface: 185 natives.** The framework's other 400-odd methods are bytecode. By class: Graphics 15, Image 15,
Resource 13, AnimBitmap 3, PlayField 8, AnimFlash 11 (all stubs), Displayable 2, Gamelet 22, Sms 12,
DialogBox 2, FX 6, List 1, Math 13, Matrix3D 6, Vector3D 8, RayCast 10, util.Debug 4, Object 4,
Class 3, String 8, StringBuffer 9, vm.sys.Runtime 3, catalog.* 7.

---

## 2. Native method catalog

The tables below are generated from the parsed class blocks plus disassembly of each target. Sections
2.1 to 2.4 were written by sub-analyses and folded in here. Helper calls are given as `→ 0xADDR`.

### 2.1 exen.Graphics and exen.Image (0–29)

The field layouts (Graphics `pixIma`+0x18, `curColorIDX`+0x1c, `curPaintMode`+0x20, `clip*`+0x24..+0x30; Image `width`+0x18, `height`+0x1c, `depth`+0x20, `image_Data`+0x24, `image_imgT`+0x28 holding the 0x58-byte native `EXimg`, `palette`+0x2c) and the decompilations are in `natives/eximg_common.c.txt`, `graphics_raster.c.txt`, `graphics_drawImage.c.txt`, `graphics_font.c.txt` and `image_decode.c.txt`.

| idx | method (sig) | C addr | semantics | conf |
|---|---|---|---|---|
| 0 | Graphics.clearRect(IIII)V | 0x425c73 | fillRectCore(x,y,w,h, colour **0xFF**, mode 1). Always white, whatever the current colour. Clipped to the clip rect. | high |
| 1 | Graphics.drawImage(Image,I×10)V (pkg-private) | 0x425699 | args: img, dx,dy,dw,dh, *unused*, sx,sy,sw,sh, transform 0-5. If sw==dw and sh==dh it does an unscaled blit with flips/rotations, else a nearest-neighbour scale (transform ignored). Rejects src depth > dst depth. The public overloads are bytecode. | high (transform 4/5 orientation medium) |
| 2 | Graphics.drawLine(IIII)V | 0x4257a1 | Cohen-Sutherland clip (integer division, int16 truncation), then Bresenham. Both endpoints are drawn. A single point calls setPixel. | high |
| 3 | Graphics.drawTriangle(IIIIII)V | 0x42585a | Closed 3-point polyline via 0x417e45. | high |
| 4 | Graphics.drawRect(IIII)V | 0x425940 | Polyline (x,y)-(x+w,y)-(x+w,y+h)-(x,y+h), which covers (w+1)×(h+1) pixels. | high |
| 5 | Graphics.drawChars(C[],I,I,I,I,I)V | 0x425a50 | (data, off, len, x, y, unused). Bounds-checked. Built-in 5×8 font, advance 6, colour = curColorIDX. | high |
| 6 | Graphics.fillTriangle(IIIIII)V | 0x425ba4 | 16.16 scanline fill. Rows [ymin,ymax), spans inclusive. **Top clipping is wrong when clipY≠0.** | high |
| 7 | Graphics.fillRect(IIII)V | 0x425d20 | Fills exactly w×h with curColorIDX after clipping. | high |
| 8 | Graphics.fillTextureTriangle(IIIIII,TextureMap)V | 0x425dd8 | Affine-mapped triangle. u,v come from TextureMap.ptU[0..2]/ptV[0..2] (16.16 +0.5) and the texture is pixImage. The target must be 8bpp without a palette, and the source must have a palette (or the original crashes). No wrapping. | medium |
| 9 | Graphics.setPixel(III)V | 0x425f6e | Writes the raw byte `c&0xFF` (not curColor) if the pixel is inside the clip rect. | high |
| 10 | Graphics.getPixel(II)I | 0x426015 | Returns the raw stored byte (RGB332, palette index, or nibble). Returns **-5** if the pixel is outside the clip rect. | high |
| 11 | Graphics.setClip(IIII)V | 0x426096 | clip = requested ∩ image bounds, w and h clamped ≥0. Half-open. | high |
| 12 | Graphics.setNormalVideo()V | 0x426172 | `ctx20->normalVideo(+0x24)=1` (display flag only). | medium |
| 13 | Graphics.setInverseVideo()V | 0x426184 | Same flag = 0. | medium |
| 14 | Graphics.setColor(III)V | 0x426196 | rgb=r<<16\|g<<8\|b (unmasked). If the target has a palette, picks the nearest entry by squared RGB distance (first minimum wins). Otherwise uses RGB332 (8bpp) or 0xFF (4bpp, -1). The result goes to curColorIDX. | high |
| 15 | Image.updateNativePaletteFromJavaPalette()V | 0x426235 | Aliases the native palette to the Java colorTabRGBA, sets the LUT version pointer, and sets blitMode 0x40 (**which drops a transparency key or alpha mode**). With a null palette it clears the palette and sets 0x10 unless the mode is 0x20. | high |
| 16 | Image.getNativePaletteSize()I | 0x426302 | Returns e->palCount. | high |
| 17 | Image.updateJavaPaletteFromNativePalette()V | 0x426357 | Copies the native palette into Java colorTabRGBA (Java length), frees the native buffer, and re-aliases. | high |
| 18 | Image.setPaletteAlpha(Z)V | 0x426419 | alphaMode=b. On true, mode 0x30. On false, mode 0x40 with a palette or 0x10 without (the key is lost). | high |
| 19 | Image.setTransparentColor(I)V | 0x4264a3 | transpIdx=c&0xFF. Mode 0x50 with a palette, else 0x20. | high |
| 20 | Image.getTransparentColor()I | 0x4264ed | Returns transpIdx (0 if no struct). | high |
| 21 | Image.removeTransparentColor()V | 0x426548 | Mode 0x40 with a palette, else 0x10. | high |
| 22 | Image.getSizeOfEXimgStruct()I static | 0x426210 | Returns 0x58. | high |
| 23 | Image.getManufDisplayHeaderSize()I static | 0x426222 | Returns 0 (0x4022c4). | high |
| 24 | Image.Init(III)V | 0x4267f6 | (w,h,depth) → 0x4176c7. Sets the depth and the ops table (only 4 and 8 have one), then ops.init: pitch = w or (w+1)/2. **New 8bpp images are filled with 0xFF.** Clears the palette, LUT key and alpha fields. | high |
| 25 | Image.transformNativePaletteToRawRGB()V | 0x426589 | data[i] = low byte of pal[data[i]] (that is, the **blue** byte). transpIdx is remapped the same way. Reproduce literally. | medium |
| 26 | Image.TransformBitmapFromResExed(Resource,Z)V | 0x4265ca | Decodes the image at the resource cursor (PNG or raw 1bpp) into this image. Z = keepPalette. | high |
| 27 | Image.GetBitmapDepthFromResExed(Resource,Z)I static | 0x42664c | Returns 4 for non-PNG, 4 for indexed 4-bit with Z, else 8. | high |
| 28 | Image.TransformBitmapFromResExed(B[],Z)V | 0x4266a1 | Same as 26, from a byte array. | high |
| 29 | Image.GetBitmapDepthFromResExed(B[],Z)I static | 0x426732 | Same as 27, from a byte array. | high |

#### Pixel formats and palette

- **The display and default images are 8bpp RGB332.** `getBitmapDepth()` is the constant 8
  (0x4022ba) in every device profile, so the mono and gray profiles render RGB332 internally too.
  The format is R = bits 7-5, G = bits 4-2, B = bits 1-0: `(rgb>>16&0xE0)|(rgb>>11&0x1C)|(rgb>>6&3)`
  (0x42d373).
- There are two image kinds:
  - **raw 8bpp**: the bytes are RGB332 and there is no palette.
  - **paletted 8bpp or 4bpp**: the bytes are indices into `pal` (0xAARRGGBB). In 4bpp, the high
    nibble is the even x and the low nibble is the odd x, with pitch (w+1)/2.
- Indices go through a single global 256-byte RGB332 LUT (0x42d279) when blitting. The LUT is
  cached by `*palVersion`, the address of Palette.paletteVersion. It is rebuilt whenever the version
  changes, and on every blit when an image has no version pointer.
- Depth 1 has no ops table, so 1-bit images cannot be Init'ed. Raw 1bpp resources are expanded to
  8bpp: either 0x00/0xFF, or indices 0/1 with palette {0x00000000, 0xFFFFFFFF}. Bit 1 is white.
- Depth-ops tables are at 0x45bca8 (4bpp) and 0x45bcf8 (8bpp). On a 4bpp *target*, drawImage,
  texture spans and colour conversion are stubs: 4bpp images can only be blit sources.

#### Paint modes

**PM_COPY/PM_INVCOPY/PM_OR/PM_AND/PM_XOR/PM_NAND have no effect in this simulator** (high
confidence). Every native passes curPaintMode to `ops->setPaintMode` (slot 12), which is
`return 1` in both tables (4bpp 0x42baf3; 8bpp 0x42c351 → 0x42d46d). Every primitive does plain
stores. The 0x45bca8/0x45bcf8 tables are per-depth op vtables, not paint-mode tables.

#### Blit modes and transparency (per source image, `EXimg.blitMode`)

| mode | meaning | 8bpp row blitter (unscaled / scaled) |
|---|---|---|
| 0x10 | opaque, raw | 0x41a3d2 / 0x41a794 |
| 0x20 | colour key, raw (key compared on the stored byte) | 0x41a48f / 0x41a85a |
| 0x30 | palette alpha (pal>>24: 255 copy, 0 skip, else per-field RGB332 lerp) | 0x41a511 / 0x41a94d |
| 0x40 | opaque, palette LUT | 0x41a70b / 0x41ab18 |
| 0x50 | colour key on the index, palette LUT | 0x41a66c / 0x41abfb |

The transitions are listed in `graphics_drawImage.c.txt`. There are several lossy transitions:
updateNativePalette → 0x40, and setPaletteAlpha(false) → 0x40/0x10.

#### Clipping

- Clip boxes are half-open. setClip intersects the request with the image bounds.
- Every primitive clips against the Graphics clip, copied into EXimg before each call (0x425650).
  Image bounds matter only through setClip.
- Lines are clipped geometrically, with the inclusive max = clip+size-1.
- fillRect and drawImage clip the rectangles.
- Triangles clip per scanline span. The top-edge adjustment has a bug that is correct only when
  clipX/clipY = 0.
- Text clips whole characters, then glyph pixels.
- getPixel and setPixel use the clip, not the image bounds.

#### Font and text

The font is built in: 256 glyphs of 5×8, a 1-bit strip at **0x454dd0** (stride 160 bytes, 8 rows,
glyph c at bit column 5c). It inks where the **bit is 0**, MSB first. The advance is 6 and the
nominal height is 9. The font object (0x413a40) is created at startup and is not loaded from the
game or the language ini. A 0xFF char terminates a string. `drawGlyph` writes one byte per pixel,
so text on a 4bpp image corrupts it. The "FntBuild" code (0x436fdc, TextOutA+GetPixel) builds a
font for the simulator's own UI at startup only. It is Win32-only and not part of the VM.
getCharWidth/getCharHeight are bytecode and are not in this range.

#### Image decode: "ExEn PNG" (high confidence, checked against flash/*.exn)

- The format is the PNG signature plus `[u32 BE len][type][data]` chunks with **no CRC and no IEND**.
  IHDR compression is 1, and the corpus is all 8-bit indexed.
- **IDAT is not zlib.** `data[0]>>4` selects one of 5 In-Fusio codecs: 1 0x431750, 2 0x42f560,
  3 0x42d500, 4 0x432180, 5 0x432a00. The corpus has 28 images of type 5 and 3 of type 3. The
  unpacked size is the 24-bit LE value at data[9..11]. There are no filter bytes.
- Codec 5 is a simple LZ77 (32-bit BE control words, 30 flags each, 14-(ctl&3) distance bits) and is
  fully decompiled. **Codecs 1-4 are not decompiled yet** (bit-reader based, 600-2800 instructions
  each). This is the main open item for porting images.
- keepPalette = false (the usual case for 8bpp) folds the palette to RGB332 (raw image).
  keepPalette = true keeps the indices and a 0xFF-alpha palette.
- tRNS: without keepPalette the key is the RGB332 of the last alpha-0 entry (mode 0x20). With it,
  one alpha-0 entry gives mode 0x50 and two or more give mode 0x30.
  **Bug: the alpha is ORed into the blue byte and the alpha byte is zeroed.**
- Truecolour draws "True color PNG not supported" into the image.
- The decoder works in place: the compressed resource is staged at the tail of the pixel buffer.
  Resource file reads go through a 30-byte cache window (0x41d620).

#### Performance notes

- Per primitive: one syncClip, plus an indirect setPaintMode no-op call, plus one indirect call per
  scanline span (triangles) or per row (blits). Lines use one call for the whole line.
- There are **no per-pixel function calls** in any Graphics/Image native, and no GDI use. GetPixel
  appears only in the startup FntBuild.
- LUT rebuild cost is up to 256 entries per drawImage when consecutive sources have different
  palette versions, or have no version.
- Nothing in this range explains a low framerate. Look at the display path
  (EXManufDisplayUpdate 0x4342f0 float/_ftol per pixel) and the host loop.

#### Findings from the port (src/core/graphics.c)

- **Image bytecode** (`tools/disasm.py` on exen.Image) [H]: `<init>(w,h,d)` allocates
  `image_Data = new byte[getDataSize(w,h,d)]` with `getDataSize = ((w*d+7)>>3)*h +
  getManufDisplayHeaderSize()`, then `image_imgT = new byte[getSizeOfEXimgStruct()]`, then calls
  `Init(w,h,d)` and `setPalette(Palette.createPalette())`. `setPalette` stores the field and calls
  `updateNativePaletteFromJavaPalette`. `Palette.createPalette()` makes a Palette whose
  `colorTabRGBA` is **null** (and returns null outright when `numColors() == 2`), which is why
  native 15 keeps mode 0x20 for a null table: raw RGB332 images keep their colour key.
- **Image loading** [H]: `Resource.readImage(keep)` reads `u16 w, u16 h`. If `type & 0x7f == 1`
  it reads `((w+7)>>3)*h` raw bytes and calls `createImageFromRawData(w,h,bytes,keep)`; otherwise
  it reads the u16 stream length and calls `createImageFromRawData(w,h,this,keep)`, which decodes
  at the resource cursor (just past the 6-byte header). Both helpers construct the Image with
  `GetBitmapDepthFromResExed` (so raw 1bpp data becomes a **4bpp** image), call
  `TransformBitmapFromResExed`, then: native palette size != 0 → `palette =
  Palette.createPalette(n)` + `updateJavaPaletteFromNativePalette`; else keep →
  `setPalette(createPalette(Gamelet.getPalette()))`; else `setPalette(createPalette())`.
  `Resource.getImage(id)` uses keep = true, `getRawImage(id)` keep = false.
- **ops4 import1bpp (0x42b515)** [H]: pitch = (w+1)/2; rows are byte aligned in the source; two
  source bits make one output byte through a table: keep → {0x00,0x01,0x10,0x11} (indices 0/1 per
  nibble), else {0x00,0x0f,0xf0,0xff}. keep sets a malloc'd palette {0x00000000, 0xffffffff},
  palCount 2, mode 0x40; otherwise mode 0x10. Sets `+0x54 = 1`.
- **4bpp setBlitMode (0x418f00)** [H]: stores `mode & 0xf0`; modes 0x10 and 0x20 call
  `setBlitMode8`, i.e. the 8bpp row blitters, which read whole bytes of the packed source (so a
  4bpp image without a palette draws as garbage in the original); 0x30 uses 0x4190f6/0x419e64 for
  every transform; 0x40 and 0x50 pick per-transform blitters from the low nibble (the transform
  ORed in by ops8 blit).
- **drawImage transforms 4 and 5** [M, from the pointer set-up in ops8 blit]: with `(i, j)` the
  destination offset and the source rect `[sx0,sx1) x [sy0,sy1)`, transform 4 reads
  `src(sx1-1-j, sy0+i)` (90° counter-clockwise) and transform 5 reads `src(sx0+j, sy1-1-i)` (90°
  clockwise). 1 flips vertically, 2 horizontally, 3 both.
- exen-reloaded keeps the EXimg inside the image_imgT byte[] as the original does, with arena
  references instead of pointers and 1 KiB of inline storage for decoder-created palettes, so
  `getSizeOfEXimgStruct` returns 0x458 instead of 0x58 (the layout is in graphics.c).

### 2.2 exen.Gamelet (67–88)

| idx | method (sig) | C addr | semantics | conf |
|---|---|---|---|---|
| 67 | Gamelet.isColor()Z (static) | 0x424f70 | `depth != 1` where depth = `0x4022ba()` = **constant 8**, so it always returns 1 | H |
| 68 | Gamelet.numColors()I (static) | 0x424f86 | `1 << 8` = 256 | H |
| 69 | Gamelet.getBitmapDepth()I (static) | 0x424fac | constant **8** | H |
| 70 | Gamelet.getScreenWidth()I (static) | 0x424f99 | display struct `[ctx+0x20]+0` (= profile width, clamped by 0x40224b) | H |
| 71 | Gamelet.getScreenHeight()I (static) | 0x424fbf | `[ctx+0x20]+4` | H |
| 72 | Gamelet.screenUpdate(I,I)V (private) | 0x424ff2 | **Ignores both ints.** The screen Image is Gamelet field `+0x1c`, resolved to EXimg via 0x426785. Calls `0x402304`, the centre/format logic, then EXManufDisplayUpdate, which converts and **paints synchronously** (§4.3). A null image is fatal; a return of -2 is fatal ("memory leak"). | H |
| 73 | Gamelet.exitVm()V (private) | 0x424fd2 | Stops the melody (0x406628) and calls 0x40777b. Sets the VM exit flag `[ctx+0x24]=1`. | H |
| 74 | Gamelet.throwInternalException(I)V (static) | 0x42506c | Logs, stops the melody, fatal interrupt 0 | H |
| 75 | Gamelet.getTimerTickCount()I (static) | 0x425090 | `GetTickCount() - base` in **ms** (0x435d3f) | H |
| 76 | Gamelet.startTimer(I)V | 0x4250a3 | if `ms >= 0`: EXManufSetTimerTick(ms), clamped to 10..60000, then `SetTimer(hwnd,1,ms)` | H |
| 77 | Gamelet.stopTimer()V | 0x4250ba | `KillTimer(hwnd,1)` | H |
| 78 | Gamelet.sendSms(LSms;)V | 0x4250c7 | Gets the SMS native buffer (0x42989d) and stores the price index `sms.+0x34` (m_type) at buffer byte 0x8c. If the price is invalid (0x423b1d: the 0x15-byte number at cfg+0x190+0x1c*idx is empty) or there is no network (0x423a00(3): the SIM's MCC/MNC must equal cfg+0x160/+0x162, or both be 0), it stops timer and melody, clears `[0x51f904]+0x1e` and sets VM exit state 3. Otherwise `0x4218d3(buf,1)` sends it via EXManufSmsSend and the result arrives later as Bootstrap.smsSent(status). **Port deviation:** we do not halt. The message goes to the core's mock In-Fusio SMS server ([SMS.md](SMS.md)), which decides the status from the host's SMS mode (server / delivered: `SMS_SENT_SUCCESSFULLY`, static slot 2 = 1; offline: `SMS_SENT_UNSUCCESSFULLY`, slot 1 = 0) and may queue an answer for Bootstrap.smsReceived. The core runs smsSent after the current host callback returns (`call_bootstrap`, api.c), never re-entrantly. | H |
| 79 | Gamelet.saveCtx(B[])V | 0x425156 | Copies min(len,300) bytes into the gamelet's EEPROM record (`0x4153f8`, §3.1). Null is fatal. | H |
| 80 | Gamelet.loadCtx(B[])V | 0x4251d4 | the reverse (`0x41547b`) | H |
| 81 | Gamelet.playVibrator()V | 0x4253bc | If EEPROM `cfg+0x15e` is set: `EXManufVibratingDeviceRun(1000)` (0x4065e0(10)), which shakes the window for 1 s | H |
| 82 | Gamelet.playMelody(B[])V | 0x425252 | If EEPROM sound `cfg+0x15c` is nonzero: EXManufMelodyPlay(data, byte length) (§3.2). Null is fatal. | H |
| 83 | Gamelet.stopMelody()V | 0x4252df | EXManufMelodyStop | H |
| 84 | Gamelet.getNickName()C[] | 0x4252ec | 0x423b63 copies the 16 bytes at EEPROM **cfg+0x66** into a 28-byte stack buffer; 0x422f34 measures up to the first 0xff; new char[n]. The shipped eeprom.dat gives "Xcell". The port also stops at a 0 byte, so an all-zero EEPROM gives "". | H |
| 85 | Gamelet.setNickName()V | 0x425372 | 16-byte buffer filled with 0xff. Sets VM state `+0xc=0xa`. Opens `EXManufUIEditBox(title 0xb, max 15)` (modal busy loop) through 0x403d8a; the result comes back through Bootstrap.NickNameChanged. **Port deviation:** no modal UI; a no-op (logged once) that keeps the stored name. | M |
| 86 | Gamelet.getVersionInfo()I | 0x4253e3 | `(c[4]<<24)|(c[5]<<16)|(c[6]<<8)|c[7]` from EEPROM bytes 4..7, using **sign-extended** bytes (`movsx`). A byte ≥ 0x80 therefore ORs 1s into the higher bits. Shipped eeprom gives `0xffffff80`. | H |
| 87 | Gamelet.getProperty(LString;)LString; (static) | 0x425427 | Null → throws (crc 0x364a8d65, NullPointerException). Searches the pair table at `0x4492cc` (count byte `0x4492d4` = 2, i.e. 1 pair) with exact length + byte compare. **The only key is `"FrameWork"` → `"V2.00"`.** Anything else returns null. | H |
| 88 | Gamelet.platformRequest(LString;)Z | 0x42563e | always false | H |

Gamelet's other methods, including onKeyPress, onTimerTick, the constructors and so on, are bytecode.

Gamelet bytecode notes [H]: the constructor sets `Component.width/height` from
getScreenWidth/Height and creates the screen with `image = Image.createImage(w, h)` and
`gBuff = new Graphics(image)`; `update()` is `screenUpdate(1, 100)`. `<clinit>` defines the key
codes `KEY_NUM0..9 = '0'..'9'`, `KEY_STAR = '*'`, `KEY_POUND = '#'`, `KEY_PAD_UP = -1`,
`KEY_PAD_LEFT = -2`, `KEY_PAD_RIGHT = -5`, `KEY_PAD_DOWN = -6`, `KEY_CLEAR = -7`, `KEY_VAL = -8`,
and the game actions `UP 1, DOWN 6, LEFT 2, RIGHT 5, FIRE 8, GAME_A..D 9..12`. gensimu itself
never sends the KEY_PAD codes (arrows are mapped to digits, §4.2).

### 2.3 Resource, AnimBitmap, PlayField, AnimFlash, Displayable, DialogBox, FX, List, RayCast

Decompilations: `natives/Resource.c.txt`, `AnimBitmap.c.txt`, `PlayField.c.txt`, `FX.c.txt`. The
RayCast engine, DialogBox and List were translated directly into `src/core/raycast.c` and
`src/core/ui.c`, which carry the address of every routine; their summaries are below the table.

| idx | method | C addr | semantics | conf |
|---|---|---|---|---|
| 30 | Resource.init(I)B[] | 0x428aa0 | `_start=offsets[id]`, `_len=offsets[id+1]-offsets[id]`, `_offset=0`. Out of range: log and throw_internal(0). Always returns null. Does not set `_id`. | high |
| 31 | Resource.readBoolean()Z | 0x428b4e | reads 1 byte, returns `&1`. Past the end: 0, no exception | high |
| 32 | Resource.readInt()I | 0x428be5 | 4 bytes, **little-endian** | high |
| 33 | Resource.readShort()S | 0x428c79 | 2 bytes LE, sign-extended | high |
| 34 | Resource.readByte()B | 0x428d0d | 1 byte, sign-extended | high |
| 35 | Resource.readChar()C | 0x428da2 | reads **1 byte**, zero-extended. Writes only the low 16 bits of args[0] (quirk) | high |
| 36 | Resource.readBytes(I)B[] | 0x428e41 | new byte[n] (n truncated to u16). Tag 0x81: the rest of the resource is unpacked into it (in place). Otherwise a raw read, null if past the end. `_offset+=n` | high (codec: low) |
| 37 | Resource.readShorts(I)S[] | 0x42908e | short[n], raw LE, needs `n <= remain/2` | high |
| 38 | Resource.readInts(I)I[] | 0x429177 | int[n], raw LE | high |
| 39 | Resource.readUTF()LString; | 0x429265 | **big-endian** u16 byte length, then modified UTF-8. Decoded chars are truncated to 8 bits, char count truncated to u8 | high |
| 40 | Resource.readString(I)LString; | 0x4295df | skips k strings separated by 0xFF, returns the next one (up to 0xFF or the end). `_offset` is **not** advanced | high |
| 41 | Resource.getNbResources()I (static) | 0x4297fa | N (the `.exn` value at 0x34) | high |
| 42 | Resource.getResourceType()I | 0x429813 | `types[_id]`, or 0 if out of range | high |
| 43 | AnimBitmap.draw(Graphics,x,y,frame,transform)V | 0x42469c | frame -> rect, then blit to `g`. WITHMASK: the mask is the same rect shifted by image.width/2, blitted first. Mode codes 3 then 4 (inverse video: 4 then 3), which have **no effect**: drawImageCore 0x418008 passes the mode only to the source's setPaintMode slot (ops+0x30), a stub returning 1 in both vtables (8bpp 0x42c351 -> hook `[0x457f38]` = 0x42d463 `return 1`). So the mask is drawn through the source's own blit mode like any image | high |
| 44 | AnimBitmap.moduloFrame(I)I | 0x4245fe | positive `f mod (frameSequence.length or nbFrame)` | high |
| 45 | AnimBitmap.getRealFrame(I)I | 0x42467b | through frameSequence (if any), then mod nbFrame | high |
| 46 | PlayField.fillCells(cx,cy,w,h,v)V | 0x427013 | rect clipped to the map, fill with v | high |
| 47 | PlayField.moveTiles(x1,y1,x2,y2,w,h)V | 0x42712f | **Broken in gensimu.** Clips both rects against the map and picks one of 4 loop directions (jump table 0x427521, case = (x2<x1)*2 + (y2<y1)), but every loop body is `cellSet(x1, j, cellGet(x2, j))` with j = 0..h-1: the column/row counters are never added to the coordinates and the y arguments are unused after clipping. Net effect: column x2 rows [0,h) is copied to column x1, w times. No game in the corpus calls it. **exen-reloaded fixes this**: it moves the whole w×h block from (x2,y2) to (x1,y1) after the same clipping, iterating away from the overlap (tests/c/test_playfield.c) | high |
| 48 | PlayField.draw(Graphics,x,y)V | 0x427531 | draws tiles in the view window, then z-sorted sprites. See below. charSet or target image without native data: log and **fatal** 0x407a13(0) | high |
| 49 | PlayField.setCellTile(cx,cy,v)V | 0x426fa8 | bounds-checked store. Negative v becomes `v+(1<<nbBits)` | high |
| 50 | PlayField.getCellTile(cx,cy)I | 0x426f37 | bounds-checked load. Values >= `(1<<nbBits)-animTileMaxIndex` come back negative. 0 if out of range | high |
| 51 | PlayField.addSprite(Sprite)V | 0x427f95 | appends at the tail. Already linked (next or prev set): log and ignore; a sprite alone in a list has neither and is re-linked as the only element. null: log and **fatal** 0x407a13(0) | high |
| 52 | PlayField.removeSprite(Sprite)V | 0x4280bd | unlinks (searching from firstSprite by next). Not in the list: log. null: log and fatal 0x407a13(0) | high |
| 53 | PlayField.removeAllSprite()V | 0x428208 | clears every link, first=last=null | high |
| 54 | AnimFlash.initAnimFlash(B[])I | 0x4248b0 | stub, returns 0 | high |
| 55 | AnimFlash.draw(Graphics,Z)V | 0x4248c2 | stub (no-op) | high |
| 56 | AnimFlash.delete()V | 0x4248ca | stub | high |
| 57 | AnimFlash.setFrame(I)V | 0x4248d2 | stub | high |
| 58 | AnimFlash.getNbFrames()I | 0x4248da | returns 1 | high |
| 59 | AnimFlash.getNbLoops()I | 0x4248ec | returns 0 | high |
| 60 | AnimFlash.setPosition(I,I)V | 0x4248fe | stub | high |
| 61 | AnimFlash.setSize(I,I)V | 0x424906 | stub | high |
| 62 | AnimFlash.draw(Graphics,I,I,I,I)V | 0x42490e | stub (PlayField calls it for AnimFlash sprites) | high |
| 63 | AnimFlash.getWidth(I)I | 0x424916 | returns field `width` (+0x24), ignores the arg | high |
| 64 | AnimFlash.getHeight(I)I | 0x42492a | returns field `height` (+0x28) | high |
| 65 | Displayable.haveDisplayableCommand()Z | 0x424a60 | `cmdpos(0)` or `cmdpos(1)` (0x403f56 maps 0->2, 1->1), so it is **always true** in gensimu | high |
| 66 | Displayable.drawCommand(Graphics,String label,int pos)V | 0x424a9d | Temporarily sets the dst EXimg clip to the full image (0,0,w,h); the Graphics clip is not used. Then 0x4238f0(chars, len, img, pos) draws the label with the built-in font (`[0x45ff3c]+0xc`) in **colour 0**: y = img.h - 9 (font op +4, glyphH), n = min(img.w/6, len) (op +0x18), nothing if n < 1 or y < 0. x by `cmdpos(pos)` through jump table 0x4239e3: 1 -> x = 1 (left), 2 -> op +0x20 = img.w - n*6 (right), 3 -> op +0x1c = (img.w - n*6)/2 (centre, unreachable), 0 -> not drawn. So pos 0 is right-aligned and pos 1 left-aligned. Text via drawText (op +0xc). Clip restored afterwards. A null label is dereferenced | high |
| 101 | DialogBox.getNbLinesFormatted(String,width)I | 0x424940 | 0x423174 -> 0x40356e -> 0x4231db -> 0x4231f6(s, len, width, -1): the number of word-wrapped lines, see "DialogBox and List" below. A null String is dereferenced | high |
| 102 | DialogBox.drawBox(Graphics,width,height,scrollLine,Z cursor)V | 0x42497c | paint() passes getWidth(), getHeight(), m_scrollLine, m_doDisplayCursor. sync_clip, clear (white fill of (0,0,clipW,clipH)), then 0x42318d -> 0x403587 -> 0x4233ed with a block {title chars/len, 1 (title centred), text chars/len, m_justify, dst, scrollLine, cursor, m_startTextY, 0, width, height}. Layout below | high |
| 103 | FX.doRotazoom(g,img,angle,zoom,cx,cy)V | 0x424b70 | fx[0]=0x415b6a. Rotate/zoom over the whole destination, ignoring the clip. zoom is added to 1.0 (>0 shrinks). The source **always tiles**: masks for power-of-2 sizes (0x415d19), repeated subtraction otherwise (0x415e5d), both with the width as row stride. See FX.c.txt | high |
| 104 | FX.doMosaic(g,img,x,y,level)V | 0x424bfd | fx[1]=0x415fc6. Scaled copy whose sample steps come from 4.12 arithmetic, level >= 16 (16 = identity). No clip | high |
| 105 | FX.doShiftHorizontal(g,img,x,y,byte[])V | 0x424c84 | fx[4]=0x41688d. Row r moves by (s8)offs[r]+x; the array length is not checked. Uses the destination clip **and the source image's stored clip height** | high |
| 106 | FX.doShiftVertical(g,img,x,y,byte[])V | 0x424d62 | fx[5]=0x416b75. Column c moves by (s8)offs[c]+y; uses the source's stored clip width; a negative x reads the source from 2*abs(x) | high |
| 107 | FX.doShutterVertical(g,img,step,Z)V | 0x424e40 | fx[2]=0x4162d0. Column blinds in groups of 8 from the data origin (the clip only bounds the size); columns revealed per group from 0x4165f5 | high |
| 108 | FX.doShutterHorizontal(g,img,step,Z)V | 0x424ed2 | fx[3]=0x416715. Row blinds, h/8 groups of whole source rows, no clipping | high |
| 109 | List.drawList(Graphics,width,height)V | 0x426870 | Builds `{chars,len,property}[]` from `m_captions` and `m_properties` in a do-while (an empty list reads item 0 and loops ~2^32 times). sync_clip, then 0x41b910 -> 0x403598 -> 0x41b950, see below | high |
| 137 | RayCast.draw(Graphics,x,y,angle)V | 0x4284c9 | 0x4282b0 points the engine at the fields; null map or wallgraph_width_poxOf2 is logged and returns; wall EXimgs 1..15 and every sprite's images are refreshed; sync_clip; render 0x41f4db. See "RayCast engine" | high |
| 138 | RayCast.isThereAWall(I,I)I | 0x428683 | 0x41f5b9: cell ((x>>16)/64, (y>>16)/64); 0 outside the inner map (cx,cy >= 1, idx in [W, (H-1)*W)); else 0 only if cell.hi, cell.lo, below.hi and right.lo nibbles are all set, otherwise 1 | high |
| 139 | RayCast.addMonster_internal(id,w,h)I | 0x4286c9 | 0x42022a: active=1, images cleared, size and source rect (0,0,w,h); -5 for a bad id | high |
| 140 | RayCast.findFirstSpriteFreeID()I | 0x428716 | 0x4201bf: first inactive sprite, -1 if none, **0 when there is no sprite list** | high |
| 141 | RayCast.removeSprite(I)V | 0x42874b | 0x4202c9: active=0 | high |
| 142 | RayCast.moveSprite(id,x,y,z)V | 0x42877a | 0x420511: 16.16 world position; z is the height above the floor | high |
| 143 | RayCast.setSpritePos(id,sx,sy,sw,sh,mx,my)V | 0x4287be | range check against num_monster_Max, refresh the sprite's images, then 0x420302: rect (sx,sy,sw,sh) in the sprite image and (mx,my) in the mask, validated against both | high |
| 144 | RayCast.setSpriteSize(id,w,h)V | 0x4288d3 | 0x420412: world size, both > 0 | high |
| 145 | RayCast.changeInternalValues_internal(viewW,viewH,wallH,step,horizonOff,maxSteps)V | 0x428910 | 0x41f0da, see below. **Sets exen.Math's sine period** to 6*viewW as a side effect | high |
| 146 | RayCast.castRay(x,y,angle,int[] out)I | 0x428962 | 0 if out is null or shorter than 6. Sets and clamps the position (0x41f3fe), casts one ray (0x41f7f2): out = {hitX, hitY, raw distance, corrected distance, wall nibble, texture column}; returns 1 | high |

RayCast fields: `wallGraph_Table` (Image[16]), `wallgraph_width_poxOf2` (byte[16], log2(width)-3),
`num_monster_Max`, `spriteImages`/`maskImages` (Image[]), `internal_rayc_values` (byte[452]),
`internal_sprite_list` (byte[84*max]), `sliceInfos` (byte[8*viewW]), `maze_map_pointer`
(byte[W*H]), `mapWidth`/`mapHeight`.

#### RayCast engine ("exrayc", 0x41f0da..0x420ca8) [H]

Read instruction by instruction and translated in `src/core/raycast.c`, whose header comment
gives the layout of the engine state and of a sprite. Summary:

- **State** lives in `internal_rayc_values`. Every native first runs 0x4282b0, which points a
  global (`[[0x45ff3c]+0x2c]`) at it and copies the map, slice and sprite-list pointers, the map
  size and the sprite count into it. A null `internal_rayc_values` is fatal (interrupt 0x27). Three
  84-byte nodes inside it hold the sprite blit parameters, the mask blit parameters and the head of
  the draw list.
- **Units**: positions are 16.16 with 64 units per map cell. Angles use a circle of `6*viewW`
  units (the field of view is `viewW` units, 60 degrees), through exen.Math's getSin/CosOfPeriod
  with the period that changeInternalValues sets globally. draw() switches the period to the
  engine's and back; **castRay does not**, so it uses whatever period the game last set.
- **changeInternalValues(w, h, wallH, step, hoff, steps)** (0x41f0da): only positive values are
  taken, and `step` only if it is 1 or 2 (2 rounds w down to even). Derived: rays = w/step,
  half FOV = w/2, quarter turns 3w/2, 3w and 9w/2, circle 6w, horizon = h/2 + hoff (h is used even
  when it is <= 0), projection distance = |(w/2)*cos(w/2) / sin(w/2)| (1 if the sine is 0).
  Java's init passes (w, h, 64, 1, 0, 16).
- **Rays** (0x41f7f2): the classic two-pass grid walk. The horizontal-line pass halves sin/cos with
  `sar` (floor), the vertical-line pass with a truncating division. Slopes are `(c<<16)/s`, or
  0xc80 when the divisor is 0. The first intersection is biased by +-0x7fff. Each pass walks at
  most `maxSteps` cells, and an exact quarter angle skips its pass. A map byte's high nibble is the
  wall seen across horizontal grid lines, the low nibble the wall across vertical ones. Distances
  are delta/sin or delta/cos in whole units; the nearer pass wins (a tie goes to the vertical
  pass). The fisheye correction multiplies by cos(angle - **the angle of the last draw()**). The
  texture column is the hit offset inside the cell, mirrored by step direction.
- **Walls** (0x41fee7): one slice per ray, from the clip's x to the end. Wall height is
  `wallH*projDist/dist`, centred on the horizon. The column renderer is the destination's ops
  slot 17 (8bpp 0x42ccc4, a no-op on other depths): 1 or 2 pixels wide, clipped vertically only,
  texture row stride `8 << log2width`. Raw textures copy bytes; paletted 8bpp and 4bpp textures go
  through the global LUT. Floor and ceiling are not drawn: the game paints them first.
- **Sprites** (0x42055c, 0x42080d): active sprites are rotated into view space (products of `>>8`
  values), projected (scale = projDist/depth), culled, and inserted in a far-to-near list. Each one
  is drawn with drawImageCore (scaled, with the source's own transparency) in runs of columns where
  it is nearer than the wall slice. The mask image is validated by setSpritePos but never drawn.
- Quirks the port keeps: the `0x7fff` bias, the bounds of isThereAWall, findFirstSpriteFreeID
  returning 0 without a list, castRay's dependency on the global period, and draw's use of clipX as
  the first slice index even when the column step is 2.

#### DialogBox and List (0x4231f6, 0x4233ed, 0x41b950) [H]

- **Word wrap**, 0x4232ee(s, len, max) with max = width/6: scan to a space, a '\n' or the limit. A
  '\n' ends the line and counts in it. After a space, it recurses on the rest with `max-i-1` and
  takes the longer line when `total < max` (strict). Words longer than a line are split. The scan
  reads the byte after the string.
- **Line count**, 0x4231f6(s, len, width, limit): repeat {take a line; skip one following space}.
  With `limit >= 0` it returns the byte offset where line `limit` starts instead. A zero-width line
  never advances, so the original hangs on it. **Quirk:** the count gets one extra line when the
  profile's maximum LCD height (0x45c7c8) is a multiple of 10, i.e. on the 101x80 profiles.
- **drawBox** (0x4233ed): skip = max(scrollLine - startTextY/10, 0) lines. With a title, a blue
  (RGB332 3) bar of height 9 spans the image and the title is centred in white, drawn twice (at x
  and x+1, for bold); the text then starts at y=10. Text starts at max(y + startTextY -
  10*scrollLine, y), lines are 10 pixels apart, black, justified by m_justify (1 centre, 2 right,
  otherwise left), and drawn while `y + ty + 1 < height` (so the title height counts twice). If the
  text is not scrolled to its end (`max(lines + startTextY/10 - (height/10 - 1), 0) != scrollLine`)
  and the cursor flag is set, the 8x4 down arrow at 0x456d70 (1 bit, black on white, opaque) is
  drawn at ((width-6)/2, height-5).
- **drawList** (0x41b950): rows of 11 pixels, at most height/11 of them. When m_current > 3 the
  list scrolls so that the current item is on the fourth row. LEAF (1) items are drawn at x=5 in
  black, BRANCH (0) items at x=0 in RGB332 2, each twice (bold). The current item gets a 12-pixel
  RGB332 0x10 bar across `width` and white text. Each BRANCH row is followed by a black rule at its
  bottom made of the segments [0, w-25], then gaps of 2 and lengths 3, 2, 1, 0, plus one extra
  pixel of spacing.
- Both natives first clear the target with ops slot 10: a white fill of (0, 0, clipW, clipH), the
  clip's size but not its position.

#### Resource format (how natives see the `.exn`)

- Loader 0x406c85:
  - 0x406d1a checks the `NEXE` magic.
  - 0x406d8a reads 16 bytes at offset 4 as 4 x (u16,u16) into `res+0x4f0`. In every game these are
    (0x80,0), (512,8192), (1024,12288), (2048,24576).
  - 0x406eeb reads the u32 N at 0x34, then `u32 offsets[N+1]` at 0x38, then `u8 types[N]`.
  - 0x406fc8 loads the code chunk from `align4(offsets[N])` into RAM and walks the 4CVP blocks.
  - This matches docs/FORMAT.md. The offsets are absolute file offsets.
- Resource reads are **not served from memory**. Every primitive read does fseek and fread on the
  open `.exn` (`G+0x14 -> +4`, 0x403680 and 0x403648). readUTF and readString also malloc and read
  the *whole remaining resource* on every call.
- Type tag **0x81 means compressed**. `readBytes(n)` on such a resource unpacks into an n-byte array
  (n = the unpacked size, supplied by the bytecode). The high nibble of the first packed byte selects
  one of 5 decoders: 1 = 0x431750, 2 = 0x42f560, 3 = 0x42d500, 4 = 0x432180, 5 = 0x432a00. Method 5
  takes a 24-bit size from packed bytes 9..11. **Open:** identify these codecs (the 0x400-byte state
  tables suggest LZ/Huffman variants).
- The other tags (0x0d image, 0x06, ...) mean nothing to the natives. Their interpretation is
  bytecode (Image.TransformBitmapFromResExed etc. are out of my range).
- Endianness is mixed. Scalar and array reads are little-endian. The readUTF length is big-endian.

#### PlayField and sprite rendering (summary of natives/PlayField.c.txt)

- **Coordinate mapping.** Field pixel f is drawn at screen `f - x + view`. The Graphics clip is
  temporarily intersected with `[viewX, viewY, viewW, viewH]` and restored on exit.
- **Gate.** Tiles are drawn only if `background`, `charW` and `charH` are set and `(state & 6) == 6`.
- **Traversal.** Column-major. It runs until the destination *image* edge or the map edge, not the
  view edge, so the clip does the trimming.
- **Cells.** A u8 per cell (nbBits==8), otherwise a u16. Cell 0 is empty and nothing is drawn.
  Negative cells are animated: `animTileIndex[(-v) % (animTileMaxIndex+1)]`. Tile t is drawn from
  sheet index t-1, in rows of `charSet.width / charW` tiles.
- **Tile modes.**
  - `!(state&8)`: opaque blit, mode 1.
  - `state&8` without 0x10: one blit, mode 3, or 4 with `state&0x20`.
  - `state&8|0x10`: the image is tile 2t and the mask is tile 2t+1, drawn mask first. Modes 4 then
    3, or 3 then 4 with 0x20.
  - **Resolved [H]:** the mode argument of `blit` 0x418008 has no effect. 0x418008 calls the
    source's setPaintMode slot (ops+0x30) with it (0x418154) and nothing else reads it; that slot
    is a stub in both vtables (8-bit 0x42c351 calls hook `[0x457f38]` = 0x42d463, `return 1`).
    Transparency comes only from the source image's blit mode (transparent key, palette alpha).
    With WITHMASK the mask tile is simply drawn before the image tile, through the same blitter.
  - The OPACITY bit is inverted from what its name suggests: without it tiles are drawn with
    mode 1, with it (and without WITHMASK) mode 3/4. Both draw the same way.
  - An animated cell reads `animTileIndex[(-v) % (animTileMaxIndex+1)]` without a bounds or null
    check; `background` null, `charW` or `charH` 0 skip the tile pass.
- **Clip.** The view intersection is written into the Graphics object's clip fields (+0x24..+0x30)
  for the duration of the call (so AnimBitmap.draw's sync_clip sees it) and restored on every exit
  except the fatal one.
- **Sprites.**
  - Every draw does an in-place cocktail sort of the linked list by `z`, ascending, so the lowest z
    is drawn first, at the back. **Bug:** a swap relinks only the two sprites (a.next, a.prev,
    b.next, b.prev) and first/last, never the outer neighbours (0x427c2d..0x427c90 and
    0x427d4b..0x427dae). Any swap that is not at both ends of the list corrupts it: e.g. A(z0),
    B(z5), C(z3) leaves A.next = B, B.next = null, so C is no longer drawn (it is still reachable
    backwards through B.prev). No corpus game calls addSprite. **exen-reloaded fixes this**: the
    list is sorted with the same order (ascending z, stable, since the original only swaps on a
    strictly smaller z) and every neighbour is relinked (tests/c/test_playfield.c).
  - Then each visible sprite is drawn at `(viewX-x+s.x, viewY-y+s.y)` with `s.frame` and
    `s.transform`.
  - Dispatch is by the anim's class CRC32: AnimBitmap (0xe7167d52) goes to native 43, AnimFlash
    (0xd414954a) goes to the stub native 62. **Other Animation subclasses are not drawn at all.**
- **AnimBitmap frame rects.**
  - With no listCoords, frames form a grid of `(image.width [/2 if WITHMASK]) / width` columns, and
    frame f sits at `((f%cols)*w, (f/cols)*h)`.
  - With listCoords, there are 4 shorts per frame (x,y,w,h).
  - The WITHMASK mask is the right half of the image, at the same coordinates + width/2.

#### Performance-relevant notes

- Resource reads go to disk: one fseek+fread per `readInt`/`readShort`/`readByte`. readUTF and
  readString read the whole remaining resource for each string. Level loaders that call `readInt`
  in a loop run thousands of tiny CRT reads. This is a load-time cost, not per frame.
- PlayField.draw does one 0x418008 blit call per tile (or 2 with a mask). Each call re-validates and
  re-clips and goes through two vtable calls. It walks to the image edge even when the view is
  smaller, and re-sorts sprites with an O(n²) bubble sort every frame. That is cheap at these sizes
  but worth noting.
- The FX effects use full-image per-pixel loops on 8-bit. On 1-bit they are no-ops.

#### Quirks a reimplementation must keep

- readChar writes 16 bits, and readShort's failure path also writes only 16 bits.
- Lengths are truncated: readBytes, readShorts and readInts to u16; String length from readUTF and
  readString to u8.
- `Resource.init` returns null. `readString` does not advance the position.
- `haveDisplayableCommand` is always true.
- AnimFlash is entirely stubbed. getNbFrames=1, getNbLoops=0, getWidth/getHeight return the fields.
- A divide by zero in getRealFrame when nbFrame == 0 would crash the host (no guard).
- FX shift effects read the offset array without a bounds check.
- PlayField.moveTiles copies one column w times instead of moving a block (see #47). Fixed in
  exen-reloaded.
- PlayField.draw's z sort can drop sprites from the list (see the sprite notes above). Fixed in
  exen-reloaded.
- addSprite/removeSprite(null) and PlayField.draw with an image lacking native data are fatal
  (0x407a13), not catchable exceptions.

### 2.4 Sms, Math, Matrix3D, Vector3D, Debug, java.lang, vm.sys.Runtime, catalog

Decompilations: `natives/math.c.txt`, `vec3d_matrix3d.c.txt`, `string.c.txt`, `sms.c.txt`.

| idx | method (sig) | C addr | semantics | conf |
|---|---|---|---|---|
| 89 | Sms.deleteSms()V | 0x429a18 | no-op, returns 0 | high |
| 90 | Sms.createSms()V (private) | 0x4298c8 | Takes the native buffer from `this.+0x20` (byte[] data, 0x94 bytes) and zeroes it. Writes magic `"EXEN"` at bytes 0..3 and the header bits (see SMS). Writes 8-bit `0x60` and an 11-bit 0. Resets the block cursor fields `+0x24..+0x30`. | med-high |
| 91 | Sms.createSms(B[])V (private) | 0x42993b | For parsing a received SMS: copies 0x88 bytes from the arg array to buf+4, zeroes the magic, sets bitpos=0, resets the cursor | high |
| 92 | Sms.createBlock(B id)V | 0x429a20 | if bitpos<=0x42d: blockStart(+0x24)=pos, blockId(+0x28)=id, len/consumed=0; write id in 8 bits, then skip 11 bits (length placeholder) | high |
| 93 | Sms.readBits(I n)B | 0x429ac0 | returns the next n bits, MSB-first, masked to n (full int in args[0]) | high |
| 94 | Sms.writeBits(B v,I n)V | 0x429b0a | if bitpos<=0x42d: n>7 is clamped to 8; writes `v&0xff` MSB-first. **The value is not masked to n bits**, so high bits OR into the preceding bits. | high |
| 95 | Sms.endBlock()I | 0x429b8d | len = pos-(blockStart+19); writes len as 3 bits (len>>8) + 8 bits at blockStart+8; restores pos; returns len in bits (0 on error) | high |
| 96 | Sms.nextBlock()V | 0x429c43 | reads the 11-bit length at pos, pos += len, resets the cursor | high |
| 97 | Sms.getIdBlock()I | 0x429cb0 | if pos>=0x460 (1120 bits = 140 B) returns -1; else blockStart=pos, id=read 8 bits, returns id | high |
| 98 | Sms.getLengthBlock()I | 0x429d2a | reads 11 bits into `+0x2c`, returns it | high |
| 99 | Sms.skipBits(I n)V | 0x429d86 | if pos+n<=0x460: pos+=n, consumed(+0x30)+=n | high |
| 100 | Sms.getPrice()LString; | 0x429e0a | The 16.16 value at EEPROM `cfg+0x1a8 + 0x1c*m_type`, formatted by 0x422e57 with the fixed-width itoa 0x422bc8 (at most width-1 digits): integer part (width 4, <= 3 digits), '.', a '0' if cents < 10, cents (width 2, so **only the last digit**), ' ', then the 0xff-terminated currency at `cfg+0x168` ("Euro(s)"). Cents = (frac*100)>>16, so the shipped table gives "0.00 ", "0.0 " (0.11), ... | high |
| 110 | Math.sin(I)I | 0x426990 → 0x41c956 | `cos(a-0x200)` | high |
| 111 | Math.cos(I)I | 0x4269ac → 0x41c972 | table lookup, 2048 units/turn, result ±65535 (16.16) | high |
| 112 | Math.getAngle(I x,I y)I | 0x4269c8 → 0x41ca3c | atan2(y,x) in 0..2047 using isqrt and a binary search of the cos table; (0,0) gives 0 | high |
| 113 | Math.setSinusPeriod(I)V | 0x4269eb → 0x41cb68 | period = p only if p>0 | high |
| 114 | Math.getSinusPeriod()I | 0x426a01 | returns period (default 1) | high |
| 115 | Math.getSinOfPeriod(I)I | 0x426a14 → 0x41cb91 | `sin((x<<11)/period)` (C truncating division) | high |
| 116 | Math.getCosOfPeriod(I)I | 0x426a30 → 0x41cbb2 | `cos((x<<11)/period)` | high |
| 117 | Math.getCosPeriodPrecise(I)I | 0x426a4c → 0x41cbd3 | Linear interpolation between cos(i) and cos(i+1) with f/255 weights; see math.c.txt | high |
| 118 | Math.getSinPeriodPrecise(I)I | 0x426a68 → 0x41cc88 | same with sin | high |
| 119 | Math.abs(I)I | 0x426a84 | x<0 ? -x : x (INT_MIN stays INT_MIN) | high |
| 120 | Math.setRandSeed(I)V | 0x426ab4 → 0x41cd3d | state = aux = seed | high |
| 121 | Math.random()I | 0x426aca → 0x41cd5d | custom PRNG, see below | high |
| 122 | Math.sqrt(I)I | 0x426add → 0x41ce03 | integer floor sqrt (bitwise); x<2 (signed, so negatives too) returns x | high |
| 123 | Matrix3D.copyFrom(I[])V (private) | 0x426b20 | copies 16 ints into `this.m` (field +0x18 = int[16]); null → log + fatal interrupt 1 | high |
| 124 | Matrix3D.rotX(I a)V | 0x426b89 → 0x41d25b | `this.m` = rotX matrix (overwrites, does not multiply) | high |
| 125 | Matrix3D.rotY(I)V | 0x426be3 → 0x41d2c4 | overwrite with rotY | high |
| 126 | Matrix3D.rotZ(I)V | 0x426c3d → 0x41d32e | overwrite with rotZ | high |
| 127 | Matrix3D.multiply(LMatrix3D;)V | 0x426c97 → 0x41d396 | this = this × other, 4×4, terms `(a>>8)*(b>>8)` | high |
| 128 | Matrix3D.multiply(LVector3D in,LVector3D out)V | 0x426d7b → 0x41d17b | out = in × M (row vector, upper 3×3 only, **translation row ignored**) | high |
| 129 | Vector3D.squareLength()I | 0x42a020 → 0x41ceb4 | Σ(c>>8)² (fields x,y,z at +0x18/+0x1c/+0x20) | high |
| 130 | Vector3D.length()I | 0x42a074 → 0x41cefa | isqrt(squareLength)<<8 | high |
| 131 | Vector3D.normalise()I | 0x42a0c8 → 0x41cf17 | len<=0 → returns -1, vector unchanged; len<0x10000 → c *= 0x10000/len (integer factor); else c /= (len>>16); returns 1 | high |
| 132 | Vector3D.sum(LVector3D;)V | 0x42a132 | this += o | high |
| 133 | Vector3D.minus(LVector3D;)V | 0x42a1ad | this -= o | high |
| 134 | Vector3D.dot(LVector3D;)I | 0x42a228 → 0x41d041 | Σ(a>>8)(b>>8) | high |
| 135 | Vector3D.crossProduct(LVector3D;)V | 0x42a28a → 0x41d090 | this = this × o (each product (a>>8)(b>>8)) | high |
| 136 | Vector3D.scale(I s)V | 0x42a305 → 0x41d12b | c = (c>>8)*(s>>8) | high |
| 147 | Debug.DisplayText(C[])V | 0x429f40 | copies the chars to a malloc'd C string and writes it to the simulator ExEn log (0x434760 → 0x434771) | high |
| 148 | Debug.WaitForKey()V | 0x429fb2 | no-op | high |
| 149 | Debug.DisplayMemoryBlocks()V | 0x429fba | no-op | high |
| 150 | Debug.DisplayInt(I)V | 0x429fc2 | decimal itoa (0x422d10, 16-byte buf) → log; -2 → fatal interrupt | high |
| 151 | Object.getClass()LClass; | 0x42a660 | `0x411710(typecode obj+0xc, class hash, classptr or 0 for arrays)` returns a Class object | med |
| 152 | Object.hashCode()I | 0x42a6cc | returns the object pointer itself (args[0] unchanged) | high |
| 153 | Object.clone()LObject; | 0x42a718 | Objects: Cloneable (crc 0x2b927978) required, else CloneNotSupportedException; shallow copy via 0x411a0f. Arrays: copy via 0x411add. A null result throws OutOfMemoryError. | high |
| 154 | Object.wait(J)V | 0x42a6df | negative timeout → IllegalArgumentException; otherwise **does nothing** (no waiting) | high |
| 155 | Class.forName(LString;)LClass; | 0x42a3be → 0x411795 | null → IllegalArgumentException. Name looked up by crc32. Array descriptors "[..L..;"/"[I" etc. go through the type table. Not found → ClassNotFoundException (0xf2985715). | med-high |
| 156 | Class.newInstance()LObject; | 0x42a360 | If the Class describes a plain object type (0x99), allocates an instance of it. **No constructor is run.** Otherwise InstantiationException. | high |
| 157 | Class.getName()LString; | 0x42a3da | New String: plain class → its name; arrays → `'['`×(dims+1) + type letter (+ class name for L) | med |
| 158 | String.length()I | 0x42ac79 | `pstr ? pstr->len : 0` | high |
| 159 | String.toCharArray()C[] | 0x42a7b0 | new char[len], byte copy | high |
| 160 | String.getBytes()B[] | 0x42a99f | new byte[len], identity byte copy (0x4116fd) | high |
| 161 | String.compareTo(LString;)I | 0x42a85e | Returns -1/0/1, unsigned byte compare. **Length tiebreak is inverted**: if one string is a prefix of the other, the longer `this` gives -1. A null arg throws NPE; a null `this.pstr` gives -1. | high |
| 162 | String.append(LString;)V (private init) | 0x42aa54 | Constructor helper. If the source has no backing array (literal), shares its pstr pointer; else clones the array. Null → NPE. | high |
| 163 | String.append(C[])V (private) | 0x42aafc | clones the char[] as backing (len 0 → stays empty); null → NPE | high |
| 164 | String.append(B[])V (private) | 0x42ab8c | new char[len] from bytes (identity 0x4116f5) | high |
| 165 | String.valueOf(I)LString; (static) | 0x42acb6 | decimal, '-' prefix (itoa 0x411490; INT_MIN gives garbage digits) | high |
| 166 | StringBuffer.initStringBuffer(I)V (private) | 0x42aeef | n<=0 → NegativeArraySizeException; else allocates char[n] storage | high |
| 167 | StringBuffer.length()I | 0x42b17d | pstr->len (no null check) | high |
| 168 | StringBuffer.capacity()I | 0x42ae20 | storage.blocksize-0x14 | high |
| 169 | StringBuffer.append(LString;)LStringBuffer; | 0x42b056 | Appends bytes. **A null arg is silently ignored** (no "null"). The returned word is unchanged `this`. | high |
| 170 | StringBuffer.append(C[])… | 0x42b0f2 | appends array bytes; null → NPE | high |
| 171 | StringBuffer.append(C)… | 0x42b19f | append 1 byte | high |
| 172 | StringBuffer.append(I)… | 0x42b1ed | reserves 11, appends itoa | high |
| 173 | StringBuffer.append(J)… | 0x42b244 | Reserves **only 11** chars, then ltoa via 0x411552 (args[1]=lo, args[2]=hi). A long with more than 11 chars can overrun. | high |
| 174 | StringBuffer.toString()LString; | 0x42ae4e | new String with a **copy** of the storage array | high |
| 175 | Runtime.gc()V | 0x42b2a0 → 0x40a30b | Mark and sweep over the heap object list at `[0x51f90c]`. Roots are objects with the +0xe/+0xf counts nonzero (plus the stack). Sweep frees objects whose u16 +0xc mark is 0. | med |
| 176 | Runtime.createTempClass(B[],I)Z | 0x42b2ad | null → NPE (0), n>len → AIOOBE (0); else `0x410060()` which is a stub returning 0, so always false | high |
| 177 | Runtime.getTickCount()I | 0x42b338 → 0x435d3f | `GetTickCount() - base`, in ms. The base `[0x50498c]` is set by 0x43f5aa from the gamelet start/init routine 0x438c06. **Win32 GetTickCount** (10-16 ms granularity). | high |
| 178 | Catalog.doesGameExist(LGameProperty;)Z | 0x4240a0 | `0x423bd0(gp.fileId u16 @+0x34)`: checks the flash/catalog table in config `[0x45fe8c]` (CRC compare). Port: no catalog, false | med |
| 179 | Catalog.launchGameIfPresent(LGameProperty;)Z | 0x4240e1 | Writes the launch request to `[[0x45ff3c]+0x34]` (+0x1c=fileId, state=5). Then stops the timer (0x435dde) and melody (EXManufMelodyStop), sets the VM exit flag `[[0x45ff3c]+0x24]=1` and returns 1. The host loads the new gamelet after the VM exits. Returns 0 if absent. Port: no catalog, false | med |
| 180 | Catalog.isUserRegistred()Z | 0x424178 → 0x423b7f | tests registration fields in config `[0x45fe8c]` | med |
| 181 | Catalog.downloadGame(LGameProperty;)V | 0x424194 | Copies name (≤16 B) / fields into the launch struct and stops. `0x423a00(3)` checks connectivity (MCC/MNC via EXManufGetMccMnc). On failure: state 0, exit code 3. Otherwise state=registered?2:6 and the exit flag is set. The actual EXManufConnectionOpen/DownloadFile happens in the host after the VM exits. Port: does nothing (logged once), no exit | med-low |
| 182 | Catalog.doEditBox(LString;)V | 0x4242b2 | Title (≤14 chars, 0xff padded) → EXManufStringRegister (0x406674). Sets `[[0x45ff3c]+0x24]+0xc=0xb`, then 0x403d8a → **EXManufUIEditBox(titleID, maxSize=6)**, which runs a modal PeekMessage loop (0x4364c3) into a 7-byte buffer. Port: no-op, logged once | med |
| 183 | Catalog.atoi(LString;)I | 0x424375 → 0x422ad2 | decimal parse of pstr: skip spaces, optional '-', digits; -1 when there are no digits or the length runs out (also after leading spaces or a lone '-') | high |
| 184 | GameProperty.joke()V | 0x4243c0 | no-op | high |

#### Math tables / PRNG — high confidence

- **Cos table**: `u16 T[513]` at `0x4567f8..0x456bf9`, with T[0]=65535, T[256]=46336, T[512]=0. It follows no simple rounding formula (closest is 65535·cos(i·π/1024) with mixed rounding), so **copy it verbatim** (1026 bytes at file offset 0x567f8). A circle is 2048 units and results are 16.16 (±65535).
- `cos(a)`: a&=0x7ff; [0,512): T[a]; [512,1024): -T[1024-a]; [1024,1536): -T[a-1024]; [1536,2048): T[2048-a]. `sin(a)=cos(a-512)`.
- `random()` (0x41cd5d), state s0,s1 (32-bit):
  ```
  v = s0; n = s1 & 31;
  repeat n: v = ((int32)v >> 1) | ((v & 1) << 31);   // arithmetic shift: not a true rotate when v<0
  s1 = s0 ^ 0x05960395;
  s0 = v + s1 + 0x7921;      // wraps mod 2^32
  return (int32)s0;
  ```
  The initial state is s0=s1=0xc800. setRandSeed(x) sets s0=s1=x.
- `sqrt` (0x41ce03): restoring bitwise isqrt on unsigned values. k=floor(log4 x); r=1<<k; sq=4^k; then for each lower bit, b>>=1 and t=sq+((2r+b)<<k'). If t<=x then r+=b and sq=t.
- getAngle lookup `0x41c9e9`: binary search on T over [0,512] (lo=0, hi=512): while hi-lo>1, mid=(lo+hi)>>1, and `T[mid]>v ? lo=mid : hi=mid`. Returns lo.

#### 3D fixed point — high confidence

All values are 16.16. The Matrix3D field +0x18 is an int[16], row-major `m[r*4+c]`. Every product is `(a>>8)*(b>>8)`, which loses the low 8 bits.
- rotX: m0=1, m5=c, m6=s, m9=-s, m10=c, m15=1.
- rotY: m0=c, m2=-s, m8=s, m10=c, m5=1, m15=1.
- rotZ: m0=c, m1=s, m4=-s, m5=c, m10=1, m15=1. All other entries are 0.

#### String representation — high confidence

- Characters are **8-bit**. A `char` is stored in one byte, and there is no Unicode conversion (the conversion helpers 0x4116f5/0x4116fd are identity).
- A String or StringBuffer object holds:
  - `+0x18`: a pointer to a **pstring** (u16 len, then the bytes). The field is stored as a 64-bit value, with the high dword at `+0x1c`, filled by `cdq`.
  - `+0x20`: the backing char[] (or 0).
- A literal String points straight into the class constant data (the same u16-len pstrings) and has no backing array. A dynamically built String points at `array+0x12`, so the array's count field doubles as the length.
- StringBuffer growth (`0x42af2f`) reallocates to exactly `len+need`, copies and frees the old array. There is no doubling, so building a string by repeated appends is O(n²). The maximum is 0x4000 chars, beyond which StringIndexOutOfBoundsException is thrown.

#### SMS — med-high

- The native buffer is a byte[0x94] in `Sms.+0x20`:
  - `[0..3]`: magic `"EXEN"`
  - `[4..0x8f]`: 140-byte payload, bit-addressed **MSB-first, big-endian** (writer `0x413071`, reader `0x412ea0`)
  - `+0x90`: u32 bit position
  - Write limit: pos<=0x42d (1069). Read limit: 0x460 (1120 bits).
- Header written by createSms (0x4215fa), in bit widths: 8 cfg[+5], 8 cfg[+7], 16 gamelet id (`[[0x45ff3c]+0x14]+4 → +0xc`, or 0), 4×8 cfg[+0x1e..+0x21], 12 MCC (nibble-swapped), 8 MNC (nibble-swapped), 8 EXManufGetLanguageId, 16 cfg[+0x164]. Then pos += 20 bits reserved, then 8 bits `0x60`, then 11 bits 0.
- `cfg = [0x45fe8c]`, the simulator's network/eeprom config.
- A block is `id:8, len:11 (bits), payload`.
- Language ids (0x4341cc): English 1, Dutch 5, French 3, German / German D2 0, Italian 2, Portuguese 8, Spanish 4, default 1.
- **The 0x94-byte buffer overruns its array.** Sms's constructor allocates `java_exsms` as a
  byte[142], but the natives use 0x94 = 148 bytes of it: the end of the payload and the u32 bit
  position at +0x90 lie past the array, in the following heap bytes. The port keeps them in the
  padding of the array's heap block (a byte[142] block is 160 bytes in our heap) and refuses a
  block that is smaller.
- The header's cfg bytes are passed sign-extended, and putbits does not mask its value: a byte
  >= 0x80 written at a position that is not byte-aligned would OR ones into the preceding bits (the
  header itself is aligned). getbits' 5-byte case (0x412fab) combines its two words with `<< 2`
  instead of the right shift; the port keeps it.
- `cfg` is the EEPROM image itself (flash/eeprom.dat): currency at +0x168, price table at +0x1a8
  (28-byte entries, the value first; 0x423b1d tests a 0x15-byte number string 0x18 bytes before
  it), registration bytes +0x1e..+0x21, nickname +0x66 (16 bytes), home network MCC +0x160 and
  MNC +0x162.
- **Port:** MCC/MNC 0 (no SIM) and language 1 go into the header. Nothing goes over a network:
  the core's mock SMS server answers instead. The wire format, the receive path and the games'
  protocol are in [SMS.md](SMS.md).
- **Catalog** (178-183) in the port: doesGameExist and launchGameIfPresent return false;
  downloadGame does nothing (the original records a launch or download request and exits the VM so
  that the host can act on it); isUserRegistred tests cfg+0x1e..+0x21 (true with the shipped
  eeprom.dat); atoi is 0x422ad2.

#### Win32-specific / notable

- Runtime.getTickCount uses GetTickCount (coarse resolution). The simulator log goes to `simulator.log`.
- doEditBox and setNickName block in their own PeekMessage modal loop (the port does not block).
- Behaviour that must be reproduced, not "fixed", for fidelity:
  - Object.wait does not wait.
  - String.compareTo's length tiebreak is inverted.
  - Class.newInstance skips the constructor.

#### Open questions from the Graphics/Image analysis

1. Codecs 1-4 (decode fns 0x4318f6, 0x42f724, 0x42d960, 0x43228b) need decompiling. Type 3 occurs
   in the flash corpus, alongside type 5, so it is needed first. A sweep of real games would show
   whether 1, 2 and 4 are used.
2. Exact orientation of drawImage transforms 4 and 5 (rot90 vs rot270). Verify by running.
3. The 4bpp-source blitters (0x418f00 family) are only summarised. Modes 0x10/0x20 on a 4bpp source
   fall back to the 8bpp blitters, which looks like a bug. Confirm what games do.
4. Graphics bytecode (setPaintMode, getCharWidth, the drawImage wrappers) should be checked to see
   whether it compensates for anything in the natives, for example the unused args[6].
5. What consumes `normalVideo` (ctx20+0x24) in the display update (parent/EXManuf scope).

---

## 3. The EXManuf* porting layer

The ExEn core (roughly `0x401000..0x407000`: memory, files, events, UI glue) calls a manufacturer
porting layer. In the simulator that layer is roughly `0x433380..0x436700` plus host helpers above it.
Every function logs through `0x438146` ("Manufacturer: ..." in `simulator.log` and the log window,
when `LogManuf=1`). Each log line also does a `SendMessage`/`UpdateWindow` on the log window, which is
not free; see §4.5.

The table gives the signature as reconstructed (cdecl, int return; 1 = OK, -1 = error, unless noted),
the core-side caller and the simulator behaviour.

| Function | Addr | Called from | Simulator behaviour | conf |
|---|---|---|---|---|
| `EXManufConnectionOpen()` | 0x433380 | 0x4012e2 | log only, returns OK | H |
| `EXManufConnectionClose()` | 0x433397 | 0x40202c | log only | H |
| `EXManufDownloadFile(name,...)` | 0x4333ae | 0x401864 (catalog download) | Maps the remote name via `[Download]` in simulator.ini (`NNNNN=file.exn`) and copies `SERVER_PATH\file` to `FLASH_PATH\downloadN.exn` (CopyFileA). Waits with a **busy-wait** `0x43373d` (spins on GetTickCount for the given ms) to fake transfer time, then posts ExEn events 0x5xx back through the message pump. | M |
| `EXManufEepromRead(u8 *buf, int size)` | 0x433770 | 0x402d43 (size 0x800) | `fread` of `FLASH_PATH\EEPROM.DAT` ("rb") into buf | H |
| `EXManufEepromWrite(u8 *buf, int size)` | 0x4337f2 | 0x402d7f (size 0x800) | `fwrite` of the whole 2048 bytes ("wb"). It is called after **every** config change (contexts, settings, gamelet table): 0x402d7f has 12 call sites. | H |
| `EXManufFileOpen(int fileId, int mode, int size)` | 0x433880 | 0x403608 | fileId 1 → `catalog.exn`, 2..5 → `download1..4.exn`, 1001 → `embedded1.exn`, 1002 → `embedded2.exn` (under FLASH_PATH). mode 0 = "rb+", 1 = "wb+". **Reads the whole file into a malloc'd buffer** (`0x460f48`, size `0x460f4c`). Warns if the real size ≠ `size`. Only one file is open at a time (globals `0x460f44..50`). | H |
| `EXManufFileClose(id)` | 0x433abc | 0x403627/0x403726 | free buffer + fclose | H |
| `EXManufFileWrite(id, off, buf, size)` | 0x433b22 | 0x403664 | fwrite | M |
| `EXManufFileRead(id, off, buf, size)` | 0x433b8f | 0x403648 | fread | M |
| `EXManufFileSeek(id, off)` | 0x433bf4 | 0x403680 | fseek | M |
| `EXManufFileGetPtr(id, off)` | 0x433c50 | 0x403698 | returns `buffer + off`; the game image is executed in place from this buffer | M |
| `EXManufFlashGetPtr(id)` | 0x433ca0 | (none: unused) | old variant of FileOpen + GetPtr | M |
| `EXManufGetGamelet(id)` / `EXManufReleaseGamelet()` | 0x433e20 / 0x434075 | (none: unused) | | M |
| `EXManufVibratingDeviceRun(int ms)` | 0x434189 | 0x4065e0 | Clamps 0..10000. Sets `0x4a20bc = GetTickCount()+ms`. The idle hook `0x438840` then **shakes the main window** (sin/cos offsets of the StretchDIBits origin, `0x4a20c0/c4`) and repaints until the time passes. | H |
| `EXManufGetLanguageId()` | 0x4341cc | 0x403d80 | Reads `[Language.current]`. The name maps to an id: English 1, Dutch 5, French 3, German 0, German D2 0, Italian 2, Portuguese 8, Spanish 4, default 1. | H |
| `EXManufDisplayUpdate(EXDisplay *d)` (no log) | 0x4342f0 | 0x4024ab (from screenUpdate) | See §4.3. Converts the 8bpp/1bpp/2bpp framebuffer to XRGB32 and **repaints synchronously**. | H |
| `EXManufMelodyPlay(u8 *mel, int size)` | 0x434790 | 0x406601 | Despite the log text "EMPTY FUNCTION", it **does play**, via `0x43b192` (MIDI, §3.2) | H |
| `EXManufMelodyStop()` | 0x4347bb | 0x406628 | `0x43b259`: KillTimer(10), all-notes-off (`0xff` short msg), midiOutClose | H |
| `EXManufSetVolume(int v)` | 0x4347d2 | 0x403c30 (at VM init, from EEPROM `cfg+0x15c`) | `0x43ac4e`: midiOutSetVolume, with v replicated into the L/R bytes. The log also says "EMPTY". | H |
| `EXManufMalloc(int size)` / `EXManufFree(p)` | 0x434866 / 0x434959 | 0x404167 / 0x404178 | CRT malloc. Records it in a 0x7fff-entry table `[0x460f54]`. **Each call sums all 32767 entries** and then refreshes the memory-statistics dialog (`0x43cfbf`) for the peak-usage display. | H |
| `memcpy/memset/memcmp/strlen`-style helpers | 0x434a23 / 0x434a4e / 0x434a73 / 0x434a9e | 0x4041ca etc. | thin CRT wrappers (EXManufMemCopy, ...; names not logged) | M |
| `EXManufGetMccMnc(...)` | 0x434ad0 | 0x403eda | From `[Simcard.NNN]` MCC/MNC strings (hex nibbles via 0x434c05) | M |
| `EXManufGetSIMID(u8 *imsi)` | 0x434c8a | 0x403eef | Builds the IMSI from `[Simcard.current] IMSI1..4` | M |
| `EXManufSmsSend(...)` | 0x434efe | 0x406514 | `0x434f76`: with `[SmsServerConfig] Activate=0`, reports success without a network. Otherwise TCP connect to `IP:Port` (default 192.168.0.46:5555), send 17 bytes (`nb_phone`, then `Port_bind`), read a 1-byte ack (0 = ok), send the 140-byte message ("EXEN" + payload). Posts event 0x101 with 1 (sent) or 2 (failed). Details in [SMS.md](SMS.md). | H |
| SMS receive server | 0x4351f8, thread 0x435396 | WinMain init 0x438c6a | TCP listen on `Port_bind` (default 5556, incremented until bind succeeds). The thread does **blocking** accept/recv (no CPU burn) and hands 0x8c bytes to `0x43544d`, which queues an "SMS received" event polled by `0x438840` (`0x434eaf/0x434e50`) and delivered as ExEn event 0x100 (`onSmsReceived`). | M |
| `EXManufSocketCreate/Bind/Close/Sendto/Recvfrom/Ntohs/Htons` | 0x4354c0, 0x4354fa, 0x435658, 0x435697, 0x4356d1, 0x4357a9, 0x4357cb | (none: unused by this framework) | Winsock UDP wrappers. Bind starts a recvfrom thread (0x435582). | M |
| periodic-callback thread `(fn, period)` | 0x4357ed, thread 0x435861 | (none: **dead code**) | `while(1){ if (timeGetTime() >= next) { next += period; fn(); } }` with **no Sleep**. It would pin a core, but nothing references it (no calls, no data pointers). | H |
| `EXManufStringRegister(char *s, int size, int type)` | 0x4358d0 | 0x406674, 0x4377ea | Type 0 = EXEN_STRING_ASCII, 1 = GSM, others are rejected. Appends `{ptr,size,type,id,next}` to a linked list `[0x4a1044]`; returns a new id. The language file `language\*.ini` (`EXEN_STRING_NN`, loaded at 0x4377ea) is registered this way, along with game strings. | M |
| `EXManufStringUnregisterAll()` / `Unregister(id)` | 0x435a04 / 0x435a89 | | list free | M |
| `EXManufStringGetPtr(id)` / `GetLength(id)` | 0x435b99 / 0x435c7b | 0x4067fc / 0x406819 | list walk; the strings are 0xff-terminated | M |
| `EXManufSetTimerTick(unsigned ms)` | 0x435d00 | 0x406851 (from Gamelet.startTimer and core init) | **ms**, clamped to [10, 60000] → `0x43f5ba`: `SetTimer(hwnd, 1, ms, NULL)` | H |
| (stop timer) | 0x435dde | 0x406868 (Gamelet.stopTimer) | `KillTimer(hwnd, 1)` | H |
| (timer tick count) | 0x435d3f | 0x406872 (Gamelet.getTimerTickCount, Runtime.getTickCount) | `GetTickCount() - [0x50498c]`: **milliseconds** since the gamelet was (re)started (base set by `0x43f5aa` from init `0x438c06`). Despite the name it is not a tick count. GetTickCount has 10 to 16 ms granularity. | H |
| `EXManufGetDateTime(int *date, int *time)` | 0x435d65 | 0x403f00/0x403f2b | `GetSystemTime` (**UTC**): `date = Y*10000+M*100+D`, `time = h*10000+m*100+s` | H |
| `EXManufUIDialog(titleID, textID)` | 0x435df0 | 0x4025f6, 0x402681 | Modal native dialog (strings by id) drawn by the simulator. **Busy PeekMessage loop**, see §4.4. | H |
| `EXManufUIMenu(titleID, int *ids, int n, int *sel)` | 0x436251 | 0x4024e5 (the ExEn launcher menu, 0x404ca4/0x4045c7...) | Modal native menu. Items are allocated at 0x100c bytes each. The loop runs until Enter/Esc (keys handled in WndProc state 5, `0x439c4b`). Returns 1, or -2 if the key was 8 (Back) or 27 (Esc). **Busy PeekMessage loop.** | H |
| `EXManufUIEditBox(titleID, maxSize, buf)` | 0x4364c3 | 0x403d8a (Gamelet.setNickName, Catalog.doEditBox) | Modal text entry. **Busy PeekMessage loop.** | H |
| backlight `(mode)` | 0x4340d0 | 0x403c30 (init, from EEPROM `cfg+0x15d`), WndProc keydown | Mode 2 = always on, 1 = auto (on at key press, dims after 30 s), 0 = off. Brightness is a float `0x45c7c0` in [0.7, 1.0], stepped by 0.05 per idle call, each step forcing a repaint. | H |

### 3.1 EEPROM [H]

The EEPROM is a 2048-byte image of the simulator's config struct, kept in RAM at `[0x45fe8c]` and
mirrored to `flash\EEPROM.DAT`. It is read at start (`0x402d43`: EXManufEepromRead, then the
deserialiser `0x402e00`) and written after every change (`0x402d7f`, 12 call sites: serialise
with `0x402e4d`, then EXManufEepromWrite of all 2048 bytes). The serialiser only scrambles bytes
4..0x7ff (`0x414d53`, key from `0x414da0`) when the first u32 is nonzero; the shipped image has
0 there, so **the file is the struct verbatim**. Re-read from the disassembly in 2026-11 (issue
#17); the table code was traced through `0x415580`..`0x415899`, the File/Open install path
`0x417009`..`0x4173d4` and the launch path `0x403b3e`.

Config area (`cfg` = the image):

| Offset | Contents (shipped value) |
|---|---|
| `+0x00` u32 | serialisation flag (0 = plain) |
| `+0x04..+0x07` | version bytes for getVersionInfo (`00 85 00 80`) |
| `+0x08` | phone number string, 0xff-terminated |
| `+0x1e..+0x21` | registration bytes (isUserRegistred) |
| `+0x25`, `+0x46` | operator strings ("Joper3", "bopr3"; +0x24 holds 0x0a) |
| `+0x66` | nickname, 16 bytes ("Xcell") |
| `+0x78`..`+0x7f` | small values (0x35, 0x0c, 0x05) |
| `+0x80` | "Begin News" |
| `+0x14c` | "Fusio" |
| `+0x15c` / `+0x15d` / `+0x15e` | sound, backlight mode, vibrator |
| `+0x160` / `+0x162` | home network MCC / MNC |
| `+0x164` u16 | SMS header field |
| `+0x168` | currency ("Euro(s)"), 0xff-terminated |
| `+0x190` | SMS price table: 28-byte entries, a 0x15-byte number string, the 16.16 price at +0x18 |
| `+0x21c` u16 | number of records in the gamelet table |
| `+0x21e` u16 | cleared when a gamelet is removed (`0x41585a`) |
| `+0x224` | the gamelet table |

**Gamelet table.** Records follow each other from `+0x224`; each starts with a 0x1c-byte header:

| Record offset | Contents (shipped CrazyCobra record at 0x224) |
|---|---|
| `+0x00` u32 | 0 |
| `+0x04` u32 | `[download+4]`, apparently the file size (0x18760) |
| `+0x08` u32 | record size: 0x1c, plus 0x244 when the gamelet has an id (0x260) |
| `+0x0c` u32 | gamelet id, the signed 16-bit download id; -1 = no gamelet (0x80) |
| `+0x10` u32 | 0 |
| `+0x14` u32 | 2 (`0x403be9(rec, 0)`), 1 for the other install path |
| `+0x18` u32 | 1; the remover `0x4157e1` drops records with an id and a nonzero `+0x18` |
| `+0x24` u32 | `0x423c1f()`: a hash of the 30-byte time string from `0x403eef` |
| `+0x28` | gamelet name, 0xff-terminated ("CrazyCobra") |
| `+0x3c` | **the 300-byte context** (`saveCtx` / `loadCtx`) |
| `+0x168` | passed to the record's save callback (vtable `+0x1c`) when flag bit 0 is set |
| `+0x1f0` u16 | flags; bit 0 = context to flush (`0x414e31`) |
| `+0x1fc` | description ("This gamelet has been downloaded via FileOpen from the simulator.") |

`0x415668(id)` finds a record by id; `0x4156c9` installs one (copying over an existing record with
the same id, else appending and incrementing the count); `0x415742` removes one and moves the rest
down. A game opened with File/Open gets a freshly built record (body zeroed, so **the context
starts empty and a reinstall clears it**), is installed, and becomes the current record:
`[[0x45ff3c]+0x14]+4` points straight into the image (`0x4173c2`; the launcher sets it the same
way through `0x415668` at `0x403b4c`). Where the id of a File/Open install comes from was not
traced to the end (the download session, `[[0x45ff3c]+0x18]+0x14`, `0x401933`); the shipped
record has 0x80, which is also the u32 at `.exn +0x04` in every known game.

- `Gamelet.saveCtx(byte[])` (`0x425156` -> `0x4153f8`) copies **min(len, 300)** bytes into the
  current record at `+0x3c`; `loadCtx` (`0x41547b`) copies them back. Neither does anything if
  the current record's id is -1. The flush to EEPROM.DAT happens later, through the record's
  callbacks.
- **Per-game persistent storage is therefore 300 bytes**, not 2048.

**Port.** The core keeps the image (`exen_eeprom`) and uses the same layout: saveCtx/loadCtx
find the current game's record by id (the signed u16 at `.exn +0x04`, 0x80 in practice) and use
its `+0x3c`. Deviation: with no record (a blank image), loadCtx leaves the array alone, as the
original, but saveCtx appends a record first, so that games keep their saves without a
File/Open step. `exen_eeprom_reset(defaults)` builds a fresh image the way File/Open does: the
config area `0..0x21b` from the simulator's `flash/eeprom.dat`, and a table holding one record for
the game (size 0x260, id, name = database title, `+0x04` = file size, `+0x24` = file CRC-32 instead
of a time hash, empty context). The frontends persist the image per game and write it on
`ExenHost.eeprom_changed` (after every saveCtx) and on exit.

### 3.2 Melody playback (`0x43b192`, timer proc `0x43ad57`) [H]

Melodies are byte pairs `(a, b)` played through `midiOutShortMsg` on the default MIDI device. Playback
runs from a **periodic** Win32 timer, `SetTimer(hwnd, 10, ms, 0x43ad57)`. Each firing handles one event
and usually re-arms the timer with the next period; a firing that does not re-arm keeps the old period.
Re-read from the disassembly (2026-11, issue #16); `docs/re/natives/melody.c.txt` has the pseudocode.

- Start (`0x43b192`, via EXManufMelodyPlay `0x434790`): midiOutOpen(device 0) every time (the
  `need_open` flag `0x45cab0` is never cleared), `mel_size` = size, event count 0, device volume
  `0x80808080` (both channels at 0x80/0xff), `prev = 0xffff`, then `SetTimer(..., 0, ...)`: elapse 0 is
  raised to USER_TIMER_MINIMUM, so the first event comes **10 ms** after playMelody. A play while a
  melody runs simply restarts (the note sounding on the old handle is never turned off). The channel
  byte `0x4c6596` is a global that start does not reset.
- Each firing, first: if `prev >= 1`, `cur_dur = dur[mel_ptr[1] % 14]` (the pair about to be handled)
  and **note-off** `0x80|ch, prev, 0`. So a note lasts exactly until the next event: notes are legato,
  there is no gap, and the player is **monophonic** (one note at a time, on one channel).
- Then `if (++ev_count >= mel_size) return;` and `prev = a`.
- `0 < a <= 0x7f`: **note-on** `0x90|ch, a, 0x40` (velocity fixed at 64), advance, and re-arm with
  `(1875 / tempo) * cur_dur` ms. `tempo` `[0x45caac]` = 120 and **nothing writes it** (its only
  reference is this read), so the unit is `1875 / 120 = 15` ms (integer division: 125 bpm instead of
  the nominal 120).
- `dur = {0xc0,0x80,0x60,0x40,0x30,0x20,0x18,0x10,0x0c,8,6,4,3,2}` in units of 1/32 beat: index
  1, 3, 5, 7, 9, 11, 13 = whole, half, quarter, 1/8, 1/16, 1/32, 1/64; the even indices are the
  dotted values. At 15 ms: whole 1920 ms, quarter 480, 1/8 240, 1/16 120, 1/64 30.
- `a == 0`: end. If `size > 2` the player calls melody_stop (`0x43b259`); with a 2-byte melody it just
  returns (silence). There is **no loop marker**: games loop music by calling playMelody again.
- `a >= 0x80`: **command**: `cur_dur = 0`, then by `a - 0x80` (jump table `0x43b17a`):
  - `0`: channel = b & 0xf
  - `1`: program change. b == 0: channel = 9 (GM drums) and `0xC9` (program 0). 1..0x80: `0xC0|ch, b`.
    Larger b is ignored.
  - `2`: "rest": `cur_dur = dur[b % 14]`, but the pair is **not advanced past and the timer is not
    re-armed**, so every later firing re-reads the same 0x82 pair until the event count runs out. In
    effect 0x82 ends the melody silently (a simulator bug; no game in the corpus uses it).
  - `3`: `SetTimer(1875 / b)`. Always overridden by the re-arm of the next note, or moot because the
    melody ends, so it has no audible effect. b == 0 divides by zero.
  - `4`: volume b (`0x43ac4e`): if both channel high bytes are equal, both become b (`b * 0x101`);
    otherwise only the channel that was nonzero gets b and the other becomes 0.
  - `5`: balance (midiOutGetVolume/SetVolume on the high bytes): b < 0x80 ORs the right level into the
    left and zeroes the right; b > 0x80 the opposite; b == 0x80 copies left to right if right is 0,
    else ORs right into left.
  - `0x86`..`0xff`: ignored.
  - Except for `2`, the next pair is then handled at once (a recursive call into the timer proc, which
    runs the note-off/count steps again; the note-off is for the command byte, a no-op).
- The event counter is compared with `size`, the **byte length** of the array (`Gamelet.playMelody`
  `0x425252` passes the u16 array length), while every event consumes 2 bytes. Without a 0 terminator the
  player reads up to `size/2 - 1` pairs past the array. Faithful ports should stop at the end of the
  array.
- Stop (`0x43b259`, EXManufMelodyStop): `ev_count = size`, KillTimer(10), short message `0xff`
  (system reset: programs back to 0), midiOutClose.
- `Gamelet.playMelody` (`0x425252`) only calls EXManufMelodyPlay when EEPROM `cfg+0x15c` (sound) is
  nonzero. The shipped eeprom.dat has 0 there, so **melodies are silent by default** in the simulator.
  EXManufSetVolume (`0x4347d2`, at VM init from the same byte) only sets the MIDI device volume, and
  every melody start resets it to 0x80, so the byte acts as an on/off switch.
- The timer is a USER timer, so periods are rounded up to the system tick (10 to 15.6 ms): on a
  15.6 ms tick a 240 ms note lasts 250 ms. This depends on the host and is not part of the format.

**Corpus** (33 of 47 games call playMelody; 79 distinct melodies captured with `exen-run --melodies`):
no game uses a command byte; notes span MIDI 28..74 (E1..D5); duration indices 1..9, mostly 7 and 8
(1/8 and dotted 1/16). Most melodies have no terminator; BombJack's come from type-0x04 resources whose
4-byte padding supplies `00 00` pairs. Melodies double as sound effects: short 1- to 4-note jingles are
common, and Arthur & Les Pirates, Flynn and XMasTales call playMelody over 200 times in 1500 ticks.

**exen-reloaded** (`src/core/melody.c`): the sequencer is a port of the timer proc running on the
audio sample clock (an event at `t` ms lands on sample `ceil(t * rate / 1000)` from the start), with
the deviations noted above (stop at the array end; release the old note on restart). The core
synthesizes the notes itself instead of using MIDI, with two host-selectable voices
(`exen_audio_set_voice`):
- `EXEN_VOICE_RINGTONE` (default): a PolyBLEP square wave, 3 ms attack, decay to 60 % over about
  250 ms, 12 ms release, a one-pole low-pass. Closer to a 2003 handset than the simulator was.
- `EXEN_VOICE_GM` (formerly `EXEN_VOICE_PIANO`, the same value): what the simulator actually
  played. Its notes went to the default MIDI device, on Windows the Microsoft GS Wavetable synth,
  whose program 0 is the acoustic grand piano. With a SoundFont from the host
  (`exen_audio_set_soundfont`; the releases ship TimGM6mb, `vendor/soundfont/`), the core sends the
  player's MIDI events to TinySoundFont (`src/core/gm.c`) exactly as the simulator sent them to
  `midiOutShortMsg`: note-on `0x90|ch, a, 0x40` (velocity 64), the note-off of `prev` on the
  current channel at the start of every firing (so notes are legato, and the SoundFont's own
  release overlaps the next note as on the GS synth), program changes `0xC0|ch, b` (b = 0: channel
  9 and the drum kit, program 0; GM channel 9 is always the drum kit, bank 128; b = 0x80, not a
  valid MIDI data byte, is taken as program 0, unverified), and the system reset of melody stop and
  of the end marker (all notes released, programs back to 0). The 0x84/0x85 device volume is the
  output gain, as `midiOutSetVolume` was. Note-ons reach the synth on the exact sample of the
  player's timer (10 ms after the start, then 15 ms per duration unit); what is heard starts with
  the SoundFont sample's own attack (0-3 ms for TimGM6mb's piano).
  Without a SoundFont the voice is procedural (no samples): up to 16 slightly inharmonic
  partials on two detuned strings each, struck-string partial levels, a two-stage decay that is
  faster for higher partials, a hammer-noise transient, and on note-off a 40-120 ms damper release,
  so the player's legato note changes overlap briefly as on the MIDI synth. That fallback plays
  every program as the piano (no game in the corpus sends a program change) and channel 9 as noise.

All voice pools receive every note event and always run; the setting only selects (with a 30 ms
crossfade) what is heard, so switching mid-melody never affects the sequencer. The voice is a host
setting and is not in save states; the piano pool's state is (optional `PIAN` chunk), and so are
the SoundFont's voices (optional `GMSF` chunk, restored exactly into a VM with the same SoundFont
and output rate, else restarted at the current note). The 0x84/0x85 levels are the L/R gains of
every voice. Channel 9 plays noise bursts in the ring tone. The EEPROM sound byte is **not**
honoured, because the shipped EEPROM would silence every game; hosts mute with
`exen_audio_set_volume`.

---

## 4. Host loop, timing and performance

### 4.1 Threads and loops [H]

| Thread / loop | Where | Blocks? |
|---|---|---|
| Main UI thread `WinMain` | 0x438d1e | Yes. It uses `GetMessageA` (0x439310). For the first 50 ms it also processes command-line files (0x439283 loop). |
| Idle hook after **every** dispatched message | `0x438840` | It polls the SMS queue, vibrator shake, backlight fade and synthesises `WM_PAINT` (`DispatchMessageA` of a fake `{hwnd, 0xf}`). |
| SMS server thread | 0x435396 | Yes (accept/recv) |
| Socket recvfrom thread | 0x435582 | Yes (unused) |
| Periodic-callback thread | 0x435861 | **No (spin)**, but dead code |
| Modal native UI loops (UIMenu / UIDialog / UIEditBox) | 0x436446, 0x4360a5, 0x43659a | **No: spin** (`PeekMessage`, else `0x438840`, repeat) |

The VM runs **synchronously on the UI thread**. There is no VM thread.

Every input to the VM is an "ExEn event" `{code, wParam, lParam}`. It is posted with `0x436e00`, which
maps the code to `WM_USER + index` through the pair table at `0x458094` (`600:1 601:2 602:3 200:4
300:5 301:6 100:7 101:8 500..508:9..0x11 603..605 700 800 509 510`) and calls PostMessage. The WndProc
then hands it back to `0x436f58` → core `0x402f10`, which dispatches on `code >> 8` through the table at
`0x449298`:

- `0x1xx` SMS
- `0x2xx` timer
- `0x3xx` key (0x300 press, 0x301 release)
- `0x4xx`
- `0x5xx` UI results and downloads
- `0x6xx` init (0x600 start the VM with a memory block, 0x601 ...)
- `0x7xx`

The core calls the current state's callback vtable at `[[ctx+0x14]]`:

- `0x456030`: the In-Fusio splash animation, 60 ticks, any key skips it
- `0x456760`: the running gamelet
- `0x4567d0` / `0x456c00`: the menu and SMS states

The vtable slots are `+8` timer tick, `+0xc` key press, `+0x10` key release.

For the running gamelet:
- tick → `0x41b624` → `0x40766e`: invoke the Gamelet's tick method (method record at `[ctx+0x28]+0x4b8`)
  through `0x4078f1`
- key → `0x41b5ee` → `0x4075ba`: push the key code and invoke the method at `+0x4b0`

Each callback runs the interpreter (`0x40dc74`) to completion. Afterwards `0x41b288(1)` runs a **full
mark-and-sweep GC** (`0x40a30b`) when the VM is idle, then checks for exit.

### 4.2 Timer and key semantics [H]

- `Gamelet.startTimer(int ms)` → `EXManufSetTimerTick(ms)`, clamped to 10..60000 → `SetTimer(hwnd, 1,
  ms)`. The log line "EXManufSetTimerTick speed=100" means **100 ms (10 Hz)**. The core sets 100 ms at
  init. `stopTimer` is `KillTimer(1)`.
- `WM_TIMER` (WndProc `0x4399e5`):
  1. refresh the memory-statistics dialog (`0x43cfbf`, a **walk of the whole VM heap block list** plus 5
     `SendDlgItemMessage`, every tick, even when the dialog is hidden)
  2. if `wParam == 1`, call `0x43f5f2`, which runs ExEn event 0x200 synchronously (`0x436f58(0x200, 0,
     0)`). If the VM returns 2 or 10 (exit), post WM_CLOSE.
  - "Manual timer tick" mode (`0x4c6580`, menu option) only lets a tick through after a CONTROL keypress.
- USER timers are coalesced and have 10 to 15.6 ms granularity. A tick is generated only when the queue
  is otherwise empty, and missed ticks are dropped, not queued. **If a tick takes longer than its period,
  the game simply runs slower** (no catch-up). Any `startTimer(n)` with n < ~16 actually runs at 64 Hz
  or less.
- `WM_KEYDOWN` (0x439a6f):
  - Auto-repeat is ignored (lParam bit 30 set ⇒ dropped).
  - The backlight is switched on. In game state (UI state 4) the key goes to `0x4375f0(0x300, vk)`.
- `WM_KEYUP` always sends `0x4375f0(0x301, vk)`.
- Key mapping in `0x4375f0` (VK → ExEn key code, the ASCII of the phone key). Unmapped keys are
  dropped.

| PC key | ExEn code |
|---|---|
| Enter (0x0d) | -8 (soft/select) |
| Esc (0x1b) | -7 (clear/back) |
| ←/↑/→/↓ | '4' / '2' / '6' / '8' |
| Numpad 0 | '0' |
| Numpad 7 8 9 | '1' '2' '3' (phone layout, rows flipped) |
| Numpad 4 5 6 | '4' '5' '6' |
| Numpad 1 2 3 | '7' '8' '9' |
| Numpad * | '*' |
| Numpad / | '#' |

  In the core, key events go to `0x403f90(code, pressed)` → vtable `+0xc` / `+0x10`. Code 0xff is
  ignored.

### 4.3 How a frame reaches the window [H]

`Gamelet.screenUpdate(int,int)` (`0x424ff2`) **ignores both int arguments**. It takes the Gamelet's
screen Image (field `+0x1c` → native image via `0x426785`) and calls `0x402304`:

1. The image must be ≤ the display size, or it returns -5.
2. Pick the display format from the image depth: 1 → fmt 0 (1bpp), 8 → fmt 2 (RGB332). fmt 3 (2bpp,
   4-grey) converts first through `0x402704`.
3. Centre the image in the display (`x = (W - w)/2`, `y = (H - h)/2`).
4. Call `0x4024ab` → **`EXManufDisplayUpdate` `0x4342f0`**, which:
   - converts every pixel to XRGB32 in `[0x4df890]` (W×H×4). The conversion multiplies each channel as
     a float and truncates with **`_ftol` (`0x4414f0`) per channel**:
     - RGB332: `R = (p>>5)*36.4286`, `G = ((p>>2)&7)*36.4286`, `B = (p&3)*85.0`
     - 2bpp grey: `v*85`
     - 1bpp: bit set → `0xC8C8C8`, clear → `0x373737`. MSB = leftmost pixel; rows are `w/8` bytes plus
       a partial byte.
   - then calls **`0x43dff2` synchronously** (the WM_PAINT routine is called directly, not posted).

`0x43dff2`, the repaint (also the WM_PAINT handler):

- a. Pick the source buffer by UI state (`0x43f591`): the game framebuffer, or the menu/dialog renderer
  (`0x43f7c2`, `0x43fdf5`, `0x440210` → `0x440436`).
- b. **Count distinct colours** for the Statistics dialog. This is an O(W·H·K) loop: for each of the
  W×H pixels it searches the list of colours seen so far linearly.
- c. Optional grid overlay (`0x4a20d4`).
- d. **Backlight**: multiply **every byte** of the W×H×4 buffer by the float brightness (`0x45c7c0`,
  0.7 to 1.0), again with an `_ftol` per byte. The default "auto" backlight dims to 0.7 after 30 s, so
  this always runs.
- e. `BeginPaint`, `GetDC`, `StretchDIBits` (32-bit DIB `0x4df860`, SRCCOPY) to the client area at
  ZoomRatio (default 3; ini value 2), `EndPaint`, `ReleaseDC`, then `0x43e432` (screenshot capture if
  enabled).

The display "hardware" therefore always has 8 bits per pixel internally. `Gamelet.getBitmapDepth()`
(`0x424fac` → `0x4022ba`) **returns the constant 8**, `numColors()` returns 256 and `isColor()` returns
true, whatever the profile says. See §5.

### 4.4 CPU burn while idle in a menu: root cause [H, confirmed by running]

**The ExEn launcher menu, and every native dialog or edit box, is a modal busy loop**
(`EXManufUIMenu` at `0x436446..0x436489`):

```c
while (!done && !quit) {
    if (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    else idle_hook_0x438840();      // a few GetTickCount calls, then returns immediately
}
```

There is no `WaitMessage`, `MsgWaitForMultipleObjects` or `Sleep`, so the UI thread never blocks.
`EXManufUIDialog` (`0x4360a5`) and `EXManufUIEditBox` (`0x43659a`) are identical.

When a gamelet is launched from the command line, the simulator first shows the ExEn core menu: the
log has `EXManufUIMenu [titleID=1, sizeList=5, *defaultSel=0]` right after start. The process then
spins. **Measured: 57% and 99% of one core over two 10 s windows** while sitting in that menu (scratch
copy, `gensimu.exe flash\download1.exn`). That matches the ~94% reported. The main `GetMessage` loop
is not the problem when nothing is happening.

Other busy-waits:
- `0x43373d`, used by the fake download delay, spins on GetTickCount.
- The dead periodic thread `0x435861`.

A port must replace all of them with blocking waits.

### 4.5 Why games run at low frame rates [M/H]

The cost of one `screenUpdate` on the original (unoptimised build, UI thread), for the default 132×176
display (23,232 pixels):

1. **Per-channel float → int**: 3 `_ftol` per pixel in EXManufDisplayUpdate, about 70k calls. The 2003
   CRT `_ftol` does `fnstcw`/`fldcw` twice per call, which serialises the FPU.
2. **Colour counting**: 23k pixels × up to 256 colours ≈ up to 3M compare iterations, each spilling to
   memory (debug codegen).
3. **Backlight**: 4 bytes × 23k = 93k more `_ftol` calls. This runs on every paint, including the
   synthetic WM_PAINTs during the 0.05-step fades and the vibrator shake.
4. GDI `StretchDIBits` of a 32-bit DIB scaled ×2 or ×3, done twice when a WM_PAINT follows.
5. Every WndProc message, WM_PAINT included, first calls `0x43f301`. That refreshes the "VM memory"
   statistics dialog: 3×`SendDlgItemMessage` + `UpdateWindow` + `ShowWindow`, even when the dialog is
   hidden. WM_TIMER and WM_PAINT also call `0x43cfbf`, which walks the whole VM heap block list and does
   5 more `SendDlgItemMessage`.
6. A **full GC after every callback** (tick or key), see §4.1.
7. Logging: with `LogManuf=1` (the shipped ini), every EXManuf call writes to `simulator.log` and
   updates the log window (`0x438146` → `UpdateWindow`). `EXManufStringGetPtr` is called twice per menu
   item per redraw, for example.
8. The native drawing primitives are *not* a big cost. There are no per-pixel calls: dispatch is one
   indirect call per row or span through the per-depth op vtables `0x45bca8` (4bpp) and `0x45bcf8`
   (8bpp). A palette LUT rebuild costs up to 256 entries per drawImage. See §2.1. The bytecode
   interpreter, however, is unoptimised debug code.

Timer granularity (≥ 10 to 16 ms, ticks dropped when late, no catch-up) turns any overrun directly into
a lower tick rate. Since everything above runs inside the tick, a game asking for 50 to 100 ms ticks on a
slow machine of the era could fall well below its intended rate. On a modern CPU the paint path is a few
ms, which costs frames only at high tick rates. Where the "extremely low framerate" reports come from
should be pinned down by profiling a specific game (open question).

`GetPixel` is used only once, at start-up (`0x43720c`), to rasterise the fonts the *simulator's own*
native menus and dialogs use. It is never called per frame. The VM's font is a built-in 5×8 bitmap at
`0x454dd0` (§2.1).

### 4.6 Start-up sequence (`0x438c06`, called from WinMain) [M]

1. `0x43abf3`, `0x43f170`, ... load the ini.
2. `0x436fdc` builds the 2 fonts for the *simulator-drawn* UI (EXManufUIMenu/Dialog/EditBox; not
   the VM font). It registers a hidden "FntBuild" window, draws 99 characters (the
   charset string at `0x45817c`) with `TextOutA`, reads them back with `GetPixel` (`0x43720c`) into font
   structs `0x4a2068` and `0x4a2080`, then destroys the window.
3. `0x4377ea` loads the language strings.
4. `0x43f5aa` sets the timer base.
5. `0x436650`, then `0x4351f8` starts the SMS server.
6. Backlight auto (`0x4340d0(2)`).
7. `malloc(core + small + big [+extra])` (§5), then post ExEn **0x600** with that block. The core then
   calls `0x403268` → `0x403c30`:
   - `0x404224` initialises the memory pool on the block
   - `0x432f50` builds the context `[0x45ff3c]` (0x58 bytes of sub-struct pointers: +0x14 callbacks,
     +0x20 display, +0x24 VM state, +0x28 class table, ...)
   - backlight and volume are set from the EEPROM
   - `exOnInit` runs

---

## 5. Device profiles [H]

`simulator.ini` `[Manuf.001..016]` sections are loaded by `0x43b2e6` into an array of 0x1024-byte
records at `0x4c65a8`:

| Offset | Key | Default (DEPTH 8) |
|---|---|---|
| +0x0000 | NAME (char[0x1000]) | |
| +0x1000 | EXEN_VM_BLOCKS_SMALL | 1 |
| +0x1004 | EXEN_VM_SIZE_SMALL | 0x7400 (29696) |
| +0x1008 | EXEN_VM_BLOCKS_BIG | 1 |
| +0x100c | EXEN_VM_SIZE_BIG | 0xec00 (60416) |
| +0x1010 | EXEN_CORE_MEMORY_SIZE | 0x5800 (22528) |
| +0x1014 | EXEN_DISPLAY_WIDTH | 121 |
| +0x1018 | EXEN_DISPLAY_HEIGHT | 143 |
| +0x101c | EXEN_DISPLAY_DEPTH | |
| +0x1020 | section number | |

(The depth-1 defaults in dead code are 1/3000/4/5000/0x2800/87/65.)

- **Only sections with `EXEN_DISPLAY_DEPTH == 8` are kept** (`0x43b404`: `jne skip`). The
  1-bit profiles (Alcatel OT526, Sagem MYX3, Philips Fisio 625) are discarded. This build is the colour
  simulator.
- The active profile is `[Manuf.current] CURRENT_COLOR` (an index into the *filtered* list, so 2 = the
  third 8-bit profile, Panasonic GD87 132×176). `CURRENT_BW` and `CURRENT_GRAY` are ignored
  (`0x43cc29`). This matches the log line `width=132 height=176 depth=8`.
- Display: `0x45c7c4` / `0x45c7c8` = width/height (WinMain `0x438f43`). They size the window (×ZoomRatio),
  the XRGB buffer, and the clamp in `0x40224b`/`0x402278`. The Gamelet screen Image is created at
  that size by the bytecode, from `getScreenWidth`/`getScreenHeight` (`0x424f99`/`0x424fbf` → display struct
  `[ctx+0x20]+0/+4`). The ini allows 50..230 in each dimension (`0x45d4e8`).
- Memory:
  - VM block size = `CORE_MEMORY_SIZE + EXTRA + VM_SIZE_SMALL + VM_SIZE_BIG` (`0x43cc63 + 0x43cc8d +
    0x43ccb0`; EXTRA = 0x19000 = 100 KiB when the "Extra Memory" menu option `0x4a20b8` is on).
  - It is `malloc`'d and handed to the core with event 0x600 (`0x438c79..0x438cc1`, again at
    `0x43f541` on reload).
  - The `*_BLOCKS_*` counts are stored but no reader was found here. The minimum block is 48 bytes
    (`0x45d54c`).
  - The split between core and VM heaps happens in the core pool init `0x404224`, which is for the VM
    agent.
- `.exn` header: the loader `0x406c85` reads the 16 bytes at offset 4 as 4×(u16,u16) into
  `res+0x4f0`. In every game they are (0x80,0), (512,8192), (1024,12288), (2048,24576), which looks
  like per-depth (1/4/8-bit?) memory requirements. No consumer was found yet. Open question.

---

## 6. Open questions

- **The In-Fusio decompression codecs** are the biggest blocker for porting. The same 5 decoders unpack
  PNG IDAT and type-0x81 resources. Dispatcher `0x432e50`: nibble = `first_byte >> 4` (`0x432f2b`),
  jump table `0x432f17`. 1 → `0x431750`, 2 → `0x42f560`, 3 → `0x42d500`, 4 → `0x432180`, 5 → `0x432a00`.
  Each init routine installs a decode function (1: 0x4318f6, 2: 0x42f724, 3: 0x42d960, 4: 0x43228b).
  Only codec 5 (LZ77) is decompiled (`natives/image_decode.c.txt`). Codec 3 is also used by the flash
  corpus and should be next.
- Paint modes: both analyses find `setPaintMode` to be a stub in both op tables, so PM_* would have no
  visible effect. Confirm by running a game that uses PM_XOR.
- The EEPROM scrambler `0x414d53` (only used when the image's first u32 is nonzero), the source
  of a File/Open gamelet id, and the record bytes `+0x168..+0x1fb`.
- How the core heap is split from the VM heap (`0x404224`), and whether `*_BLOCKS_*` is ever used.
- Method-hash function for 4CVP methods (not needed for binding).
- `0x407aae` / `0x410f92` run-time native linking: is it ever reachable (createTempClass always fails)?
- Whether 2bpp grey (fmt 3) is reachable at all, given that only 8-bit profiles load and depth is
  hard-wired to 8.
- Profile a real game to confirm which of the §4.5 costs dominates the reported very low frame rates.
- Melody: what real handsets do with command 0x82 (the simulator stalls on it), and whether they use
  the MIDI note numbers as is (BombJack goes down to E1, 41 Hz).
- ExEn events 0x4xx/0x7xx/0x8xx and the UI-state numbers of `0x43f591` (4 = game, 5 = menu, 7 = ?).
- FX, RayCast and DialogBox were translated from the code and checked visually, not compared frame
  by frame with the running simulator. The rotation direction of doRotazoom and the handedness of
  the ray caster follow the arithmetic; a side-by-side capture against gensimu would settle them.
- List.drawList is not called by any game in the corpus, so its port is unexercised.
- Sms.readBits/getbits with n > 32 - (pos & 7) uses the odd 5-byte path (0x412fab); no game seen
  reads more than 16 bits at a time.
