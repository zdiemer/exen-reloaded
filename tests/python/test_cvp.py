"""Tests for tools/cvp.py (4CVP class images, docs/re/CVP.md) and tools/gen_natives.py, using
the vendored gensimu.exe and flash/*.exn.

Run from the repository root: python -m unittest discover -s tests/python
"""
from __future__ import annotations

import contextlib
import io
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import cvp  # noqa: E402
import gen_natives  # noqa: E402

GENSIMU = ROOT / "vendor" / "gensimu" / "gensimu.exe"
FLASH = ROOT / "vendor" / "gensimu" / "flash"

FRAMEWORK_ORDER = [
    "exen.Animation", "exen.Graphics", "exen.Image", "exen.TextureMap", "exen.Rectangle",
    "exen.Palette", "exen.Resource", "exen.AnimBitmap", "exen.PlayField", "exen.Sprite",
    "exen.AnimFlash", "exen.Command", "exen.CommandListener", "exen.Displayable",
    "exen.Gamelet", "exen.Component", "exen.Sms", "exen.DialogBox", "exen.FX",
    "exen.GameletEnhanced", "exen.List", "exen.Math", "exen.Matrix3D", "exen.Vector3D",
    "exen.Palette_RAW", "exen.Point2D", "exen.RawData", "exen.RayCast", "exen.util.Debug",
    "java.lang.Object", "java.lang.Class", "java.lang.Exception", "java.lang.Throwable",
    "java.lang.String", "java.lang.ClassLoader", "java.lang.StringBuffer", "java.lang.System",
    "java.lang.Error", "java.lang.Boolean", "java.lang.Byte", "java.lang.Character",
    "java.lang.Cloneable", "java.lang.CloneNotSupportedException", "java.lang.Integer",
    "java.lang.Long", "java.lang.OutOfMemoryError", "java.lang.VirtualMachineError",
    "java.lang.Short", "java.io.Serializable", "vm.sys.Runtime", "vm.sys.Application",
    "vm.sys.Bootstrap", "catalog.Catalog", "catalog.GameProperty",
]


class Hashes(unittest.TestCase):
    def test_crc16_check_value(self):
        self.assertEqual(cvp.crc16(b"123456789"), 0x6F91)  # CRC-16/MCRF4XX

    def test_class_hash(self):
        self.assertEqual(cvp.class_hash("exen.Graphics"), 0xC6ED8E2A)
        self.assertEqual(cvp.class_hash("vm.sys.Bootstrap"), 0x6551F7DC)
        self.assertEqual(cvp.class_hash("java.lang.ArithmeticException"), 0x86F6360B)

    def test_member_hash(self):
        self.assertEqual(cvp.member_hash("<init>", "()V"), 0x3F52EF2F)
        self.assertEqual(cvp.member_hash("drawLine", "(IIII)V"), 0xCF20EA2F)
        self.assertEqual(cvp.member_hash("<clinit>", "()V"), 0x3F5201B3)
        self.assertEqual(cvp.member_hash("<init>", "(Lexen/Image;)V"), 0xD5E1EF2F)
        self.assertEqual(cvp.member_hash("PM_COPY", "S"), 0x6F994AA8)

    def test_hash_command(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            cvp.main(["hash", "drawLine", "(IIII)V"])
            cvp.main(["hash", "exen.Graphics"])
        self.assertIn("cf20ea2f", out.getvalue())
        self.assertIn("c6ed8e2a", out.getvalue())


class Framework(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = GENSIMU.read_bytes()
        cls.va, cls.blob = cvp.find_fw_blob(cls.exe)
        cls.classes = cvp.parse_image(cls.blob)

    def test_blob_location(self):
        self.assertEqual(self.va, 0x4494F0)
        self.assertEqual(len(self.blob), 0xAF8C)
        self.assertEqual(self.blob[:4], b"4CVP")
        kind, img = cvp.load_image(GENSIMU)
        self.assertEqual(kind, "framework")
        self.assertEqual(img, self.blob)

    def test_class_order(self):
        self.assertEqual([c.name for c in self.classes], FRAMEWORK_ORDER)
        self.assertEqual([c.index for c in self.classes], list(range(54)))
        for c in self.classes:
            self.assertEqual(cvp.class_hash(c.name), c.hdr["hash"], c.name)

    def test_header_constants(self):
        for c in self.classes:
            self.assertEqual(c.hdr["u06"], 0x0100, c.name)
            self.assertEqual(c.hdr["u08"], 0x27DD, c.name)
            if c.hdr["clinit"]:
                self.assertEqual(struct.unpack_from("<I", c.raw, c.hdr["clinit"])[0],
                                 0x3F5201B3, c.name)
            if not c.is_interface and c.hdr["ctor"]:
                self.assertEqual(struct.unpack_from("<I", c.raw, c.hdr["ctor"])[0],
                                 0x3F52EF2F, c.name)

    def test_interfaces_and_supers(self):
        by_name = {c.name: c for c in self.classes}
        self.assertTrue(by_name["exen.Animation"].is_interface)
        self.assertTrue(by_name["java.lang.Cloneable"].is_interface)
        self.assertFalse(by_name["exen.Gamelet"].is_interface)
        self.assertEqual(by_name["java.lang.Object"].hdr["super"], 0)
        uni = cvp.Universe(self.classes)
        ge = by_name["exen.GameletEnhanced"]
        self.assertEqual(uni.ref_name(ge, ge.hdr["super"]), "exen.Gamelet")
        s = by_name["java.lang.String"]
        self.assertEqual([uni.ref_name(s, r) for r in s.interfaces], ["java.io.Serializable"])

    def test_no_anomalies(self):
        uni = cvp.Universe(self.classes)
        self.assertEqual(cvp.check_classes(uni, self.classes), [])
        self.assertEqual(sum(len(c.methods) for c in self.classes), 529)

    def test_bootstrap_entry_points(self):
        bs = next(c for c in self.classes if c.name == "vm.sys.Bootstrap")
        hashes = {m.hash for m in bs.methods}
        for h in (0x305A6D35, 0x305A1E56, 0x3F522033, 0x35B0F11E, 0x6F6C0565, 0x305A7631,
                  0x35B0015C, 0x3F523566):
            self.assertIn(h, hashes, f"{h:08x}")
        # the only two methods in the corpus with exception handlers are in the framework
        init = next(m for m in bs.methods if m.hash == 0x35B0F11E)
        self.assertTrue(init.handlers)

    def test_natives(self):
        natives = {}
        for c in self.classes:
            for m in c.methods:
                if m.native_index is not None:
                    self.assertNotIn(m.native_index, natives)
                    natives[m.native_index] = (c.name, m.name)
        self.assertEqual(sorted(natives), list(range(185)))
        self.assertEqual(natives[0], ("exen.Graphics", "clearRect"))
        self.assertEqual(natives[2], ("exen.Graphics", "drawLine"))
        self.assertEqual(natives[158], ("java.lang.String", "length"))

    def test_descriptors(self):
        uni = cvp.Universe(self.classes)
        g = next(c for c in self.classes if c.name == "exen.Graphics")
        m = next(m for m in g.methods if m.name == "drawLine")
        self.assertEqual(uni.descriptor(g, m), "(IIII)V")
        self.assertEqual(m.argc, 4)


class Games(unittest.TestCase):
    def test_vendored_games_parse_cleanly(self):
        for name, nclasses in (("catalog.exn", 4), ("download1.exn", 9)):
            with self.subTest(name):
                uni, classes = cvp.open_file(str(FLASH / name), None)
                self.assertEqual(len(classes), nclasses)
                self.assertEqual(classes[0].index, 54)  # numbered after the framework
                self.assertEqual(cvp.check_classes(uni, classes), [])
                for c in classes:
                    self.assertIsNone(c.name)            # game classes are stripped
                    self.assertEqual(c.hdr["name_off"], 0)

    def test_split_blocks_rejects_garbage(self):
        with self.assertRaises(ValueError):
            cvp.split_blocks(b"JUNK" + bytes(60))
        with self.assertRaises(ValueError):
            cvp.split_blocks(b"4CVP" + struct.pack("<H", 0x10) + bytes(58))  # size < 0x28

    def test_load_image_kinds(self):
        kind, img = cvp.load_image(FLASH / "catalog.exn")
        self.assertEqual(kind, "game")
        self.assertEqual(img[:4], b"4CVP")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "chunk.4cvp"
            p.write_bytes(img)
            self.assertEqual(cvp.load_image(p), ("chunk", img))
            bad = Path(d) / "bad.bin"
            bad.write_bytes(b"hello world")
            with self.assertRaises(ValueError):
                cvp.load_image(bad)

    def test_check_command(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = cvp.main(["check", str(GENSIMU), str(FLASH / "catalog.exn"),
                           str(FLASH / "download1.exn")])
        self.assertEqual(rc, 0)
        self.assertEqual(out.getvalue().count(" 0 anomalies"), 3)


class GeneratedNatives(unittest.TestCase):
    def test_natives_gen_is_up_to_date(self):
        """src/core/natives_gen.h must be what tools/gen_natives.py produces."""
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "natives_gen.h"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gen_natives.main([str(GENSIMU), str(out)]), 0)
            want = out.read_text(encoding="utf-8")
        have = (ROOT / "src" / "core" / "natives_gen.h").read_text(encoding="utf-8")
        self.assertEqual(have.replace("\r\n", "\n"), want)
        self.assertRegex(want, r"#define EXEN_NATIVE_COUNT 185\b")
        self.assertEqual(len(re.findall(r"^    X\(", want, re.M)), 185)


if __name__ == "__main__":
    unittest.main()
