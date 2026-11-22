"""Tests for tools/exn.py on the .exn files vendored with the simulator.

Run from the repository root: python -m unittest discover -s tests/python
"""
from __future__ import annotations

import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import exn  # noqa: E402

FLASH = ROOT / "vendor" / "gensimu" / "flash"


class ParseVendoredFiles(unittest.TestCase):
    def check_layout(self, e: exn.Exn, blob: bytes) -> None:
        # resources tile the file from the end of the type table to the code chunk
        n = len(e.resources)
        table_end = 0x38 + 4 * (n + 1) + n
        prev = None
        for r in e.resources:
            self.assertGreaterEqual(r.offset, table_end)
            if prev is not None:
                self.assertEqual(prev.offset + len(prev.data), r.offset)
            prev = r
        self.assertEqual(prev.offset + len(prev.data), e.code_offset)
        self.assertEqual(e.code, blob[e.code_offset:])
        self.assertEqual(e.code[:4], b"4CVP")
        self.assertEqual(e.header_words[0], 0x80)

    def test_catalog(self):
        blob = (FLASH / "catalog.exn").read_bytes()
        e = exn.parse(blob)
        self.assertEqual(e.main_class, "testcatalog.FTM")
        self.assertEqual(len(e.resources), 15)
        self.assertEqual(e.code_offset, 0x4084)
        self.assertEqual(len(e.code), 5900)
        types = sorted({r.type for r in e.resources})
        self.assertEqual(types, [0x01, 0x03, 0x0D, 0x81])
        self.check_layout(e, blob)
        # an image resource: u16 w, u16 h, u16 stream length, then the PNG signature
        img = e.resources[2]
        self.assertEqual(img.type, 0x0D)
        w, h, n = struct.unpack_from("<HHH", img.data, 0)
        self.assertEqual((w, h), (88, 65))
        self.assertEqual(img.data[6:10], b"\x89PNG")
        self.assertLessEqual(n, len(img.data) - 6)
        self.assertEqual(exn._describe(img), "image 88x65 (png)")

    def test_download1(self):
        blob = (FLASH / "download1.exn").read_bytes()
        e = exn.parse(blob)
        self.assertEqual(e.main_class, "CrazyCobra.GameTopLevel")
        self.assertEqual(len(e.resources), 150)
        self.assertEqual(e.code_offset, 0xE878)
        self.check_layout(e, blob)
        self.assertEqual(sum(r.type == 0x0D for r in e.resources), 25)

    def test_header_words_constant(self):
        a = exn.parse((FLASH / "catalog.exn").read_bytes()).header_words
        b = exn.parse((FLASH / "download1.exn").read_bytes()).header_words
        self.assertEqual(a, b)  # "constant across the known corpus"


class Errors(unittest.TestCase):
    def test_bad_magic(self):
        with self.assertRaises(ValueError):
            exn.parse(b"XEXE" + bytes(0x40))

    def test_missing_code_chunk(self):
        blob = bytearray((FLASH / "catalog.exn").read_bytes())
        e = exn.parse(bytes(blob))
        blob[e.code_offset:e.code_offset + 4] = b"JUNK"
        with self.assertRaises(ValueError):
            exn.parse(bytes(blob))

    def test_truncated_type_table(self):
        blob = bytearray(0x40)
        blob[:4] = b"NEXE"
        struct.pack_into("<I", blob, 0x34, 100)  # 100 resources in a 64-byte file
        with self.assertRaises((ValueError, struct.error)):
            exn.parse(bytes(blob))


class Commands(unittest.TestCase):
    def test_info(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(exn.main(["info", str(FLASH / "catalog.exn")]), 0)
        text = out.getvalue()
        self.assertIn("main=testcatalog.FTM", text)
        self.assertIn("resources=15", text)
        self.assertIn("image 88x65 (png)", text)

    def test_dump(self):
        with tempfile.TemporaryDirectory() as d:
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(exn.main(["dump", str(FLASH / "catalog.exn"), d]), 0)
            files = sorted(p.name for p in Path(d).iterdir())
            self.assertIn("code.4cvp", files)
            self.assertEqual(len(files), 16)
            self.assertIn("res002_t0d.png", files)
            code = (Path(d) / "code.4cvp").read_bytes()
            self.assertEqual(code[:4], b"4CVP")
            png = (Path(d) / "res002_t0d.png").read_bytes()
            self.assertEqual(png[:4], b"\x89PNG")

    def test_usage(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(exn.main([]), 2)


if __name__ == "__main__":
    unittest.main()
