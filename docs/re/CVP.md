# `4CVP` prelinked class images

The ExEn VM loads code as a sequence of **`4CVP` class blocks**: one block per class, already
linked against a fixed class numbering. The framework (54 classes) lives in `gensimu.exe`
`.rdata`; a game's `.exn` ends with its own blocks, which are numbered after the framework.

`tools/cvp.py` is the executable version of this document (`info`, `dump`, `check`, `fw`, `hash`).
`check` re-verifies every claim marked *verified* below against the framework and all 47 games.

Addresses are gensimu.exe virtual addresses (image base `0x400000`). The binary is an
unoptimised debug-style MSVC build, so every structure access is a literal offset, which makes
the readings below fairly direct. All integers are little-endian.

Confidence: **verified** = checked by `cvp.py check` on all 54 framework classes and all 47 games
(460 classes, 8776 methods, 0 anomalies); **code** = read from the disassembly; **inferred** = a
consistent pattern without a direct code reading.

## 1. Image layout and block walking

An image is a run of blocks. The walker (code: `0x406fc8` for the game chunk, `0x40713f` for the
framework) is:

```
p = 0
while p < len:
    assert u32[p] == '4CVP'
    register block p as class #next_index
    p = align4(p + u16[p+4])            # block size
    while p < len and u32[p] != '4CVP': p += 4
```

* The framework image is at VA `0x4494f0`, length `0xaf8c` (immediates at `0x40714c`/`0x407153`),
  i.e. file offset `0x494f0`. It holds 54 blocks. (The other four `4CVP` hits in `.text` are the
  tag compares at `0x407103`, `0x407266`, `0x41075e`, `0x4111fa`.) `cvp.py fw` finds the blob by
  that instruction pair, so it does not hard-code the address.
* The game chunk is everything from the NEXE code offset to end of file. 43 games end exactly on
  a block; 4 have 1–6 bytes of trailing padding, which the walker skips.
* Blocks are 4-byte aligned in practice; no gaps were seen.
* Block size is a `u16`, so a class is at most 64 KiB.

### Class numbering (code, verified)

`0x406fc8` first calls `0x40713f(0)`, which registers the 54 framework blocks as classes 0..53 and
returns 54. It then registers the game blocks as 54, 55, ... in file order. All class references in
both images are these global indices, so **the framework order is part of the ABI**: a C loader
must load the framework blocks in exactly the `.rdata` order, then the game blocks.

The runtime class table (`[[0x45ff3c]+0x28] + 0xc`, also `[0x51f900]+8`) has 12-byte entries:
`+0 u16 index`, `+4 block pointer`, `+8 u16 loader/group id (0)`, `+0xa u16 state`. Both loaders
write state `0xb`; a terminating entry has state bit `4` (`0x40711b`). State `&3 == 3` means
"prelinked, usable"; `&3 == 2` is an unlinked class (see §8).

Framework order (index: name): 0 exen.Animation, 1 exen.Graphics, 2 exen.Image,
3 exen.TextureMap, 4 exen.Rectangle, 5 exen.Palette, 6 exen.Resource, 7 exen.AnimBitmap,
8 exen.PlayField, 9 exen.Sprite, 10 exen.AnimFlash, 11 exen.Command, 12 exen.CommandListener,
13 exen.Displayable, 14 exen.Gamelet, 15 exen.Component, 16 exen.Sms, 17 exen.DialogBox,
18 exen.FX, 19 exen.GameletEnhanced, 20 exen.List, 21 exen.Math, 22 exen.Matrix3D,
23 exen.Vector3D, 24 exen.Palette_RAW, 25 exen.Point2D, 26 exen.RawData, 27 exen.RayCast,
28 exen.util.Debug, 29 java.lang.Object, 30 java.lang.Class, 31 java.lang.Exception,
32 java.lang.Throwable, 33 java.lang.String, 34 java.lang.ClassLoader, 35 java.lang.StringBuffer,
36 java.lang.System, 37 java.lang.Error, 38 java.lang.Boolean, 39 java.lang.Byte,
40 java.lang.Character, 41 java.lang.Cloneable, 42 java.lang.CloneNotSupportedException,
43 java.lang.Integer, 44 java.lang.Long, 45 java.lang.OutOfMemoryError,
46 java.lang.VirtualMachineError, 47 java.lang.Short, 48 java.io.Serializable,
49 vm.sys.Runtime, 50 vm.sys.Application, 51 vm.sys.Bootstrap, 52 catalog.Catalog,
53 catalog.GameProperty.

## 2. Offsets and conventions

* Every offset stored in a block is **relative to the block start** (the `4`). Bytecode pcs are
  block offsets too (the frame pc starts at `code_off + 6`, `0x40e1aa`).
* A *table* is `u16 count` followed by entries starting at the next 4-byte boundary
  (`(p+2+3)&~3`; e.g. `0x40e80b`, `0x40e63a`, `0x410c4a`).
* A **class-ref** is an offset to a slot in the class-ref table (§4.1); the class index is the
  `u16` at that offset (`[block + ref]`, e.g. `0x40e40d`, `0x40e5c9`, `0x40977d`). Offset 0 means
  "none".
* Field and method records are 10 bytes of data in a 12-byte stride: the readers step
  `+0xc` then align 4 (`0x40e852`). The 2 bytes after the 10th byte of the *last* record are
  not padding: the next structure begins there (e.g. the next table's count, or the framework
  debug lists). Readers must not interpret record bytes 10–11. (verified)
* Strings (debug names, string literals) are `u16 length` + bytes (8-bit, no terminator), padded
  to an even length.

## 3. Class header

| Off | Type | Meaning | Evidence |
|---|---|---|---|
| `0x00` | `char[4]` | `4CVP` | `0x407103` |
| `0x04` | `u16` | block size in bytes | `0x4070d7`, `0x407240` |
| `0x06` | `u16` | `0x0100` in all 460 blocks. Unknown (format version?) | never read found |
| `0x08` | `u32` | `0x27dd` (10205) in all 460 blocks. Unknown (compiler/VM version?) | never read found |
| `0x0c` | `u32` | **class hash** = CRC-32 of the dotted name (`exen.Graphics` → `c6ed8e2a`) | `0x410409`, `0x40fecc`; verified for all 54 named classes |
| `0x10` | `u32` | 0 in 435 blocks; `0x00320000` on all `java.*` framework classes; a few other values in games. Unknown | — |
| `0x14` | `u16` | class flags (below) | `0x4071e6` (`&0x200`) |
| `0x16` | `u16` | offset of the class name string, 0 if stripped (all games) | inferred (framework only) |
| `0x18` | `u16` | number of static slots | `0x40e43b` (allocs `n*4+0x10`) |
| `0x1a` | `u16` | offset of the `<clinit>` method record, 0 if none | `0x40e4cd` → invoke `0x40e02c` |
| `0x1c` | `u16` | offset of the interface list (§4.2), 0 if none | `0x40e39c` |
| `0x1e` | `u16` | offset of the field table (§4.3), 0 if none | `0x410eb0` |
| `0x20` | `u16` | offset of the method table (§4.4), 0 if none | `0x40e7d2`, `0x40df27` |
| `0x22` | `u16` | offset of the class-ref table (§4.1) | `0x410781`, `0x41121d` |
| `0x24` | `u16` | offset of the field-ref table (§4.7), 0 if none | `0x410e07` |
| `0x26` | `u16` | offset of the method-ref table (§4.6), 0 if none | `0x410c09` |
| `0x28` | `u16` | class-ref of the superclass, 0 for `java.lang.Object` | `0x40ed95` (recursive object alloc) |
| `0x2a` | `u16` | number of instance slots declared by *this* class (longs count 2) | `0x40edcc` (alloc `n*4+0x18`) |
| `0x2c` | `u16` | offset of the `<init>()V` method record (no-arg constructor), 0 if none | `0x4101de` (invoked on new objects), `0x4100d3` |

**Interfaces have a 0x28-byte header**: `0x28..0x2d` are absent and the next data starts at
`0x28` (e.g. `exen.Animation`, `java.lang.Cloneable`, whose name string is at `0x28`). Every
consumer of `0x28..0x2c` only runs on non-interface classes (`0x40ed5a` is only reached via `new`;
`0x410bf2` skips `flags & 0x200`). (verified by parse; inferred rule)

Every `<clinit>` record in the corpus has hash `3f5201b3` (`<clinit>()V`); every `+0x2c` record
has `3f52ef2f` (`<init>()V`). (verified)

### Class flags (`+0x14`)

Java access-flag layout: `0x0001` public, `0x0010` final, `0x0020` super, `0x0200` interface
(checked by code: `0x4071ea`, `0x410bf6`, `0x410df0`, `0x410ff2`), `0x0400` abstract. Additional
bits with unknown meaning: `0x0008` (all framework classes, some game classes), `0x0080`,
`0x1000`, `0x8000`. Seen framework values: `80a9 00a9 00b9 0689 80b9 04a9 90a9 80f9 10a9 10a8 80e9
84a9`; game values: `00b1 10b1 00a1 10a0 00b9 10a1 00a0 10e1 ...`.

## 4. Tables

### 4.1 Class-ref table (`+0x22`)

Table of `u32`, one per class this class mentions (including itself, its superclass, field and
argument types, catch types, `new`/`checkcast` targets). In prelinked images the low `u16` is the
**global class index**; the high half is 0. Everything else refers to classes through the
*offset* of one of these slots.

In the unlinked form (state `&3 == 2`, never used by shipped images), the slot holds the target's
32-bit class hash and the link pass `0x410689` rewrites it with the index found by
`0x410b04` (match on block `+0x0c`). (code)

### 4.2 Interface list (`+0x1c`)

`u16 count`, then `count` × `u16` class-ref offsets (no alignment). `initClass` (`0x40e359`)
initialises these classes before the class's own statics/`<clinit>`. Seen only for implemented
interfaces (e.g. `exen.AnimBitmap` → `exen.Animation`, `java.lang.String` →
`java.io.Serializable`); the superclass is *not* in this list.

### 4.3 Field table (`+0x1e`)

Table of 10-byte records, 12-byte stride:

| Off | Type | Meaning |
|---|---|---|
| `+0` | `u32` | member hash (§5) |
| `+4` | `u16` | access flags: `1` public, `2` private, `4` protected, `8` static, `0x10` final, `0x40` volatile |
| `+6` | `u16` | type code (§6) |
| `+8` | `u16` | slot: index into the class's static area if `static`, else into this class's instance segment |

Slots are per declaring class and start at 0 in each class (e.g. `exen.GameletEnhanced.m_displayable`
is instance slot 0 although `Gamelet` has 2). `long` fields take two slots. Static final
constants are ordinary static fields filled by `<clinit>`; there are no constant-value
attributes. (verified)

### 4.4 Method table (`+0x20`)

Table of 10-byte records, 12-byte stride (search loop `0x40e81f`, `0x40df77`):

| Off | Type | Meaning |
|---|---|---|
| `+0` | `u32` | member hash (§5) |
| `+4` | `u16` | flags: `1` public, `2` private, `4` protected, `8` static, `0x10` final, `0x100` native (`0x40e051`), `0x400` abstract |
| `+6` | `u16` | argument words, excluding `this` (`long` = 2) (`0x40e0dc`) |
| `+8` | `u16` | offset of the code (below); 0 for abstract/interface methods |

Order in the table is declaration order; `<init>` overloads and others share name halves of the
hash (`drawImage` ×4 → `xxxxc07b`).

### 4.5 Method code

Non-native (`0x40e0c6`): at `code_off`:

| Off | Type | Meaning |
|---|---|---|
| `+0` | `u16` | max stack words |
| `+2` | `u16` | max local words, including `this` and the arguments |
| `+4` | `u16` | offset of the exception table, 0 if none (`0x4096ad`) |
| `+6` | `u8[]` | bytecode; execution starts here |

The frame needs `max_locals + max_stack + 13` words (`0x40e0f8`); locals beyond the arguments are
zeroed (`0x40e231`). There is no code-length field: a method's bytecode ends where the next
structure begins (the next method's code, an exception table or an inline constant). `cvp.py`
reports the end as "next known offset", which can include trailing constants.

Exception table (`0x409651`): `u16 count` (no alignment), then `count` × 8 bytes:
`u16 start_pc, u16 end_pc (inclusive), u16 handler_pc, u16 catch class-ref (0 = any)`. The
matching walks the thrown object's segment chain (`[seg+0x10]`) comparing the class index at
`seg+0xa` against `[block+catch_ref]`. Only 2 methods in the corpus have handlers, both in the
framework (`vm.sys.Bootstrap.init` and `vm.sys.Runtime.createRuntime`); no game uses
`try`/`catch`.

Native (`flags & 0x100`): at `code_off` is a `u32` **native index**, an index into the
function-pointer table at VA `0x457c08` (`call [idx*4 + 0x457c08]`, `0x407aa2`). Natives are
already numbered in the framework image (`Graphics.clearRect` → 0, `drawLine` → 2, ...). No game
class declares a native method.

### 4.6 Method-ref table (`+0x26`)

Table of 8-byte entries (`+8` then align 4, `0x410d68`):

| Off | Type | Meaning |
|---|---|---|
| `+0` | `u32` | member hash of the target method |
| `+4` | `u16` | argument words of the target (verified: equals target `argc` for all 10801 refs); the invoke handlers use it to find the receiver (`0x40bf66`) |
| `+6` | `u16` | class-ref of the class that declares the target |

Bytecode invoke instructions name a method-ref by its block offset (u16 operand, see
OPCODES.md). Calls to methods of the same class name the method record itself (e.g. Graphics
bytecode `ed 00 58 08` → record at `0x858`).

### 4.7 Field-ref table (`+0x24`)

Same 8-byte layout: `u32 hash`, `u16 type code of the target field` (verified for all 3179 refs),
`u16 class-ref of the declaring class`.

### 4.8 Inline constants and string literals

There is no constant pool. Instructions that load constants carry a 2-byte-aligned `u16` block
offset (code):

* opcode `0x12` (`0x40cebc`): pushes the `u32` at that offset.
* opcode `0x14` (`0x40cf42`): pushes the 8-byte value at that offset.
* opcode `0xd0` (`0x40cdaa`): creates a `java.lang.String` whose data pointer (`obj+0x18`) points
  straight at the length-prefixed string at that offset (8-bit chars, `u16` length).

The constants live inside the block next to the code; their exact placement is not separately
described by any header field.

## 5. Name hashes (verified)

* **Class hash** = standard CRC-32 (zlib) of the dotted class name. Function `0x4105f5`
  (`crc32(buf, len)`, table at `0x4548a0`); `0x40fecc` = `findClassByHash(crc32(name))` via
  `0x4102d4`. Used by `Class.forName` and for the VM's well-known classes.
* **Member hash** (field and method) = `crc16(descriptor) << 16 | crc16(name)`, where `crc16` is
  CRC-16/MCRF4XX (reflected poly `0x8408`, init `0xFFFF`, no final xor) and `descriptor` is the
  standard JVM descriptor with slash-separated class names:

  | member | hash |
  |---|---|
  | `<init>()V` | `3f52ef2f` |
  | `drawLine(IIII)V` | `cf20ea2f` |
  | `<init>(Lexen/Image;)V` | `d5e1ef2f` |
  | `<clinit>()V` | `3f5201b3` |
  | field `PM_COPY S` | `6f994aa8` |

  This was found by brute force over CRC-16 parameters and then verified against all 529
  framework methods and all framework fields using their debug names and signatures. gensimu
  never computes member hashes at run time (no CRC-16 code or table exists in the binary); it only
  compares them, and uses a few as immediates (`0x407301`: Bootstrap entry points;
  `0x40dbf2`). Overrides keep the overridden method's hash, which lets `cvp.py` name game methods
  such as `onInit` / `paint`.

## 6. Type codes (verified)

`u16`: low byte = element type, high byte = array dimensions.

| code | type | | code | type |
|---|---|---|---|---|
| `0x00` | `V` (void) | | `0x90` | `B` byte |
| `0x15` | `S` short | | `0xd5` | `Z` boolean |
| `0x55` | `C` char | | `0xd0` | `Z` as array element (`[Z` = `0x1d0`) |
| `0x59` | `I` int | | `0x99` | object (class from a class-ref) |
| `0x6e` | `J` long | | | |

No float/double types appear. Examples: `0x159` `[I`, `0x190` `[B`, `0x199` object array,
`0x259` `[[I`. Without debug info the class of an object-typed field can be recovered from the
descriptor half of its hash (`0x3dd3` = `Lexen/Image;`).

## 7. Debug information (framework only)

Framework blocks carry names; game blocks carry none (header `+0x16 = 0`, no lists). All debug
data is found from the tables, not from header fields:

* Strings start right after the header (`0x2e`, or `0x28` for interfaces).
* **Field names**: at `fields_end - 2` (where `fields_end = first_record + 12*count`), `count`
  pairs `u16 type class-ref, u16 name-string offset`.
* **Method signatures**: at `methods_end - 2`, `count` × `u16` offsets of signature records:
  `u16 name-string offset, u16 throws-list offset (0 = none), ret type, u16 arg count,
  arg types...`, where each type is a `u16` code followed by a `u16` class-ref if the code's
  low byte is `0x99`. The throws list is `u16 count, u16 class-ref...`.
* Class name: header `+0x16`.

## 8. Linking and resolution at run time

**Prelinked images skip linking.** `0x410670` runs four passes (`0x410689` class refs by hash,
`0x410b96` verify method refs, `0x410d94` verify field refs, `0x410f92` bind natives through the
`{methodHash, classHash, fn}` search `0x407aae`), but only over state-2 classes or as checks;
nothing in gensimu.exe calls `0x410670` (no call, no stored pointer). Shipped images are therefore
used exactly as stored.

Member references are resolved **by hash at every execution**, not by index (code):

* Static / special calls (`0x40e747`, `0x40e5a2`): take the class index from the ref's class-ref,
  then linearly search that class's method table for the 32-bit hash.
* Virtual calls (`0x40df05`, used by opcode `0xee` at `0x40c3a9`): start at the receiver's
  most-derived class and search each method table by hash, walking up the superclass chain.
* Failure throws `java.lang.LinkageError` (hash `13523f29`).

Every method ref and field ref in the corpus names the class that *declares* the member, and the
hash is present there (verified by `cvp.py check`). So in game code, a framework call is: operand
→ ref entry → class-ref → framework class index → hash lookup in that framework class.

Objects are a chain of per-class segments, one per class in the hierarchy (`0x40ed5a`): each is
`0x18` bytes of header (`+4` block, `+0xa` class index, `+0xc` tag `0x99`, `+0x10` next segment
toward the superclass) plus `+0x2a` slots. Static areas (`0x40e359`) are `0x10 + 4*nstatics`
bytes with tag `0x19`, created when the class is first initialised, after which `<clinit>` runs.

## 9. Boot sequence

1. `0x4069ba` opens the `.exn`; `0x406c85` parses the NEXE container; `0x406fc8` loads the code
   chunk, registering the framework (`0x40713f`) then the game classes.
2. VM init `0x40d970` caches the indices of well-known classes by CRC-32 (`0x40db48..`):
   `java.lang.Short`, `Integer`, `Long`, `Boolean`, `String`, `Byte`, `Character` (stored at
   `[0x51f900]+0x20..0x2c`).
3. `0x407301` resolves eight static methods of `vm.sys.Bootstrap` (`6551f7dc`) by member hash
   (`0x40e747`) and keeps them at `[[0x45ff3c]+0x28]+0x4b0..`: `keypress(I)V 305a6d35`,
   `keyrelease(I)V 305a1e56`, `tick()V 3f522033`, `init([C)V 35b0f11e`, `smsReceived([B)V
   6f6c0565`, `smsSent(I)V 305a7631`, `NickNameChanged([C)V 35b0015c`, `exit()V 3f523566`.
4. The host calls `Bootstrap.init(char[] mainClassName)`. Its bytecode does
   `Class.forName(name).newInstance()` (natives; `forName` hashes the name with CRC-32), stores
   the `exen.Gamelet` in a static, and calls `onInit()`. Later host events call the other
   Bootstrap statics, which forward to `Gamelet.onKeyPress`, `onTimerTick`, and so on.

## 10. Functions referenced

| VA | Role |
|---|---|
| `0x406fc8` | load game code chunk, register blocks |
| `0x40713f` | register framework blocks from `0x4494f0` |
| `0x407301` | resolve Bootstrap entry points by hash |
| `0x407a94` / `0x457c08` | native dispatch / native function table |
| `0x407aae` | find native by `(methodHash, classHash)` (unused by prelinked images) |
| `0x409580`, `0x409651` | throw by class hash; exception-table search |
| `0x40cebc`, `0x40cf42`, `0x40cdaa` | load 4-byte / 8-byte constant / string literal |
| `0x40d970` | VM init, well-known class lookup |
| `0x40df05` | virtual method lookup by hash |
| `0x40e006` | class index → block pointer |
| `0x40e02c` | invoke method record (native or bytecode frame setup) |
| `0x40e359` | initialise class (interfaces, statics, `<clinit>`) |
| `0x40e5a2` | non-virtual method lookup via method ref |
| `0x40e747` | static method lookup `(memberHash, classHash)` |
| `0x40ed5a` | allocate object (segment chain) |
| `0x4101c4` | run `+0x2c` default constructor on a new object |
| `0x4102d4` | class hash → class index |
| `0x4105f5` | CRC-32 |
| `0x410670` | link/verify passes (dead code) |

## 11. Open questions

* Header `+0x06` (`0x0100`), `+0x08` (`0x27dd`) and `+0x10` are constant or rare and never read in
  the code paths examined. They might be a version stamp and a package/loader id.
* Class flag bits `0x0008`, `0x0080`, `0x1000`, `0x8000`.
* The exact placement of inline constants and string literals relative to method code. Only the
  instruction operands locate them; a full listing needs the instruction lengths from OPCODES.md.
* Field record `+6` uses the same type codes as field refs, but whether the VM reads it (for
  example for typed `getfield`) has not been checked.
* `0x411154` walks the class-ref tables of all classes again (`+0x22`); its purpose (unloading?)
  was not determined.
* Write path: some code writes into blocks through a page cache (`0x407b20` / `0x407eb5`,
  base `[0x51f910]`), probably to simulate code in flash. Only matters for unlinked classes.

## 12. Notes for other areas

* **Opcodes**: the handler table is at VA `0x454498` (256 × `u32`, `0x4102cf` = invalid opcode).
  Java opcodes `0x00..0xb1` are mostly kept; `0xb2..0xff` are In-Fusio's. Operands that name
  block structures are `u16` block offsets aligned to 2 bytes (the handlers do `pc = (pc+1)&~1`
  first). Observed: `0xd0` string literal, `0x12`/`0x14` 4/8-byte constant, `0xf0` non-virtual
  invoke through a method ref, `0xee` virtual invoke through a method ref, `0xed` invoke of a
  method record in the same class, `0xf6` / `0xf5` with a field-record offset (look like
  put/get field). The frame pc is a block offset (`frame+0x1e`), the block pointer is at
  `frame+0xc`, and the current class index at `frame+0x1c`.
* **Natives**: a native method's code word is an index into the function-pointer table at VA
  `0x457c08` (`Graphics.clearRect` = 0 → `0x425c73`). The VM throws by class hash
  (`[0x51f900]+0x1c`): `OutOfMemoryError f2f8aa1c`, `NullPointerException 364a8d65`,
  `StackOverflowError a2c72bf1`, `ArithmeticException 86f6360b`, `ClassNotFoundException f2985715`,
  `LinkageError 13523f29`. Only `OutOfMemoryError` has a class in the framework.
