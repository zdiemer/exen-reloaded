#!/usr/bin/env python3
"""Inspect ExEn `.exn` game containers (NEXE format).

Usage:
    python tools/exn.py info  <game.exn> [...]   # header + resource table summary
    python tools/exn.py dump  <game.exn> <outdir> # extract resources and the 4CVP code chunk

Layout (little-endian), as understood so far -- see docs/FORMAT.md:
    0x00  char[4]  "NEXE"
    0x04  u32      0x80 (constant across the known corpus)
    0x08  u16[6]   three (a, b) pairs, constant across the known corpus
    0x14  char[32] main class name, NUL padded (e.g. "Worms.GameTopLevel")
    0x34  u32      N = resource count
    0x38  u32[N+1] absolute offsets; offsets[i]..offsets[i+1] is resource i,
                   offsets[N] is the start of the 4CVP code chunk
    ....  u8[N]    resource type tags
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"NEXE"
CODE_MAGIC = b"4CVP"


@dataclass
class Resource:
    index: int
    type: int
    offset: int
    data: bytes


@dataclass
class Exn:
    header_words: tuple[int, ...]
    main_class: str
    resources: list[Resource]
    code_offset: int
    code: bytes


def parse(blob: bytes) -> Exn:
    if blob[:4] != MAGIC:
        raise ValueError(f"not a NEXE file (magic {blob[:4]!r})")
    words = struct.unpack_from("<I6H", blob, 4)
    main_class = blob[0x14:0x34].split(b"\0", 1)[0].decode("latin-1")
    (n,) = struct.unpack_from("<I", blob, 0x34)
    offsets = struct.unpack_from(f"<{n + 1}I", blob, 0x38)
    types = blob[0x38 + 4 * (n + 1): 0x38 + 4 * (n + 1) + n]
    if len(types) != n:
        raise ValueError("truncated resource type table")
    resources = [
        Resource(i, types[i], offsets[i], blob[offsets[i]:offsets[i + 1]])
        for i in range(n)
    ]
    code_offset = offsets[n]
    code = blob[code_offset:]
    if code[:4] != CODE_MAGIC:
        raise ValueError(f"code chunk at {code_offset:#x} lacks {CODE_MAGIC!r}")
    return Exn(words, main_class, resources, code_offset, code)


def _describe(r: Resource) -> str:
    d = r.data
    if d[6:10] == b"\x89PNG":
        w, h = struct.unpack_from("<HH", d, 0)
        return f"image {w}x{h} (png)"
    printable = sum(32 <= b < 127 or b == 0xFF for b in d[:32])
    if d and printable > 24:
        return "text " + d[:32].replace(b"\xff", b"|").decode("latin-1")
    return d[:16].hex()


def cmd_info(paths: list[str]) -> None:
    for p in paths:
        exn = parse(Path(p).read_bytes())
        hist: dict[int, int] = {}
        for r in exn.resources:
            hist[r.type] = hist.get(r.type, 0) + 1
        print(f"{p}: main={exn.main_class} resources={len(exn.resources)} "
              f"code@{exn.code_offset:#x} ({len(exn.code)} bytes) "
              f"types={ {f'{k:#04x}': v for k, v in sorted(hist.items())} }")
        if len(paths) == 1:
            for r in exn.resources:
                print(f"  [{r.index:3}] type={r.type:#04x} @{r.offset:#07x} "
                      f"len={len(r.data):6}  {_describe(r)}")


def cmd_dump(path: str, outdir: str) -> None:
    exn = parse(Path(path).read_bytes())
    out = Path(outdir)
    out.mkdir(parents=True, exist_ok=True)
    for r in exn.resources:
        ext = "png" if r.data[6:10] == b"\x89PNG" else "bin"
        payload = r.data[6:] if ext == "png" else r.data
        (out / f"res{r.index:03}_t{r.type:02x}.{ext}").write_bytes(payload)
    (out / "code.4cvp").write_bytes(exn.code)
    print(f"wrote {len(exn.resources)} resources + code.4cvp to {out}")


def main(argv: list[str]) -> int:
    if len(argv) >= 2 and argv[0] == "info":
        cmd_info(argv[1:])
    elif len(argv) == 3 and argv[0] == "dump":
        cmd_dump(argv[1], argv[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
