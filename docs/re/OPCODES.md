# ExEn VM bytecode and execution model

These notes come from disassembling `vendor/gensimu/gensimu.exe`. The image base is `0x400000`.
The interpreter was compiled with MSVC without optimisation (`/Od`). Every handler is a small
`cdecl` function that reloads globals on each access, so the code reads almost like C source. The
tool `tools/disasm.py` implements this table. Check it against the whole corpus with the
flow-following sweep described in [Validation](#validation).

Confidence levels: **H** means read directly from the handler code. **M** means inferred from
code plus usage. **L** means a guess.

## Summary

* **The numbering is JVM-based, with changes.** Opcodes that exist keep their JVM number and JVM
  meaning: constants, `aload*`/`astore*`, the array ops, the stack ops, the int/long arithmetic,
  the branches, the switches, `new`/`newarray`/`athrow`/`checkcast`/`instanceof`, `ifnull`.
  The prelinker rewrote some groups into new opcodes in the JVM's unused range `0xcc–0xfa`:
  * int/long local loads and stores (`0xd5–0xe8`),
  * `ireturn`/`lreturn` (`0xe9`, `0xea`),
  * the string `ldc` (`0xd0`),
  * compact switches (`0xcc`, `0xcd`),
  * field access and invokes (`0xed–0xfa`), each split into a "local" form (a member of the
    current class) and an "external" form (resolved by hash).
* **No floating point.** Every `f*`/`d*` opcode, plus `iload`/`lload` `0x15–0x18`, the JVM
  `xload_n`/`xstore_n` int/long forms, `ldc_w`, `anewarray` (0xbd), `wide`, `goto_w`/`jsr_w`, and
  the JVM `getfield`/`invoke*` numbers `0xb2–0xba`, all go to the empty function `0x4102cf`.
  **Undefined opcodes are silent 1-byte no-ops. They do not trap.** (H)
* **Operands are not JVM-encoded.** Every 16-bit operand is first padded to an even pc.
  Branch targets are **absolute** u16 offsets from the class-block base, not relative
  displacements. Constants, class refs, method refs and field refs are u16 offsets to inline data
  in the same class block. There is no constant pool. (H)
* **pc is a u16.** A class block therefore holds at most 64 KiB, and code addresses are offsets
  from the start of the class's `4CVP` block. (H)
* **Longs are supported** (2 stack slots, MSVC `__allmul`/`__alldiv`/... helpers). **Chars are
  8-bit:** `char[]` elements are 1 byte, `caload` zero-extends a byte, and `i2c` is `& 0xff`. (H)

## Interpreter core

| Address | What |
|---|---|
| `0x454498` | Handler table, `void (*[256])(void)`, indexed by opcode byte (H) |
| `0x454898` | Two more pointers after the table: `0x409580` (throw) and `0x40dccb` (stop VM). Not opcodes. |
| `0x40dc74` | **Run loop** (H) |
| `0x51f904` | `R`: pointer to the **current frame**. All handlers work through it. (H) |
| `0x51f900` | `X`: VM context (class table, stack limit, pending exception, flags) (H) |
| `0x45ff3c` | `G`: host/simulator context. `*(G+0x24)` is a status word; `2` means stopped/error. (M) |

Run loop `0x40dc74`, exact:

```c
while (R->pc != 0) { uint8_t op = R->code[R->pc]; R->pc++; handlers[op](); }
```

The loop has no instruction budget, no time check, no message pumping and no yield. A pc of 0
is the sentinel for "return to host", because method code always starts at offset 6 or later in
a block. The loop is re-entered for every **nested run**:

| Nested run | Entry |
|---|---|
| Host callback | `0x4078f1` |
| Finalizer call | `0x40e86c` |
| `<clinit>` | inside `0x40e359` |
| Constructor of a VM-created object (for example an exception) | `0x4101c4` |

Each nested run saves the caller frame's pc, sets it to 0, invokes the method, and runs
`0x40dc74`. The callee's return pops back to that frame, its pc is 0, the loop exits, and the
saved pc is restored. **Every host-to-VM call runs synchronously to completion.** (H)

### Frame layout

A frame lives on the VM stack directly above the callee's locals. The allocation is done in
`0x40e02c` (invoke core). Offsets are bytes. (H)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | ptr | previous (caller) frame. `return` does `R = R->prev`. |
| +0x04 | ptr | stack-limit address for this frame (`R->sp + need*4`, see below) |
| +0x08 | ptr | method descriptor being executed (12-byte entry, below) |
| +0x0c | ptr | code base = start of the class's `4CVP` block |
| +0x10 | u32* | `sp`: next free operand slot. The stack grows upward and slots are 32 bits. |
| +0x14 | u32* | `loc`: locals base. The args are locals 0..n-1, `this` is local 0. |
| +0x18 | ptr | copy of `X+0x18` at call time. `X+0x18` = last allocated object (allocator `0x40abf5`). Purpose unknown. (L) |
| +0x1c | u16 | current class id |
| +0x1e | u16 | `pc` |
| +0x20 | u16 | `jsr` return pc (a single register, so `jsr` does not nest) |
| +0x22 | u8 | call depth. Used by the GC as an object's "stack level". |
| +0x24 | | operand stack starts here |

Invoke `0x40e02c(classId, M, thisSeg, args, hasThis)`, bytecode path:

1. `C = classptr(classId)`, from `0x40e006`: `*(X->classes + id*12 + 4)`.
2. `H = C + M.code_off`, giving the method header `{u16 max_stack, u16 max_locals, u16 exc_table_off}`.
   The code starts at `H+6`.
3. `nargs = M.nargs + hasThis`. Compute
   `need = max_locals + max_stack + 13 - nargs` (in words).
   If `R->sp + need*4 >= X->stack_limit (X+0xc)`, throw hash `0xa2c72bf1` (H). This is
   probably `java.lang.StackOverflowError`; the name is not confirmed.
4. The arguments have already been popped by the opcode, so `args == R->sp`. The new frame `F` is at
   `args + max_locals*4`, so the caller's argument area *is* the callee's locals.
5. Set `F->prev = R`, `F->depth = R->depth + 1`, `F->method = M`, `pc = M.code_off + 6`,
   `sp = F + 0x24`, `code = C`, `class = classId`, `X+0x1c = 0`.
6. Zero the locals beyond the arguments.

The caller's `sp` stays at the argument base. Return values are written there.

Native path: if `M.flags & 0x100`, take `idx = *(u32*)(C + M.code_off)` and call
`n = natives[idx](args, thisSeg)` through `0x407a94`, which does
`call [idx*4 + 0x457c08]` with 185 entries. Then `R->sp += n*4`. The native writes its result at
`args[0]` and returns its size in words. Afterwards, if `X+0x14 != 0 || X+0x1c != 0`, the invoke
returns "exception" and the opcode calls `throw(X+0x1c)`. (H) The natives are covered in
`NATIVES.md`.

## Object and array representation

Heap blocks come from the free-list allocator `0x40a985`, wrapped by `0x40ac24`. On failure it
calls `gc()` at `0x40a30b` and retries once. If that also fails it prints
"No more memory in the VM ..." and returns NULL. The size argument is a **u16**. (H)

The object header is common to all heap blocks. (H)

| Off | Type | Meaning |
|---|---|---|
| +0x00 | ptr | next in the all-objects list (`*0x51f90c`) |
| +0x04 | ptr | class pointer |
| +0x08 | u16 | block size |
| +0x0a | u16 | class id. For arrays it is the element class id or the primitive wrapper class. |
| +0x0c | u16 | type word. Low byte is the type tag; high byte is non-zero for arrays (`0x01xx`). `0x99` = plain object, `0x19` = statics block, `0` = freed. |
| +0x0e | u8 | **reference count** (heap references: fields, array elements, super link, pending exception) |
| +0x0f | u8 | **stack level**: call depth of the shallowest frame known to hold it. 0 = none. |
| +0x10 | ptr | objects: next (super-class) segment. Arrays: start of the u16 length at `+0x12`. |
| +0x12 | u16 | arrays: length |
| +0x14 | | arrays: elements |
| +0x18 | | objects: instance fields, 4 bytes per slot (a long takes 2 slots) |

**Objects are chains of per-class segments** (`new` = `0x40ed5a`, H). `new C` allocates one segment
of `0x18 + 4*C.ninstfields` bytes for C. It recursively does `new super` (with refcount 1) and
links it at `+0x10`. A reference normally points to the most-derived segment.

* `getfield_local`/`putfield_local` and the `*_local` invokes first find the segment whose class
  id equals the current frame's class (`0x40e6da`). If none matches they throw
  `java.lang.LinkageError`.
* External field access walks the chain matching the field hash (`0x40ddf4`).

**Arrays** (`newarray` `0xbc`, H):

* Element size is `1 << ((tag >> 2) & 3)`.
* The allocation is `0x14 + n*elsize` bytes, **truncated to u16**. For example an `int[]` with
  more than 16379 elements gets a short block but keeps its full length, which corrupts the heap.
* Lengths are limited to `n <= 0xffeb`. Longer requests return NULL and rethrow whatever is pending.

Type tags (H for the values; bit meanings M). Bits 2–3 hold log2 of the element size. Bits 4–5
hold the stack-slot count, used by `putfield`/`putstatic` to know how much to pop.

| Tag | Type | Element bytes | Slots | Wrapper class hash (table `0x454ca4`) |
|---|---|---|---|---|
| `0x50` | char | **1** | 1 | `java.lang.Character` |
| `0x90` | byte | 1 | 1 | `java.lang.Byte` |
| `0x15` | short | 2 | 1 | `java.lang.Short` |
| `0x59` | int | 4 | 1 | `java.lang.Integer` |
| `0x6e` | long | 8 | 2 | `java.lang.Long` |
| `0x99` | reference | 4 | 1 | (element class id from the operand) |
| `0xd5` | boolean | **2** (but accessed as 1 byte) | 1 | `java.lang.Boolean` |
| `0x1a` | float | 4 | 1 | `0xa931f301` (unknown name) |
| `0x2f` | double | 8 | 2 | `0xa931f301` |

The array opcodes check the tag with a **mask**, `(tag & M) == M`. They do not test equality.

| Opcodes | Mask |
|---|---|
| `iaload`/`iastore` | `0x59` |
| `laload`/`lastore` | `0x6e` |
| `baload`/`bastore` | `0x90` |
| `caload`/`castore` | `0x50` |
| `saload`/`sastore` | `0x15` |
| `aaload`/`aastore` | none; only "is an array" |

So `baload` accepts `boolean[]` (`0xd5 & 0x90 == 0x90`), and `saload` accepts `boolean[]` too.

**Class names are identified by `crc32` of the dotted name** (zlib CRC-32). For example
`crc32("java.lang.NullPointerException") = 0x364a8d65`. Hashes resolved so far:

| Hash | Name |
|---|---|
| `0x364a8d65` | NullPointerException |
| `0x1d3c3033` | ArrayIndexOutOfBoundsException |
| `0x86f6360b` | ArithmeticException |
| `0x794f618` | NegativeArraySizeException |
| `0xf2f8aa1c` | OutOfMemoryError |
| `0x13523f29` | LinkageError |
| `0xf2985715` | ClassNotFoundException |
| `0x39f3fe57` | IllegalAccessException |
| `0xf217c377` | Error |
| `0x72737f61` | Exception |
| `0x4161c4a6` | Object |
| `0x7772dde3` | String |

All of these are in `java.lang`. Member hashes (methods and fields) are something else (see Open
questions).

**Strings.** `ldc_string` (`0xd0`, H) allocates a **new** `java.lang.String` object every time it
runs. Literals are not interned, so `"a" == "a"` is false. The class id comes from cache `X+0x2c`.
The instance field area gets:

* `+0x18` = pointer to the literal in the class block: `u16 len` followed by bytes (8-bit chars).
* `+0x20` = 0.

The `String` layout is owned by the natives (see `NATIVES.md`).

### Garbage collection

Collection combines reference counting with "stack levels". (H for the mechanism, M for the
intent.)

* **Allocation** (`0x40ab..`): `refcount = 0`, `level = R->depth`. The object is pushed onto the
  all-objects list.
* **Stores of a reference into a local** (`astore*`, via `0x40a250`): if `level == 0`, set
  `level = R->depth`.
* **Stores into an array element, a field or a static** (`0x40a282`): `old->refcount--` and
  `new->refcount++`. The counts are u8 with no overflow check.
* **Return** (`0x40a2bf`, called by every return opcode after `R = R->prev`): every object with
  `level > R->depth` gets `level = 0`. `areturn` first does `level--` on the returned object so it
  survives.
* **`gc()`** (`0x40a30b`) runs **only when an allocation fails**:
  1. Release every object with `refcount == 0 && level == 0` (`0x40a3a7`). Release does the
     following:
     * If `class+0x14` is set, run the method with hash `0x3f52edf8` (probably `finalize()`) in a
       nested run.
     * Drop the super segment.
     * Decrement the referenced objects through the reference fields, recursing when a count
       reaches 0.
  2. Free every block whose type word became 0 (`0x40ad94`).
* **Consequences.**
  * Cycles leak.
  * References that live only on the operand stack, or in a local that was loaded from a field,
    are not counted.
  * A catch handler can receive a non-most-derived segment (see Exceptions).

  A tracing collector that treats the frames' locals and stacks, the statics and the native roots
  as roots frees a superset-safe set. It never frees anything this scheme keeps alive while the
  object is still reachable.

### Statics and class initialisation

`0x40e359(classId)` is called before every static access and invoke, and by `new` (H):

1. Look up `X+0x998 + id*4`. If the value is non-zero, return it; `-1` means "no statics".
2. Otherwise, initialise the classes listed at `class+0x1c` (interfaces or supers) recursively.
3. Allocate the statics block: tag `0x19`, refcount 1, `class+0x18` slots at `+0x10`.
4. Run `<clinit>` (descriptor offset `class+0x1a`) in a nested run.

Static slot `i` is at `statics + 0x10 + 4*i`.

## Exceptions

* **Throw** (`0x409580`, H). Opcodes throw by setting `X+0x1c` to the class-name hash and calling
  `0x409580`.
  * If `X+0x2e & 1` (already constructing an exception) or `& 2` (finalizing) is set, the throw is
    ignored.
  * Otherwise it creates the object (`0x410067(0x99, hash)`, which runs the default constructor
    with descriptor `class+0x2c` in a nested run), does `refcount++`, and unwinds.
  * If creation fails it calls `0x40dccb`: `pc = 0` and `*G+0x24 = 2`, which stops the VM.
* **`athrow`** pops the object (NPE if null), does `refcount++`, and unwinds.
* **Unwind** (`0x409651`, H). For `F = R, R->prev, ...`:
  1. Find the exception table at `class + header.exc_off`: `u16 count`, then 8-byte entries
     `{u16 start, u16 end, u16 handler, u16 catch_classref_off}`.
  2. Match if `start <= F->pc <= end`. **`end` is inclusive**, and `F->pc` already points past the
     throwing opcode or past the call.
  3. If `catch == 0`, it catches everything. Otherwise walk the exception's **segment chain**
     comparing `seg->classid` with `*(u16*)(C+catch)`.
  4. On a match: set `R = F`, `pc = handler`, `refcount--` on the exception, `X+0x1c = 0`, and
     push **the matched segment**, not the original reference.

  The operand stack is **not** reset. Values that sat below the throw point stay on the stack.

  If no frame catches it, `0x407a13` prints "Internal exception occured", shows an error (`0x415031(0x27)`)
  and sets `*G+0x24 = 2`.
* Unwinding does not stop at nested-run boundaries; it walks the whole frame chain (M).
* **Handlers keep going after a throw.** `throw()` returns normally: there is no `longjmp`. Most
  handlers return right after it, but `getfield`/`putfield` external (`0xf9`/`0xfa`) fall through
  after an IllegalAccessException or Error and still do the access, on the handler frame's stack.

## Operand encodings

* `a2` means align pc up to even before reading the u16.
* `@off` means a u16 offset from the class-block base pointing at inline data.
* All targets are absolute block offsets.

| Kind | Encoding |
|---|---|
| `u8` | 1 byte, unaligned (local index) |
| `s8` | 1 byte signed, unaligned |
| `iinc` | u8 local, s8 delta, unaligned (no `wide` exists, so locals ≤ 255 and delta ±127) |
| `s16` | a2, s16 |
| `c32`/`c64` | a2, `@off` of a 4- or 8-byte little-endian constant |
| `br` | a2, u16 absolute target |
| `type` | a2, u16 type word. If `(w & 0xff) == 0x99`, another u16 `@off` of a u16 class id follows. |
| `newarr` | a2, u16 type word. If 0x99, then a2 and `@off` of a u16 class id. |
| `cls` | a2, `@off` of a u16 class id |
| `str` | a2, `@off` of `{u16 len, u8 chars[len]}` |
| `mloc` | a2, `@off` of a 12-byte method descriptor **in the current class**: `{u32 hash, u16 flags, u16 nargs, u16 code_off, u16 ?}`. This is the class's own method-table entry. |
| `mext` | a2, `@off` of `{u32 hash, u16 nargs, u16 @classref}` |
| `floc` | a2, `@off` of a 12-byte field descriptor in the current class: `{u32 hash, u16 flags, u16 type, u16 slot, u16 ?}` |
| `fext` | a2, `@off` of `{u32 hash, u16 type, u16 @classref}` |
| `tsw` | a2 `u16 default`, then a4 `s32 low, s32 high`, then `u16 target[high-low+1]` |
| `lsw` | a2 `u16 default, u16 n`, then a4 `s32 key[n]`, then `u16 target[n]`. The search is linear. |
| `ctsw` | a2 `u16 default, s16 low, s16 high, u16 target[]` |
| `clsw` | a2 `u16 default, u16 n, s16 key[n], u16 target[n]` |
| `mna` | u8 dims (unaligned), then a2 type word and u16 `@classref`. The second word is only present for 0x99. |

Access flags in descriptors use JVM values: `0x1` public, `0x2` private, `0x8` static,
`0x10` final, plus `0x100` native. (M, from the checks in `0xf7`–`0xfa`.)

## Opcode table

The stack column uses JVM notation. `L` means a 2-slot long. The address is the handler.

| Op | Mnemonic | Operands | Stack | Handler | Notes |
|---|---|---|---|---|---|
| 00 | nop | | | 40f070 | |
| 01 | aconst_null | | → 0 | 408a78 | |
| 02–08 | iconst_m1…iconst_5 | | → i | 40b1dc,40b0ec… | |
| 09,0a | lconst_0/1 | | → L | 40cd4c,40cd7b | |
| 10 | bipush | s8 | → i | 40908f | sign-extended |
| 11 | sipush | s16 | → i | 40fc16 | |
| 12 | ldc | c32 | → i | 40cebc | int constants only. Strings use `d0`. |
| 14 | ldc2_w | c64 | → L | 40cf42 | |
| 19 | aload | u8 | → ref | 408aa0 | |
| 2a–2d | aload_0..3 | | → ref | 408b0d… | |
| 2e | iaload | | arr,i → i | 40aecd | null: NPE. Not an array or mask mismatch: **`java.lang.Exception`**. `i<0 \|\| i>=len`: AIOOBE. |
| 2f | laload | | arr,i → L | 40ca2f | mask 0x6e |
| 32 | aaload | | arr,i → ref | 4088b0 | |
| 33 | baload | | arr,i → i | 408eb0 | sign-extends |
| 34 | caload | | arr,i → i | 4090f0 | 1-byte element, zero-extended |
| 35 | saload | | arr,i → i | 40fa40 | |
| 3a | astore | u8 | ref → | 408c66 | marks the GC level (`0x40a250`) |
| 4b–4e | astore_0..3 | | ref → | 408cde… | same |
| 4f | iastore | | arr,i,v → | 40b01c | same checks as the loads |
| 50 | lastore | | arr,i,L → | 40cb8f | |
| 53 | aastore | | arr,i,ref → | 40899e | **no element-type check** (no ArrayStoreException). Updates refcounts. |
| 54 | bastore | | | 408fb9 | stores the low byte |
| 55 | castore | | | 4091f4 | stores the low byte |
| 56 | sastore | | | 40fb44 | |
| 57/58 | pop/pop2 | | | 40f080/40f099 | |
| 59–5e | dup, dup_x1, dup_x2, dup2, dup2_x1, dup2_x2 | | JVM | 409340, 409387, 4093d9, 409442, 409484, 4094f6 | standard |
| 5f | swap | | | 40fc84 | |
| 60/61 | iadd/ladd | | | 40ae82/40c9db | wrapping |
| 64/65 | isub/lsub | | | 40c6fc/40d7eb | |
| 68/69 | imul/lmul | | | 40bc53/40d1e4 | `lmul` uses `__allmul` `0x440b00` |
| 6c | idiv | | a,b → a/b | 40b204 | `b==0` → ArithmeticException. **`MIN_INT/-1` → ArithmeticException too** (JVM gives MIN_INT). Both operands are already popped and nothing is pushed. |
| 6d | ldiv | | | 40cfcf | `b==0` → ArithmeticException. **`MIN/MIN` → ArithmeticException** (buggy check, meant to test −1). `MIN/-1` goes to `__alldiv` (`0x440a50`). |
| 70 | irem | | | 40c457 | same checks as idiv |
| 71 | lrem | | | 40d4c0 | same checks as ldiv, `__allrem` (`0x440b40`) |
| 74/75 | ineg/lneg | | | 40bc9f/40d242 | |
| 78 | ishl | | | 40c532 | count & 31 |
| 79 | lshl | | | 40d5ee | **count not masked**: `__allshl` gives 0 for count ≥ 64 (JVM masks with & 63) |
| 7a | ishr | | | 40c582 | count & 31 |
| 7b | lshr | | | 40d645 | count not masked: ≥ 64 gives the sign fill |
| 7c | iushr | | | 40c747 | count & 31 |
| 7d | lushr | | | 40d83f | count & 63, computed as `(x>>s)+(2L<<~s)` for x<0 (correct) |
| 7e–83 | iand, land, ior, lor, ixor, lxor | | | 40afd1… | |
| 84 | iinc | u8,s8 | | 40bac1 | |
| 85 | i2l | | i → L | 40ae2b | |
| 88 | l2i | | L → i | 40c990 | low word |
| 91 | i2b | | | 40adc0 | |
| 92 | i2c | | | 40ade8 | **`& 0xff`** (8-bit chars) |
| 93 | i2s | | | 40ae0a | |
| 94 | lcmp | | L,L → i | 40cc67 | −1/0/1 |
| 99–9e | ifeq, ifne, iflt, ifge, ifgt, ifle | br | i → | 40b6d9… | |
| 9f–a4 | if_icmpeq, ne, lt, ge, gt, le | br | i,i → | 40b397… | |
| a5/a6 | if_acmpeq/ne | br | | 40b281/40b30c | |
| a7 | goto | br | | 409d72 | |
| a8 | jsr | br | → 0 | 40c8f0 | `R+0x20 = return pc`, pushes a **dummy 0** |
| a9 | ret | (none) | → 0 | 40f9e0 | `pc = R+0x20`, then pushes a dummy 0. It has **no index operand**. In the corpus `jsr` appears only in bogus decodes and `ret` never appears, so both are unused. |
| aa | tableswitch | tsw | i → | 40fd68 | unused in the corpus |
| ab | lookupswitch | lsw | i → | 40d294 | |
| b0 | areturn | | ref → [caller] | 408e23 | level--, pop frame, sweep levels, push |
| b1 | return | | | 40fa1b | |
| bb | new | cls | → ref | 40ecc0 | Allocates the chain and zeroes the fields. Does **not** run `<init>`; the code calls it separately. ClassNotFoundException or OOM on failure. |
| bc | newarray | newarr | n → arr | 40ee4d | One opcode covers primitive and reference arrays (replaces `anewarray`). `n<0` → NegativeArraySizeException. Size is truncated to u16 (see above). |
| be | arraylength | | arr → i | 408bcc | null → NPE. Not an array → `java.lang.Exception`. |
| bf | athrow | | ref → | 408dbd | |
| c0 | checkcast | type | | 4092be | **No check.** It only skips the operand. |
| c1 | instanceof | type | ref → i | 40bcea | For class types: `0x411b6f(obj, class.hash)`. `Object` is special-cased; interfaces come from `+0x1c`. Null pushes 0. **For a non-class type word (array types) it pops and pushes nothing**, which unbalances the stack. |
| c2/c3 | monitorenter/exit | | (no pop!) | 40e900/40e905 | Empty functions: the ref stays on the stack. Unused in the corpus. |
| c5 | multianewarray | mna | n1..nd → arr | 40e90a | Dimensions are read as **s16**. Any negative → NegativeArraySizeException. Builder `0x40ebcd`. |
| c6/c7 | ifnull/ifnonnull | br | | 40ba44/40b9c7 | |
| cc | lookupswitch_s16 | clsw | i → | 40d38d | **new.** Compact form with 16-bit keys. Compared as `(int)(s16)key == v`. |
| cd | tableswitch_s16 | ctsw | i → | 40fcc0 | **new.** Compact form with 16-bit bounds. |
| d0 | ldc_string | str | → String | 40cdaa | **new.** Allocates a fresh String. OOM on failure. |
| d5/d6 | iload/istore | u8 | | 40bb27/40c5d2 | **new numbers.** The int forms do no GC marking. |
| d7/d8 | lload/lstore | u8 | | 40d091/40d69c | |
| d9–e8 | {iload, istore, lload, lstore}_n for n=0..3 | | | 40bb94… | Grouped by n: d9=iload_0, da=istore_0, db=lload_0, dc=lstore_0, dd=iload_1, … e8=lstore_3 |
| e9 | ireturn | | i → [caller] | 40c4d4 | **new number** |
| ea | lreturn | | | 40d588 | **Buggy:** it moves `sp` by only one slot and copies `[sp-4],[sp]` to the caller's `[sp-4],[sp]` without advancing the caller's sp. Unused in the corpus. Implement as a correct `lreturn` and note the difference. |
| ed | invokespecial_local | mloc | this,args → | 40c1fa | Nonvirtual call of a method of the **current** class. NPE if this is null. `thisSeg` = current-class segment. |
| ee | invokevirtual | mext | this,args → | 40c304 | Search the method tables (`class+0x20`) by hash starting at `this`'s segment and walking `+0x10` (`0x40df05`). Not found → LinkageError. |
| ef | invokevirtual_local | mloc | | 40c013 | Code is byte-identical to `ed`. M: the prelinker devirtualised it (final or private). |
| f0 | invokespecial | mext | | 40bf01 | Walk the segments to the one whose class id is the ref's class, then look up by hash in that class only (`0x40e5a2`). Covers `super.m()` and the superclass `<init>`. |
| f1 | invokestatic_local | mloc | args → | 40c7f2 | runs class init first |
| f2 | invokestatic | mext | args → | 40c11d | resolve class, init, hash lookup (`0x40e265`) |
| f3/f4 | getstatic_local/putstatic_local | floc | | 40a01a/40f4fb | |
| f5/f6 | getfield_local/putfield_local | floc | | 409db4/40f0b2 | NPE on a null ref |
| f7/f8 | getstatic/putstatic | fext | | 409b03/40f73f | Field lookup in the class field table `class+0x1e`. A private field → IllegalAccessException. |
| f9/fa | getfield/putfield | fext | | 409840/40f29a | Walks the segments. Private → IllegalAccessException. Static → `java.lang.Error`. Not found → LinkageError. |
| other | — | | | 4102cf | silent 1-byte no-op |

The **field access type switch** is the same in all of `f3`–`fa`, keyed on the field type word:

| Type | Get | Put |
|---|---|---|
| ref or any array (high byte ≠ 0), `0x99` | 32-bit load | updates refcounts |
| short `0x15` | sign-extended 16-bit | stores 32 bits |
| byte `0x90`, boolean `0xd5` | **zero-extended** 8-bit | stores the low byte |
| int `0x59` | 32-bit | 32-bit |
| char `0x50` | zero-extended 8-bit | stores the low byte |
| long `0x6e` | 64-bit | 64-bit |
| any other (float/double) | pushes nothing | |

A `byte` field holding −1 therefore reads back as **255**. The corpus never follows a byte
`getfield` with `i2b`, so games see unsigned bytes. Replicate this.

## Timing behaviour

* **The interpreter never yields.** `0x40dc74` runs until the callback returns (H). A game whose
  callback loops forever freezes the simulator.
* **The host main loop** at `0x439303` in WinMain (`0x438d1e`) is a **blocking** `GetMessageA`
  loop. After each message it calls the idle routine `0x438840`. That routine polls `GetTickCount`
  for an event queue at `0x51e300` (10 entries of 0x90 bytes, due time at `+0x94`) and dispatches
  due events (`0x436f58(0x100, ev)`). It also animates simulator UI state. (H for the structure, M
  for the purpose.)
* **The game tick is a Win32 timer.** `0x435d00` logs `"EXManufSetTimerTick [speed=%d]"`, clamps
  the period to `[10, 60000]` ms, and calls `SetTimer(hwnd, 1, period)` (`0x43f5ba`). It is reached
  from `0x406851`. The tick arrives as `WM_TIMER` with the usual ~10–16 ms Windows granularity. (H)
* **Busy-wait spin loops.** `PeekMessageA` loops with no `Sleep` (Sleep is **not imported at all**)
  call `0x438840` on every empty poll:

  | Loop | Function |
  |---|---|
  | `0x4360a5` | `0x435df0` |
  | `0x436466` | `0x436251` |
  | `0x43659c` | `0x4364c3` |
  | `0x43846f` | `0x43844e` (drain only) |

  The loops run until `*(0x4a20b0)` or `*(0x4a20cc)` is set. `0x4364c3` is reached from natives
  **#85** (`0x425372`) and **#182** (`0x4242b2`) through `0x403d8a`. `0x435df0`/`0x436251` are
  modal UI or wait loops entered from host code (`0x4025f6`, `0x402681`; error dialogs among
  others). **These spin loops are the most likely cause of the 100% CPU.** (M)
* **A dead busy timer.** There is an unreferenced busy-polling timer thread at `0x435861`. It loops
  on `timeGetTime()` and calls a callback every period, started by `0x4357ed`. No code references
  `0x4357ed`, so it is dead code. (H)
* **Design implication.** The VM needs no preemption or instruction budget for compatibility,
  because the original never had one. Deliver ticks, keys and other events as separate synchronous
  callbacks. Replace the waits in natives #85 and #182 (and any modal wait) with a blocking wait
  on the next event or deadline.

## Validation

* `tools/disasm.py class code.4cvp` lists every class. The method table is at `class+0x20`; the
  header is `{max_stack, max_locals, exc_off}` and the code starts at `+6`.
* Sweep run on 2026-11-22 over all `.exn` in the corpus: **8247 methods**, flow-following decode
  (entry plus exception handlers, following branch and switch targets):
  * 2 overlapping instruction starts in total, both inside bogus methods.
  * Undefined opcodes reached only in the bogus methods. These are entries with code offset 0:
    abstract methods, for example in MotoGp's class at `0x7168`.
* The 58 framework blocks in `gensimu.exe`: 335 methods, 0 overlaps, 0 undefined opcodes.
* Corpus opcode counts include `tableswitch_s16` 1185, `lookupswitch_s16` 923, `lookupswitch` 11,
  `tableswitch` 0, `athrow` 0, `monitorenter` 0, `lreturn` 0, `ret` 0, `lshl` 0, `lushr` 0,
  `ldiv` 37, `lmul` 61, `multianewarray` 26.

Sample: BombJack, first class (hash `adc22898`), method `3f52065c` (`()V` family; see the
member-hash note below):

```
-- method 3f52065c flags=0x1 nargs=0 @0x099a max_stack=4 max_locals=3 exc_table=0x0000
  09a0: 2a               aload_0
  09a1: 1048             bipush               72
  09a3: bc9900947f       newarray             ref[] class#33 (@0x7f94)
  09a8: f6002c07         putfield_local       @0x072c slot=120 ref[] hash=82088527 flags=0x0
  09ac: 101a             bipush               26
  09ae: f200287d         invokestatic         @0x7d28 hash=b1c8d60b nargs=1 class#6
  09b2: 4c               astore_1
  09b3: 03               iconst_0
  09b4: e2               istore_2
  09b5: a7c609           goto                 0x09c6
  09b8: 2a               aload_0
  09b9: f52c07           getfield_local       @0x072c slot=120 ref[] ...
  09bc: e1               iload_2
  09bd: 2b               aload_1
  09be: e1               iload_2
  09bf: ee307d           invokevirtual        @0x7d30 hash=d0457739 nargs=1 class#6
  09c2: 53               aastore
  09c3: 840201           iinc                 2 1
  09c6: e1               iload_2
  09c7: 1048             bipush               72
  09c9: a0b809           if_icmpne            0x09b8
  09cc: 01               aconst_null
  09cd: 4c               astore_1
  09ce: b1               return
```

In Java this is `this.f = new X[72]; Y t = Y.m(26); for (i=0;i!=72;i++) this.f[i] = t.n(i); t = null;`.
Note the even-alignment pad bytes (`00` after `bc`, `f6`, `9900`) and the absolute branch targets.

## Open questions

1. **Member hashes.** Method and field hashes are not plain `crc32(name)`. The high half seems to
   encode the descriptor: every `()V` method is `0x3f52xxxx`, int fields `0xd042xxxx`, byte fields
   `0x6e91xxxx`. Two values are known: `<init>()V` = `0x3f52ef2f`, `finalize()` = `0x3f52edf8`.
   Deriving the function would let us name framework members (`CVP.md`).
2. **Class-id assignment and the class table.** How `X+8` (12-byte entries, pointer at `+4`) is
   built from the 4CVP blocks, and how game `class#N` maps onto framework and game classes.
   This is `CVP.md` territory.
3. `X+0x14`: a native status flag that makes the invoking opcode call `throw(X+0x1c)`. Possibly
   "native requested stop or yield". Find its writers.
4. Hash `0xa2c72bf1`, thrown on VM stack overflow (presumably `StackOverflowError`), and the
   stack size `X+0xc`.
5. `0xa931f301`, the float/double wrapper hash, and the unknown fields `u16` at descriptor `+10`
   and class `+0x22..+0x26`.
6. Exact semantics of `multianewarray`'s type word (element type or array type) and of builder
   `0x40ebcd`. 26 uses exist in the corpus, so check them.
7. Which natives #85 and #182 are, and whether ordinary frame pacing (`WM_TIMER` id 1) also runs
   through a spin loop. That would pin down the 100% CPU cause precisely.
8. Behaviour when an exception unwinds past a nested run (`<clinit>`, a host callback, a
   finalizer): the outer frames can catch it inside the inner C loop.
9. Frame `+0x18` (copy of the last-allocated object pointer): purpose unknown, maybe a GC root for
   a freshly allocated object.
10. `0x410067`/`0x4101c4` run the default constructor (`class+0x2c`) for VM-created objects. Check
    whether exceptions get a message.
