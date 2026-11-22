#!/usr/bin/env python3
"""Inspect ExEn `4CVP` prelinked class images (see docs/re/CVP.md).

Usage:
    python tools/cvp.py info  <file> [...]          # one line per class
    python tools/cvp.py dump  <file> [--code]       # classes, fields, methods, refs
    python tools/cvp.py fw    <gensimu.exe> <outdir> # extract framework class blocks by name
    python tools/cvp.py check <file> [...]          # parse everything, report anomalies
    python tools/cvp.py hash  <name> [<descriptor>] # crc32 class hash / member hash

<file> may be a `.exn` game, a raw `4CVP` chunk (e.g. code.4cvp from tools/exn.py dump),
or gensimu.exe itself (its framework blob is located automatically).

Game classes are numbered after the framework classes (54 in the vendored gensimu.exe),
and their class references are global indices into that combined table. To print
framework names for those references, the framework is loaded from
vendor/gensimu/gensimu.exe (override with --fw <gensimu.exe>, disable with --fw none).

Standard library only.
"""
from __future__ import annotations

import re
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path

MAGIC = b"4CVP"
REPO = Path(__file__).resolve().parent.parent
DEFAULT_FW = REPO / "vendor" / "gensimu" / "gensimu.exe"

# Class flags (header +0x14). Java access flags plus In-Fusio extras.
ACC_INTERFACE = 0x0200
ACC_NATIVE = 0x0100

# Type codes (field record +6, debug signature records). Low byte = base type,
# high byte = array dimensions. 0x99 is followed by a u16 class-ref offset.
BASE_TYPES = {0x00: "V", 0x15: "S", 0x55: "C", 0x59: "I", 0x6E: "J", 0x90: "B", 0xD5: "Z",
              0xD0: "Z", 0x99: "L"}   # 0xd0 only seen as an array element ([Z = 0x1d0)

FLAG_NAMES = [(0x0001, "public"), (0x0002, "private"), (0x0004, "protected"),
              (0x0008, "static"), (0x0010, "final"), (0x0020, "synchronized"),
              (0x0040, "volatile"), (0x0080, "transient"), (0x0100, "native"),
              (0x0200, "interface"), (0x0400, "abstract")]


def crc16(data: bytes) -> int:
    """CRC-16/MCRF4XX: reflected poly 0x8408, init 0xFFFF, no final xor."""
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c


def class_hash(name: str) -> int:
    """Class identity: standard CRC-32 of the dotted name, e.g. 'exen.Graphics'."""
    return zlib.crc32(name.encode("latin-1"))


def member_hash(name: str, descriptor: str) -> int:
    """Field/method identity: crc16(JVM descriptor) << 16 | crc16(name)."""
    return (crc16(descriptor.encode("latin-1")) << 16) | crc16(name.encode("latin-1"))


CLASS_FLAG_NAMES = [(0x0001, "public"), (0x0010, "final"), (0x0020, "super"),
                    (0x0200, "interface"), (0x0400, "abstract")]


def flags_str(f: int, method: bool = False, cls: bool = False) -> str:
    names = CLASS_FLAG_NAMES if cls else FLAG_NAMES
    out = [n for bit, n in names if f & bit and not (not method and bit == 0x20)]
    rest = f & ~sum(b for b, _ in names)
    if rest:
        out.append(f"{rest:#x}")
    return " ".join(out) or "-"


def _u16(b: bytes, o: int) -> int:
    return struct.unpack_from("<H", b, o)[0]


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def _align4(o: int) -> int:
    return (o + 3) & ~3


# --------------------------------------------------------------------------- model


@dataclass
class Field:
    off: int
    hash: int
    flags: int
    type: int
    slot: int
    name: str | None = None
    type_ref: int = 0          # debug: class-ref offset of the field's class type


@dataclass
class Handler:
    start: int
    end: int                   # inclusive
    handler: int
    catch_ref: int             # 0 = catch any


@dataclass
class Method:
    off: int
    hash: int
    flags: int
    argc: int                  # argument words, excluding `this`; long = 2
    code_off: int
    native_index: int | None = None
    max_stack: int = 0
    max_locals: int = 0
    exc_off: int = 0
    handlers: list[Handler] = field(default_factory=list)
    code_end: int = 0          # estimated (next known structure)
    name: str | None = None
    descriptor: str | None = None
    throws: list[int] = field(default_factory=list)


@dataclass
class Ref:
    off: int
    hash: int
    aux: int                   # u16 at +4: target argc (method ref) / type code (field ref)
    class_ref: int             # block offset of a class-ref entry


@dataclass
class ClassBlock:
    index: int                 # global class index
    pos: int                   # offset of the block in its container
    raw: bytes
    hdr: dict[str, int]
    name: str | None
    fields: list[Field]
    methods: list[Method]
    method_refs: list[Ref]
    field_refs: list[Ref]
    class_refs: list[tuple[int, int]]   # (offset, value)
    interfaces: list[int]               # class-ref offsets

    @property
    def is_interface(self) -> bool:
        return bool(self.hdr["flags"] & ACC_INTERFACE)

    def ref_index(self, ref_off: int) -> int:
        """Global class index stored at a class-ref offset."""
        return _u16(self.raw, ref_off)


def split_blocks(chunk: bytes) -> list[tuple[int, bytes]]:
    """Walk a 4CVP image exactly like gensimu's loaders (0x406fc8 / 0x40713f)."""
    out = []
    p = 0
    while p < len(chunk):
        if chunk[p:p + 4] != MAGIC:
            raise ValueError(f"expected 4CVP at {p:#x}")
        size = _u16(chunk, p + 4)
        if size < 0x28 or p + size > len(chunk):
            raise ValueError(f"bad block size {size:#x} at {p:#x}")
        out.append((p, chunk[p:p + size]))
        p = _align4(p + size)
        while p < len(chunk) and chunk[p:p + 4] != MAGIC:
            p += 4
    return out


def _table(b: bytes, off: int) -> tuple[int, int]:
    """u16 count at off, entries start at the next 4-byte boundary."""
    return _u16(b, off), _align4(off + 2)


def _dstr(b: bytes, off: int) -> str:
    n = _u16(b, off)
    return b[off + 2:off + 2 + n].decode("latin-1")


def parse_class(raw: bytes, index: int, pos: int = 0) -> ClassBlock:
    b = raw
    if b[:4] != MAGIC:
        raise ValueError("not a 4CVP block")
    h = {
        "size": _u16(b, 4), "u06": _u16(b, 6), "u08": _u32(b, 8), "hash": _u32(b, 0xC),
        "u10": _u32(b, 0x10), "flags": _u16(b, 0x14), "name_off": _u16(b, 0x16),
        "nstatics": _u16(b, 0x18), "clinit": _u16(b, 0x1A), "ifaces": _u16(b, 0x1C),
        "fields": _u16(b, 0x1E), "methods": _u16(b, 0x20), "classrefs": _u16(b, 0x22),
        "fieldrefs": _u16(b, 0x24), "methodrefs": _u16(b, 0x26),
    }
    interface = bool(h["flags"] & ACC_INTERFACE)
    if interface:      # interfaces stop at +0x28 (no super / instance layout)
        h.update(super=0, ninst=0, ctor=0, hdr_len=0x28)
    else:
        h.update(super=_u16(b, 0x28), ninst=_u16(b, 0x2A), ctor=_u16(b, 0x2C), hdr_len=0x2E)
    name = _dstr(b, h["name_off"]) if h["name_off"] else None

    fields: list[Field] = []
    if h["fields"]:
        n, p = _table(b, h["fields"])
        for i in range(n):
            o = p + 12 * i
            hh, fl, ty, slot = struct.unpack_from("<IHHH", b, o)
            fields.append(Field(o, hh, fl, ty, slot))
        if name:   # debug: (type class-ref, name offset) pairs from end-of-table - 2
            d = p + 12 * n - 2
            for i, f in enumerate(fields):
                f.type_ref, no = struct.unpack_from("<HH", b, d + 4 * i)
                f.name = _dstr(b, no)

    methods: list[Method] = []
    if h["methods"]:
        n, p = _table(b, h["methods"])
        for i in range(n):
            o = p + 12 * i
            hh, fl, argc, co = struct.unpack_from("<IHHH", b, o)
            m = Method(o, hh, fl, argc, co)
            if fl & ACC_NATIVE:
                m.native_index = _u32(b, co)
            elif co:
                m.max_stack, m.max_locals, m.exc_off = struct.unpack_from("<HHH", b, co)
                if m.exc_off:
                    cnt = _u16(b, m.exc_off)
                    for j in range(cnt):
                        m.handlers.append(Handler(*struct.unpack_from("<4H", b, m.exc_off + 2 + 8 * j)))
            methods.append(m)
        if name:   # debug: u16 signature-record offsets from end-of-table - 2
            d = p + 12 * n - 2
            for i, m in enumerate(methods):
                _parse_sig(b, _u16(b, d + 2 * i), m)

    def refs(off: int) -> list[Ref]:
        if not off:
            return []
        n, p = _table(b, off)
        return [Ref(p + 8 * i, *struct.unpack_from("<IHH", b, p + 8 * i)) for i in range(n)]

    class_refs = []
    if h["classrefs"]:
        n, p = _table(b, h["classrefs"])
        class_refs = [(p + 4 * i, _u32(b, p + 4 * i)) for i in range(n)]
    ifaces = []
    if h["ifaces"]:
        n = _u16(b, h["ifaces"])
        ifaces = list(struct.unpack_from(f"<{n}H", b, h["ifaces"] + 2))

    cls = ClassBlock(index, pos, raw, h, name, fields, methods, refs(h["methodrefs"]),
                     refs(h["fieldrefs"]), class_refs, ifaces)
    _estimate_code_ends(cls)
    return cls


def _parse_sig(b: bytes, so: int, m: Method) -> None:
    no, throws = struct.unpack_from("<HH", b, so)
    m.name = _dstr(b, no)
    if throws:
        m.throws = list(struct.unpack_from(f"<{_u16(b, throws)}H", b, throws + 2))
    o = so + 4
    ret, o = _read_type(b, o)
    nargs = _u16(b, o)
    o += 2
    args = []
    for _ in range(nargs):
        a, o = _read_type(b, o)
        args.append(a)
    m.descriptor = (ret, args)   # resolved to a string once class names are known


def _read_type(b: bytes, o: int) -> tuple[tuple[int, int], int]:
    t = _u16(b, o)
    o += 2
    ref = 0
    if t & 0xFF == 0x99:
        ref = _u16(b, o)
        o += 2
    return (t, ref), o


def _estimate_code_ends(cls: ClassBlock) -> None:
    marks = {len(cls.raw)}
    h = cls.hdr
    for k in ("fields", "methods", "classrefs", "fieldrefs", "methodrefs", "ifaces"):
        if h[k]:
            marks.add(h[k])
    for m in cls.methods:
        if m.code_off:
            marks.add(m.code_off)
        if m.exc_off:
            marks.add(m.exc_off)
    for m in cls.methods:
        if m.code_off and m.native_index is None:
            m.code_end = min(x for x in marks if x > m.code_off)


# --------------------------------------------------------------------------- containers


def find_fw_blob(exe: bytes) -> tuple[int, bytes]:
    """Locate the framework image in gensimu.exe.

    0x40713f does `mov [ebp-0xc], <len>; mov [ebp-0x10], <VA>` (c7 45 f4 / c7 45 f0)
    and walks that range. Find that instruction pair and map the VA through the PE
    section table."""
    sections = _pe_sections(exe)
    for m in re.finditer(rb"\xc7\x45\xf4(....)\xc7\x45\xf0(....)", exe, re.S):
        length, va = struct.unpack("<II", m.group(1) + m.group(2))
        off = _va_to_off(sections, va)
        if off is not None and exe[off:off + 4] == MAGIC:
            return va, exe[off:off + length]
    raise ValueError("framework 4CVP blob not found")


def _pe_sections(exe: bytes) -> list[tuple[int, int, int, int]]:
    pe = _u32(exe, 0x3C)
    nsec = _u16(exe, pe + 6)
    opt = _u16(exe, pe + 20)
    base = _u32(exe, pe + 24 + 28)
    out = []
    for i in range(nsec):
        s = pe + 24 + opt + 40 * i
        vsize, rva, rsize, roff = struct.unpack_from("<IIII", exe, s + 8)
        out.append((base + rva, max(vsize, rsize), roff, rsize))
    return out


def _va_to_off(sections, va: int) -> int | None:
    for sva, size, roff, rsize in sections:
        if sva <= va < sva + min(size, rsize):
            return roff + va - sva
    return None


def load_image(path: str | Path) -> tuple[str, bytes]:
    """Return (kind, 4CVP image) for a .exn, a raw chunk or gensimu.exe."""
    data = Path(path).read_bytes()
    if data[:4] == MAGIC:
        return "chunk", data
    if data[:2] == b"MZ":
        return "framework", find_fw_blob(data)[1]
    if data[:4] == b"NEXE":
        (n,) = struct.unpack_from("<I", data, 0x34)
        off = _u32(data, 0x38 + 4 * n)
        if data[off:off + 4] != MAGIC:
            raise ValueError(f"{path}: code chunk at {off:#x} lacks 4CVP")
        return "game", data[off:]
    raise ValueError(f"{path}: not a .exn, 4CVP chunk or PE file")


def parse_image(img: bytes, first_index: int = 0) -> list[ClassBlock]:
    return [parse_class(raw, first_index + i, pos)
            for i, (pos, raw) in enumerate(split_blocks(img))]


class Universe:
    """Framework + optional game classes, indexed by global class index."""

    def __init__(self, fw: list[ClassBlock] | None):
        self.fw = fw or []
        self.classes: dict[int, ClassBlock] = {c.index: c for c in self.fw}

    def add(self, classes: list[ClassBlock]) -> None:
        for c in classes:
            self.classes[c.index] = c

    def class_name(self, idx: int) -> str:
        c = self.classes.get(idx)
        if c is None:
            return f"<class #{idx}?>"
        return c.name or f"class#{idx}"

    def ref_name(self, cls: ClassBlock, ref_off: int) -> str:
        if not ref_off:
            return "-"
        return self.class_name(cls.ref_index(ref_off))

    def member_name(self, idx: int, h: int, kind: str) -> str:
        c = self.classes.get(idx)
        seen = set()
        while c is not None and c.index not in seen:   # walk superclasses
            seen.add(c.index)
            for m in (c.methods if kind == "m" else c.fields):
                if m.hash == h and m.name:
                    return (m.name if c.index == idx else f"{c.name}.{m.name}")
            c = self.classes.get(c.ref_index(c.hdr["super"])) if c.hdr["super"] else None
        return f"#{h:08x}"

    def type_str(self, cls: ClassBlock, t: int, ref: int, desc_crc: int | None = None) -> str:
        """Render a type code. Without debug info, an object type's class can often be
        recovered from the descriptor half of the member hash (desc_crc)."""
        base, dims = t & 0xFF, t >> 8
        if base == 0x99:
            if ref:
                s = "L" + self.ref_name(cls, ref).replace(".", "/") + ";"
            else:
                s = self.guess_object_type(dims, desc_crc) or "L?;"
                return s
        else:
            s = BASE_TYPES.get(base, f"?{base:02x}")
        return "[" * dims + s

    def guess_object_type(self, dims: int, desc_crc: int | None) -> str | None:
        if desc_crc is None:
            return None
        for c in self.classes.values():
            if c.name:
                d = "[" * dims + "L" + c.name.replace(".", "/") + ";"
                if crc16(d.encode("latin-1")) == desc_crc:
                    return d
        return None

    def descriptor(self, cls: ClassBlock, m: Method) -> str | None:
        if m.descriptor is None:
            return None
        (rt, rr), args = m.descriptor
        return ("(" + "".join(self.type_str(cls, t, r) for t, r in args) + ")"
                + self.type_str(cls, rt, rr))


def load_fw(spec: str | None) -> list[ClassBlock] | None:
    if spec == "none":
        return None
    path = Path(spec) if spec else DEFAULT_FW
    if not path.exists():
        return None
    return parse_image(find_fw_blob(path.read_bytes())[1])


def open_file(path: str, fw_spec: str | None) -> tuple[Universe, list[ClassBlock]]:
    kind, img = load_image(path)
    if kind == "framework":
        classes = parse_image(img)
        return Universe(classes), classes
    fw = load_fw(fw_spec)
    uni = Universe(fw)
    classes = parse_image(img, len(uni.fw) if fw else 54)
    uni.add(classes)
    return uni, classes


# --------------------------------------------------------------------------- commands


def cmd_info(paths: list[str], fw_spec: str | None) -> None:
    for path in paths:
        uni, classes = open_file(path, fw_spec)
        print(f"{path}: {len(classes)} classes")
        for c in classes:
            h = c.hdr
            sup = uni.ref_name(c, h["super"]) if h["super"] else "-"
            print(f"  #{c.index:<3} @{c.pos:#07x} size={h['size']:#06x} hash={h['hash']:08x} "
                  f"{c.name or '?':28} flags={h['flags']:#06x} super={sup} "
                  f"fields={len(c.fields)} methods={len(c.methods)} "
                  f"statics={h['nstatics']} inst={h['ninst']}")


def cmd_dump(path: str, fw_spec: str | None, show_code: bool) -> None:
    uni, classes = open_file(path, fw_spec)
    for c in classes:
        h = c.hdr
        print(f"class #{c.index} {c.name or '?'}  hash={h['hash']:08x}  @{c.pos:#x} "
              f"size={h['size']:#x}")
        print(f"  flags {h['flags']:#06x} ({flags_str(h['flags'], cls=True)})  "
              f"u06={h['u06']:#06x} "
              f"u08={h['u08']:#x} u10={h['u10']:#x}")
        if not c.is_interface:
            print(f"  super {uni.ref_name(c, h['super']) if h['super'] else '-'}  "
                  f"instance slots {h['ninst']}  default ctor @{h['ctor']:#x}")
        print(f"  statics {h['nstatics']}  <clinit> @{h['clinit']:#x}")
        if c.interfaces:
            print("  implements " + ", ".join(uni.ref_name(c, r) for r in c.interfaces))
        print(f"  class refs ({len(c.class_refs)}): " + ", ".join(
            f"{o:#x}={uni.class_name(v & 0xFFFF)}" for o, v in c.class_refs))
        for f in c.fields:
            t = uni.type_str(c, f.type, f.type_ref, f.hash >> 16)
            nm = f.name or f"#{f.hash:08x}"
            kind = "static" if f.flags & 0x8 else "inst"
            print(f"  field  @{f.off:#06x} {nm:24} {t:28} {kind} slot {f.slot:<3} "
                  f"flags {f.flags:#06x} ({flags_str(f.flags)})")
        for m in c.methods:
            nm = m.name or f"#{m.hash:08x}"
            if not m.name and h["super"]:    # an override keeps the overridden name's hash
                guess = uni.member_name(c.ref_index(h["super"]), m.hash, "m")
                if not guess.startswith("#"):
                    nm = f"{nm}={guess.rsplit('.', 1)[-1]}"
            d = uni.descriptor(c, m) or ""
            if m.native_index is not None:
                body = f"native #{m.native_index}"
            elif m.code_off:
                body = (f"code @{m.code_off:#x}..{m.code_end:#x} stack {m.max_stack} "
                        f"locals {m.max_locals}")
            else:
                body = "abstract"
            print(f"  method @{m.off:#06x} {nm + d:44} argc {m.argc:<2} {body}  "
                  f"flags {m.flags:#06x} ({flags_str(m.flags, True)})")
            for x in m.handlers:
                cat = uni.ref_name(c, x.catch_ref) if x.catch_ref else "any"
                print(f"           try {x.start:#x}..{x.end:#x} -> {x.handler:#x} catch {cat}")
            if m.throws:
                print("           throws " + ", ".join(uni.ref_name(c, r) for r in m.throws))
            if show_code and m.native_index is None and m.code_off:
                code = c.raw[m.code_off + 6:m.code_end]
                for i in range(0, len(code), 32):
                    print(f"           {m.code_off + 6 + i:#06x}: {code[i:i + 32].hex(' ')}")
        for label, refs, kind in (("mref", c.method_refs, "m"), ("fref", c.field_refs, "f")):
            for r in refs:
                tgt = c.ref_index(r.class_ref)
                print(f"  {label}   @{r.off:#06x} {uni.class_name(tgt)}."
                      f"{uni.member_name(tgt, r.hash, kind)}  hash={r.hash:08x}")
        print()


def cmd_fw(exe_path: str, outdir: str) -> None:
    va, blob = find_fw_blob(Path(exe_path).read_bytes())
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    classes = parse_image(blob)
    (out / "framework.4cvp").write_bytes(blob)
    for c in classes:
        (out / f"{c.index:02}_{c.name}.4cvp").write_bytes(c.raw)
    print(f"framework blob VA {va:#x}, {len(blob):#x} bytes, {len(classes)} classes -> {out}")


def check_classes(uni: Universe, classes: list[ClassBlock]) -> list[str]:
    """Consistency checks; returns a list of anomaly strings."""
    bad = []
    total = len(uni.classes)
    for c in classes:
        h = c.hdr
        tag = f"#{c.index} {c.name or ''}".strip()
        if c.name and class_hash(c.name) != h["hash"]:
            bad.append(f"{tag}: class hash mismatch")
        for o, v in c.class_refs:
            if (v & 0xFFFF) >= total:
                bad.append(f"{tag}: class ref {o:#x} -> {v} out of range")
        for f in c.fields:
            if f.name and crc16(f.name.encode()) != f.hash & 0xFFFF:
                bad.append(f"{tag}: field {f.name} name hash mismatch")
        for m in c.methods:
            if m.name:
                d = uni.descriptor(c, m)
                if member_hash(m.name, d) != m.hash:
                    bad.append(f"{tag}: method {m.name}{d} hash mismatch")
            if m.native_index is None and m.code_off and m.max_locals < m.argc:
                bad.append(f"{tag}: method {m.hash:08x} locals {m.max_locals} < argc {m.argc}")
        for kind, refs in (("m", c.method_refs), ("f", c.field_refs)):
            for r in refs:
                tgt = uni.classes.get(c.ref_index(r.class_ref))
                if tgt is None:
                    bad.append(f"{tag}: ref {r.off:#x} to unknown class")
                    continue
                pool = tgt.methods if kind == "m" else tgt.fields
                if not any(x.hash == r.hash for x in pool):
                    bad.append(f"{tag}: {kind}ref {r.hash:08x} not declared in "
                               f"{tgt.name or tgt.index}")
    return bad


def cmd_check(paths: list[str], fw_spec: str | None) -> int:
    rc = 0
    for path in paths:
        try:
            uni, classes = open_file(path, fw_spec)
            bad = check_classes(uni, classes)
        except Exception as e:  # noqa: BLE001 - report and continue the sweep
            print(f"{path}: ERROR {type(e).__name__}: {e}")
            rc = 1
            continue
        nm = sum(len(c.methods) for c in classes)
        print(f"{path}: {len(classes)} classes, {nm} methods, {len(bad)} anomalies")
        for b in bad:
            print("   " + b)
    return rc


def main(argv: list[str]) -> int:
    fw_spec = None
    if "--fw" in argv:
        i = argv.index("--fw")
        fw_spec = argv[i + 1]
        del argv[i:i + 2]
    show_code = "--code" in argv
    argv = [a for a in argv if a != "--code"]
    if len(argv) >= 2 and argv[0] == "info":
        cmd_info(argv[1:], fw_spec)
    elif len(argv) == 2 and argv[0] == "dump":
        cmd_dump(argv[1], fw_spec, show_code)
    elif len(argv) == 3 and argv[0] == "fw":
        cmd_fw(argv[1], argv[2])
    elif len(argv) >= 2 and argv[0] == "check":
        return cmd_check(argv[1:], fw_spec)
    elif len(argv) in (2, 3) and argv[0] == "hash":
        if len(argv) == 2:
            print(f"class {argv[1]}: {class_hash(argv[1]):08x}  crc16 {crc16(argv[1].encode()):04x}")
        else:
            print(f"member {argv[1]}{argv[2]}: {member_hash(argv[1], argv[2]):08x}")
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
