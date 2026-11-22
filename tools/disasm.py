#!/usr/bin/env python3
"""Disassemble ExEn VM bytecode (the prelinked, renumbered Java bytecode in `4CVP` blocks).

Usage:
    python tools/disasm.py class <file.4cvp> [block_offset]
        List every 4CVP block in the file (code.4cvp from `exn.py dump` holds one block per class),
        or only the block at block_offset, and disassemble each non-native method.
    python tools/disasm.py range <file> <block_offset> <start> <end>
        Disassemble raw bytes. start/end are offsets *within the class block*, because
        all pcs, branch targets and constant references are relative to the block start.

The opcode table below mirrors docs/re/OPCODES.md (handler table at 0x454498 in gensimu.exe).
Standard library only.

Encoding rules that matter (see OPCODES.md for the evidence):
  * pc is a u16 offset from the class block base (frame field +0x1e, code base at +0x0c).
  * Every 16-bit operand is first aligned to an even pc ("a2"); 32-bit switch tables to 4.
  * Branch targets are absolute u16 offsets in the class block, not relative displacements.
  * Constant-pool style operands are u16 offsets to inline data in the same class block.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------------------------
# Operand kinds
#   ""        no operand
#   "u8"      local index (unaligned byte)
#   "s8"      signed byte immediate (bipush)
#   "iinc"    u8 local, s8 delta (unaligned)
#   "s16"     a2, s16 immediate (sipush)
#   "c32"     a2, u16 offset of a 4-byte constant (ldc)
#   "c64"     a2, u16 offset of an 8-byte constant (ldc2_w)
#   "br"      a2, u16 absolute target
#   "type"    a2, u16 type word; if (word & 0xff) == 0x99 another u16 offset of a u16 class id
#   "newarr"  a2, u16 type word; if (word & 0xff) == 0x99 another a2 u16 offset of a u16 class id
#   "cls"     a2, u16 offset of a u16 class id (new)
#   "str"     a2, u16 offset of a string literal (u16 length + bytes)
#   "mloc"    a2, u16 offset of a 12-byte method descriptor in this class
#   "mext"    a2, u16 offset of an external method ref {u32 hash, u16 nargs, u16 classref off}
#   "floc"    a2, u16 offset of a 12-byte field descriptor in this class
#   "fext"    a2, u16 offset of an external field ref {u32 hash, u16 type, u16 classref off}
#   "tsw"     tableswitch:  a2 u16 default, a4 s32 low, s32 high, u16 targets[high-low+1]
#   "lsw"     lookupswitch: a2 u16 default, u16 n, a4 s32 keys[n], u16 targets[n]
#   "ctsw"    compact tableswitch: a2 u16 default, s16 low, s16 high, u16 targets[]
#   "clsw"    compact lookupswitch: a2 u16 default, u16 n, s16 keys[n], u16 targets[n]
#   "mna"     multianewarray: u8 dims, a2 u16 type word [+ u16 classref off if 0x99]
# ---------------------------------------------------------------------------------------------
OPCODES: dict[int, tuple[str, str]] = {
    0x00: ("nop", ""),
    0x01: ("aconst_null", ""),
    0x02: ("iconst_m1", ""), 0x03: ("iconst_0", ""), 0x04: ("iconst_1", ""),
    0x05: ("iconst_2", ""), 0x06: ("iconst_3", ""), 0x07: ("iconst_4", ""),
    0x08: ("iconst_5", ""),
    0x09: ("lconst_0", ""), 0x0a: ("lconst_1", ""),
    0x10: ("bipush", "s8"),
    0x11: ("sipush", "s16"),
    0x12: ("ldc", "c32"),
    0x14: ("ldc2_w", "c64"),
    0x19: ("aload", "u8"),
    0x2a: ("aload_0", ""), 0x2b: ("aload_1", ""), 0x2c: ("aload_2", ""), 0x2d: ("aload_3", ""),
    0x2e: ("iaload", ""), 0x2f: ("laload", ""), 0x32: ("aaload", ""), 0x33: ("baload", ""),
    0x34: ("caload", ""), 0x35: ("saload", ""),
    0x3a: ("astore", "u8"),
    0x4b: ("astore_0", ""), 0x4c: ("astore_1", ""), 0x4d: ("astore_2", ""), 0x4e: ("astore_3", ""),
    0x4f: ("iastore", ""), 0x50: ("lastore", ""), 0x53: ("aastore", ""), 0x54: ("bastore", ""),
    0x55: ("castore", ""), 0x56: ("sastore", ""),
    0x57: ("pop", ""), 0x58: ("pop2", ""), 0x59: ("dup", ""), 0x5a: ("dup_x1", ""),
    0x5b: ("dup_x2", ""), 0x5c: ("dup2", ""), 0x5d: ("dup2_x1", ""), 0x5e: ("dup2_x2", ""),
    0x5f: ("swap", ""),
    0x60: ("iadd", ""), 0x61: ("ladd", ""), 0x64: ("isub", ""), 0x65: ("lsub", ""),
    0x68: ("imul", ""), 0x69: ("lmul", ""), 0x6c: ("idiv", ""), 0x6d: ("ldiv", ""),
    0x70: ("irem", ""), 0x71: ("lrem", ""), 0x74: ("ineg", ""), 0x75: ("lneg", ""),
    0x78: ("ishl", ""), 0x79: ("lshl", ""), 0x7a: ("ishr", ""), 0x7b: ("lshr", ""),
    0x7c: ("iushr", ""), 0x7d: ("lushr", ""), 0x7e: ("iand", ""), 0x7f: ("land", ""),
    0x80: ("ior", ""), 0x81: ("lor", ""), 0x82: ("ixor", ""), 0x83: ("lxor", ""),
    0x84: ("iinc", "iinc"),
    0x85: ("i2l", ""), 0x88: ("l2i", ""), 0x91: ("i2b", ""), 0x92: ("i2c", ""), 0x93: ("i2s", ""),
    0x94: ("lcmp", ""),
    0x99: ("ifeq", "br"), 0x9a: ("ifne", "br"), 0x9b: ("iflt", "br"), 0x9c: ("ifge", "br"),
    0x9d: ("ifgt", "br"), 0x9e: ("ifle", "br"),
    0x9f: ("if_icmpeq", "br"), 0xa0: ("if_icmpne", "br"), 0xa1: ("if_icmplt", "br"),
    0xa2: ("if_icmpge", "br"), 0xa3: ("if_icmpgt", "br"), 0xa4: ("if_icmple", "br"),
    0xa5: ("if_acmpeq", "br"), 0xa6: ("if_acmpne", "br"),
    0xa7: ("goto", "br"), 0xa8: ("jsr", "br"), 0xa9: ("ret", ""),
    0xaa: ("tableswitch", "tsw"), 0xab: ("lookupswitch", "lsw"),
    0xb0: ("areturn", ""), 0xb1: ("return", ""),
    0xbb: ("new", "cls"), 0xbc: ("newarray", "newarr"), 0xbe: ("arraylength", ""),
    0xbf: ("athrow", ""), 0xc0: ("checkcast", "type"), 0xc1: ("instanceof", "type"),
    0xc2: ("monitorenter", ""), 0xc3: ("monitorexit", ""),
    0xc5: ("multianewarray", "mna"),
    0xc6: ("ifnull", "br"), 0xc7: ("ifnonnull", "br"),
    # ExEn extensions (no JVM equivalent at these numbers)
    0xcc: ("lookupswitch_s16", "clsw"),
    0xcd: ("tableswitch_s16", "ctsw"),
    0xd0: ("ldc_string", "str"),
    0xd5: ("iload", "u8"), 0xd6: ("istore", "u8"), 0xd7: ("lload", "u8"), 0xd8: ("lstore", "u8"),
    0xd9: ("iload_0", ""), 0xda: ("istore_0", ""), 0xdb: ("lload_0", ""), 0xdc: ("lstore_0", ""),
    0xdd: ("iload_1", ""), 0xde: ("istore_1", ""), 0xdf: ("lload_1", ""), 0xe0: ("lstore_1", ""),
    0xe1: ("iload_2", ""), 0xe2: ("istore_2", ""), 0xe3: ("lload_2", ""), 0xe4: ("lstore_2", ""),
    0xe5: ("iload_3", ""), 0xe6: ("istore_3", ""), 0xe7: ("lload_3", ""), 0xe8: ("lstore_3", ""),
    0xe9: ("ireturn", ""), 0xea: ("lreturn", ""),
    0xed: ("invokespecial_local", "mloc"),
    0xee: ("invokevirtual", "mext"),
    0xef: ("invokevirtual_local", "mloc"),
    0xf0: ("invokespecial", "mext"),
    0xf1: ("invokestatic_local", "mloc"),
    0xf2: ("invokestatic", "mext"),
    0xf3: ("getstatic_local", "floc"), 0xf4: ("putstatic_local", "floc"),
    0xf5: ("getfield_local", "floc"), 0xf6: ("putfield_local", "floc"),
    0xf7: ("getstatic", "fext"), 0xf8: ("putstatic", "fext"),
    0xf9: ("getfield", "fext"), 0xfa: ("putfield", "fext"),
}
# Every other byte value dispatches to the empty handler 0x4102cf: a silent 1-byte no-op.

TYPE_NAMES = {0x50: "char", 0x90: "byte", 0x15: "short", 0x59: "int", 0x6e: "long",
              0x99: "ref", 0xd5: "boolean", 0x1a: "float", 0x2f: "double"}

TERMINATORS = {"goto", "areturn", "return", "ireturn", "lreturn", "athrow", "ret",
               "tableswitch", "lookupswitch", "tableswitch_s16", "lookupswitch_s16"}


def _u16(b: bytes, o: int) -> int:
    return struct.unpack_from("<H", b, o)[0]


def _s16(b: bytes, o: int) -> int:
    return struct.unpack_from("<h", b, o)[0]


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def _s32(b: bytes, o: int) -> int:
    return struct.unpack_from("<i", b, o)[0]


def _a2(x: int) -> int:
    return (x + 1) & ~1


def _a4(x: int) -> int:
    return (x + 3) & ~3


def _type(w: int) -> str:
    t = TYPE_NAMES.get(w & 0xFF, f"tag{w & 0xff:#x}")
    return f"{t}[]" if w & 0xFF00 else t


def decode(blk: bytes, pc: int) -> tuple[int, str, str, list[int]]:
    """Decode one instruction at pc. Returns (next_pc, mnemonic, operand text, branch targets)."""
    op = blk[pc]
    name, kind = OPCODES.get(op, (f"undefined_{op:02x}(nop)", ""))
    p = pc + 1
    txt, tg = "", []
    if kind == "u8":
        txt, p = str(blk[p]), p + 1
    elif kind == "s8":
        txt, p = str(struct.unpack_from("<b", blk, p)[0]), p + 1
    elif kind == "iinc":
        txt, p = f"{blk[p]} {struct.unpack_from('<b', blk, p + 1)[0]}", p + 2
    elif kind == "s16":
        p = _a2(p); txt, p = str(_s16(blk, p)), p + 2
    elif kind in ("c32", "c64"):
        p = _a2(p); off = _u16(blk, p); p += 2
        if kind == "c32":
            v = _s32(blk, off); txt = f"@{off:#06x} = {v} ({v & 0xffffffff:#x})"
        else:
            v = struct.unpack_from("<q", blk, off)[0]; txt = f"@{off:#06x} = {v}L"
    elif kind == "br":
        p = _a2(p); t = _u16(blk, p); p += 2; txt, tg = f"{t:#06x}", [t]
    elif kind in ("type", "newarr"):
        p = _a2(p); w = _u16(blk, p); p += 2
        txt = _type(w) + ("[]" if kind == "newarr" and not w & 0xFF00 else "")
        if w & 0xFF == 0x99:
            if kind == "newarr":
                p = _a2(p)
            off = _u16(blk, p); p += 2
            txt += f" class#{_u16(blk, off)} (@{off:#06x})"
    elif kind == "cls":
        p = _a2(p); off = _u16(blk, p); p += 2
        txt = f"class#{_u16(blk, off)} (@{off:#06x})"
    elif kind == "str":
        p = _a2(p); off = _u16(blk, p); p += 2
        n = _u16(blk, off)
        s = blk[off + 2: off + 2 + n].decode("latin-1")
        txt = f"@{off:#06x} {s!r}"
    elif kind == "mloc":
        p = _a2(p); off = _u16(blk, p); p += 2
        h, fl, na, co = struct.unpack_from("<IHHH", blk, off)
        txt = (f"@{off:#06x} hash={h:08x} flags={fl:#x} nargs={na} "
               f"{'native' if fl & 0x100 else f'code={co:#06x}'}")
    elif kind == "mext":
        p = _a2(p); off = _u16(blk, p); p += 2
        h, na, cr = struct.unpack_from("<IHH", blk, off)
        txt = f"@{off:#06x} hash={h:08x} nargs={na} class#{_u16(blk, cr)}"
    elif kind == "floc":
        p = _a2(p); off = _u16(blk, p); p += 2
        h, fl, ty, slot = struct.unpack_from("<IHHH", blk, off)
        txt = f"@{off:#06x} slot={slot} {_type(ty)} hash={h:08x} flags={fl:#x}"
    elif kind == "fext":
        p = _a2(p); off = _u16(blk, p); p += 2
        h, ty, cr = struct.unpack_from("<IHH", blk, off)
        txt = f"@{off:#06x} {_type(ty)} hash={h:08x} class#{_u16(blk, cr)}"
    elif kind == "tsw":
        p = _a2(p); d = _u16(blk, p); p = _a4(p + 2)
        lo, hi = _s32(blk, p), _s32(blk, p + 4); p += 8
        n = hi - lo + 1 if 0 <= hi - lo < 4096 else 0
        ts = [_u16(blk, p + 2 * i) for i in range(n)]; p += 2 * n
        tg = [d] + ts
        txt = f"default={d:#06x} {lo}..{hi}: " + " ".join(f"{t:#06x}" for t in ts)
    elif kind == "lsw":
        b = _a2(p); d = _u16(blk, b); n = _u16(blk, b + 2); k = _a4(b + 4)
        keys = [_s32(blk, k + 4 * i) for i in range(n)]
        t0 = k + 4 * n
        ts = [_u16(blk, t0 + 2 * i) for i in range(n)]
        p = t0 + 2 * n; tg = [d] + ts
        txt = f"default={d:#06x} " + " ".join(f"{a}:{t:#06x}" for a, t in zip(keys, ts))
    elif kind == "ctsw":
        b = _a2(p); d = _u16(blk, b); lo, hi = _s16(blk, b + 2), _s16(blk, b + 4)
        n = hi - lo + 1 if 0 <= hi - lo < 4096 else 0
        ts = [_u16(blk, b + 6 + 2 * i) for i in range(n)]
        p = b + 6 + 2 * n; tg = [d] + ts
        txt = f"default={d:#06x} {lo}..{hi}: " + " ".join(f"{t:#06x}" for t in ts)
    elif kind == "clsw":
        b = _a2(p); d = _u16(blk, b); n = _u16(blk, b + 2)
        keys = [_s16(blk, b + 4 + 2 * i) for i in range(n)]
        ts = [_u16(blk, b + 4 + 2 * n + 2 * i) for i in range(n)]
        p = b + 4 + 4 * n; tg = [d] + ts
        txt = f"default={d:#06x} " + " ".join(f"{a}:{t:#06x}" for a, t in zip(keys, ts))
    elif kind == "mna":
        dims = blk[p]; p = _a2(p + 1); w = _u16(blk, p); p += 2
        txt = f"dims={dims} {_type(w)}"
        if w & 0xFF == 0x99:
            off = _u16(blk, p); p += 2
            txt += f" class#{_u16(blk, off)}"
    return p, name, txt, tg


def disasm_range(blk: bytes, start: int, end: int, out=sys.stdout) -> None:
    pc = start
    while pc < end:
        try:
            nxt, name, txt, _ = decode(blk, pc)
        except (struct.error, IndexError):
            print(f"  {pc:04x}: <truncated>", file=out)
            return
        raw = blk[pc:nxt].hex()
        if len(raw) > 16:
            raw = raw[:14] + ".."
        print(f"  {pc:04x}: {raw:<16} {name:<20} {txt}", file=out)
        pc = nxt


# ---- 4CVP class block (layout as used by the interpreter; see OPCODES.md "Class block") -----

def blocks(data: bytes) -> list[int]:
    out, i = [], 0
    while True:
        i = data.find(b"4CVP", i)
        if i < 0:
            return out
        out.append(i)
        i += 4


def methods(blk: bytes) -> list[tuple[int, int, int, int, int]]:
    """Method table at class+0x20: u16 count, aligned-4 entries of 12 bytes
    {u32 hash, u16 flags, u16 nargs, u16 header_off, u16 ?}."""
    mt = _u16(blk, 0x20)
    if not mt:
        return []
    n = _u16(blk, mt)
    e = _a4(mt + 2)
    res = []
    for _ in range(n):
        h, fl, na, off, x = struct.unpack_from("<IHHHH", blk, e)
        res.append((h, fl, na, off, x))
        e = _a4(e + 12)
    return res


def disasm_class(data: bytes, base: int, out=sys.stdout) -> None:
    size = _u16(data, base + 4)
    blk = data[base: base + max(size, 0x30)]
    print(f"== class block @{base:#x} size={size:#x} hash={_u32(blk, 0xc):08x}", file=out)
    ms = methods(blk)
    heads = sorted(m[3] for m in ms if not m[1] & 0x100 and m[3])
    for h, fl, na, off, _x in ms:
        if off == 0:
            print(f"-- method {h:08x} flags={fl:#x} nargs={na} (no code: abstract)", file=out)
            continue
        if fl & 0x100:
            print(f"-- method {h:08x} flags={fl:#x} nargs={na} NATIVE "
                  f"index={_u32(blk, off)}", file=out)
            continue
        ms_, ml, exc = struct.unpack_from("<HHH", blk, off)
        later = [x for x in heads if x > off]
        end = min(later) if later else min(size, len(blk))
        print(f"-- method {h:08x} flags={fl:#x} nargs={na} @{off:#06x} "
              f"max_stack={ms_} max_locals={ml} exc_table={exc:#06x}", file=out)
        if exc:
            cnt = _u16(blk, exc)
            for i in range(cnt):
                s, e2, hnd, ct = struct.unpack_from("<HHHH", blk, exc + 2 + 8 * i)
                cls = f"class#{_u16(blk, ct)}" if ct else "any"
                print(f"   try {s:#06x}..{e2:#06x} -> {hnd:#06x} catch {cls}", file=out)
        end = min(end, exc) if exc > off else end
        disasm_range(blk, off + 6, end, out)


def main(argv: list[str]) -> int:
    if len(argv) >= 2 and argv[0] == "class":
        data = Path(argv[1]).read_bytes()
        bases = [int(argv[2], 0)] if len(argv) > 2 else blocks(data)
        for b in bases:
            disasm_class(data, b)
        return 0
    if len(argv) == 5 and argv[0] == "range":
        data = Path(argv[1]).read_bytes()
        base = int(argv[2], 0)
        disasm_range(data[base:], int(argv[3], 0), int(argv[4], 0))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
