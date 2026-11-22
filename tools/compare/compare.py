#!/usr/bin/env python3
"""Record a game in the original 2003 simulator and in exen-reloaded, side by side.

    python tools/compare/compare.py game.exn out.gif [--size 101x80] [--seconds 20]
        [--keys "ms:key,ms:key/hold_ms,..."] [--exen-run build/exen-run.exe]

Windows only (it drives gensimu.exe's window). Needs Pillow.

How it works:
  * gensimu: a scratch copy of vendor/gensimu is set to the handset profile with the requested
    screen size, launched on the game, and steered through its phone "Games menu" with Enter.
    Frames are grabbed from its window with PrintWindow at a fixed rate; keys are posted to it
    on a wall clock that starts when the game is selected.
  * exen-reloaded: build/exen-run boots the same game headlessly at the same size and dumps
    every presented frame; the same key script is converted from milliseconds to ticks using
    the game's own timer period.
  * Both streams are resampled onto one timeline and composed into a labelled GIF.

Key names: 0-9 * # ok clear up down left right fire (fire = '5', arrows = 2/4/6/8 as gensimu maps
them). gensimu receives the PC keys that map to those phone keys.
"""
from __future__ import annotations

import argparse
import configparser
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import winsnap  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# gensimu's colour profiles (the DEPTH=8 [Manuf.NNN] sections, in order) -> CURRENT_COLOR index
PROFILES = {(120, 143): 0, (101, 80): 1, (132, 176): 2, (128, 141): 3, (112, 112): 5}

# phone key -> Windows virtual key that gensimu maps to it (docs/re/NATIVES.md §4.2)
VK = {"ok": 0x0D, "clear": 0x1B, "up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27,
      "fire": 0x65, "0": 0x60, "1": 0x67, "2": 0x68, "3": 0x69, "4": 0x64, "5": 0x65,
      "6": 0x66, "7": 0x61, "8": 0x62, "9": 0x63, "*": 0x6A, "#": 0x6F}

ZOOM = 2  # simulator.ini ZoomRatio
FPS = 10
MATCH = 6.0         # mean abs RGB difference (0-255) for two screens to count as the same
KEY_TIMEOUT = 60.0  # seconds to wait for a matching screen before pressing anyway
REF_WINDOW = 0.6    # seconds of exen frames before a press that define its screen
SLOWDOWN = 4.0      # assumed gensimu slowdown for keys whose screen never matches


def distance(a: Image.Image, b: Image.Image) -> float:
    sa, sb = a.resize((48, 48), Image.BOX), b.resize((48, 48), Image.BOX)
    da, db = sa.tobytes(), sb.tobytes()
    return sum(abs(x - y) for x, y in zip(da, db)) / len(da)


class Signature:
    """What identifies a screen: the pixels that stay constant over a window of frames.
    Animated backgrounds, scrolling and blinking differ in phase between the two emulators,
    so only the stable part (text, logos, static UI) is compared."""

    def __init__(self, frames: list[Image.Image], size: tuple[int, int]):
        self.size = size
        native = [f.resize(size, Image.NEAREST).tobytes() for f in frames]
        self.ref = native[-1] if native else b""
        n = size[0] * size[1]
        self.mask = [i for i in range(n)
                     if all(f[3 * i:3 * i + 3] == self.ref[3 * i:3 * i + 3] for f in native)] if native else []
        self.usable = len(native) >= 3 and len(self.mask) >= n // 50

    def distance(self, img: Image.Image) -> float:
        b = img.resize(self.size, Image.NEAREST).tobytes()
        r = self.ref
        return sum(abs(b[3 * i + c] - r[3 * i + c]) for i in self.mask for c in range(3)) / (3 * len(self.mask))


def parse_keys(spec: str) -> list[tuple[int, str, int]]:
    out = []
    for item in filter(None, (s.strip() for s in spec.split(","))):
        m = re.fullmatch(r"(\d+):([^/]+)(?:/(\d+))?", item)
        if not m or m.group(2) not in VK:
            raise SystemExit(f"bad key spec {item!r}")
        out.append((int(m.group(1)), m.group(2), int(m.group(3) or 150)))
    return sorted(out)


def is_games_menu(img: Image.Image) -> bool:
    """The phone menu's highlight bar is teal (0x006666) across the second text row."""
    w, _ = img.size
    px = img.getpixel((w - 4, 20 * ZOOM))
    return abs(px[0] - 0) < 30 and abs(px[1] - 0x66) < 40 and abs(px[2] - 0x66) < 40


def record_gensimu(game: Path, size: tuple[int, int], seconds: float, keys, work: Path, tail: float = 5.0):
    """keys: (ms, name, hold_ms, reference images). A key is pressed once the screen matches
    any of its reference frames (or at its time if it has none, or after KEY_TIMEOUT). Several
    references cover animated screens, whose phase differs between the two emulators."""
    sim = work / "gensimu"
    shutil.copytree(ROOT / "vendor/gensimu", sim)
    ini = sim / "simulator.ini"
    text = ini.read_text(encoding="latin-1")
    text = re.sub(r"(?m)^CURRENT_COLOR=\d+", f"CURRENT_COLOR={PROFILES[size]}", text)
    ini.write_text(text, encoding="latin-1")
    proc = subprocess.Popen([str(sim / "gensimu.exe"), str(game)], cwd=sim)
    try:
        hwnd = winsnap.find_window(proc.pid)
        # wait for the phone's Games menu, then select the game (first entry)
        deadline = time.time() + 30
        while time.time() < deadline and not is_games_menu(winsnap.grab_client(hwnd)):
            time.sleep(0.2)
        for _ in range(10):
            winsnap.post_key(hwnd, VK["ok"], True)
            time.sleep(0.1)
            winsnap.post_key(hwnd, VK["ok"], False)
            time.sleep(0.8)
            if not is_games_menu(winsnap.grab_client(hwnd)):
                break
        t0 = time.time()
        frames, pending = [], list(keys)
        releases: list[tuple[float, int]] = []
        presses: list[float] = []
        matched: list[bool] = []
        waited_since = 0.0
        while (now := time.time() - t0) < seconds:
            ms = now * 1000
            if proc.poll() is not None:
                break
            img = winsnap.grab_client(hwnd).crop((0, 0, size[0] * ZOOM, size[1] * ZOOM))
            frames.append((now, img))
            if pending:
                sig = pending[0][3]
                # never sooner after the previous press than exen-reloaded was (gensimu is slower)
                ready = now - waited_since >= pending[0][5]
                due = ready and ((sig is not None and sig.usable and sig.distance(img) < MATCH) or
                                 (now - waited_since > pending[0][4]))
                if due:
                    matched.append(bool(sig is not None and sig.usable and sig.distance(img) < MATCH))
                    _, name, hold, _, _, _ = pending.pop(0)
                    winsnap.post_key(hwnd, VK[name], True)
                    releases.append((ms + hold, VK[name]))
                    presses.append(now)
                    waited_since = now
            for r in [r for r in releases if r[0] <= ms]:
                winsnap.post_key(hwnd, r[1], False)
                releases.remove(r)
            if not pending and not releases and presses and now - presses[-1] > tail:
                break
            time.sleep(max(0.0, 1 / FPS - (time.time() - t0 - now)))
        frames_presses = presses
        print(f"gensimu: {sum(matched)} of {len(matched)} keys pressed on a screen match, the rest on the timed fallback")
        return frames, frames_presses, proc.poll() is not None
    finally:
        proc.kill()


def record_exen(exen_run: Path, game: Path, size, seconds: float, keys, work: Path):
    w, h = size
    probe = subprocess.run([str(exen_run), "--quiet", "--frames", "5", "--size", f"{w}x{h}", str(game)],
                           capture_output=True, text=True)
    m = re.search(r"timer period: (-?\d+)", probe.stdout)
    period = int(m.group(1)) if m and int(m.group(1)) > 0 else 100
    ticks = int(seconds * 1000 / period) + 1
    keyspec = ",".join(f"{max(1, ms // period)}:{name}/{max(1, hold // period)}" for ms, name, hold in keys)
    out = work / "exen"
    out.mkdir()
    cmd = [str(exen_run), "--quiet", "--frames", str(ticks), "--size", f"{w}x{h}", "--dump", str(out)]
    if keyspec:
        cmd += ["--keys", keyspec]
    res = subprocess.run(cmd + [str(game)], capture_output=True, text=True)
    frames = []
    for p in sorted(out.glob("frame_*_t*.png")):
        tick = int(re.search(r"_t(\d+)", p.name).group(1))
        frames.append((tick * period / 1000, Image.open(p).convert("RGB").resize((w * ZOOM, h * ZOOM), Image.NEAREST)))
    status = re.search(r"status: (.*)", res.stdout)
    presses = [max(1, ms // period) * period / 1000 for ms, _, _ in keys]
    return frames, presses, period, status.group(1) if status else "?"


def at(frames, t, size):
    """Latest frame at or before time t (black before the first one)."""
    best = None
    for ft, img in frames:
        if ft <= t:
            best = img
        else:
            break
    return best or Image.new("RGB", size, "black")


def compose(gs, ex, size, seconds, path: Path, labels):
    w, h = size[0] * ZOOM, size[1] * ZOOM
    pad, top = 8, 16
    out = []
    for i in range(int(seconds * FPS)):
        t = i / FPS
        frame = Image.new("RGB", (w * 2 + pad * 3, h + top + pad), (24, 24, 28))
        d = ImageDraw.Draw(frame)
        d.text((pad, 2), labels[0], fill=(220, 220, 220))
        d.text((w + pad * 2, 2), labels[1], fill=(220, 220, 220))
        frame.paste(at(gs, t, (w, h)), (pad, top))
        frame.paste(at(ex, t, (w, h)), (w + pad * 2, top))
        out.append(frame)
    out[0].save(path, save_all=True, append_images=out[1:], duration=int(1000 / FPS), loop=0, optimize=True)


def warp(frames, presses, other_presses, t):
    """Map time t on the reference (exen) timeline to this stream's timeline, segment by
    segment between key presses, so both sides show the same content at once."""
    a = [0.0] + list(other_presses)
    b = [0.0] + list(presses)
    n = min(len(a), len(b))
    for i in range(n - 1, -1, -1):
        if t >= a[i]:
            if i + 1 < n:
                span_a, span_b = a[i + 1] - a[i], b[i + 1] - b[i]
                return b[i] + (t - a[i]) * (span_b / span_a if span_a > 0 else 1)
            return b[i] + (t - a[i])
    return t


def compose_synced(gs, gs_presses, ex, ex_presses, size, seconds, path: Path):
    w, h = size[0] * ZOOM, size[1] * ZOOM
    pad, top = 8, 16
    out = []
    for i in range(int(seconds * FPS)):
        t = i / FPS
        frame = Image.new("RGB", (w * 2 + pad * 3, h + top + pad), (24, 24, 28))
        d = ImageDraw.Draw(frame)
        d.text((pad, 2), "gensimu (time-aligned)", fill=(220, 220, 220))
        d.text((w + pad * 2, 2), "exen-reloaded", fill=(220, 220, 220))
        frame.paste(at(gs, warp(gs, gs_presses, ex_presses, t), (w, h)), (pad, top))
        frame.paste(at(ex, t, (w, h)), (w + pad * 2, top))
        out.append(frame)
    out[0].save(path, save_all=True, append_images=out[1:], duration=int(1000 / FPS), loop=0, optimize=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--size", default="101x80")
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--keys", default="")
    ap.add_argument("--gensimu-seconds", type=float, default=180,
                    help="max wall time for the gensimu recording (it runs slower)")
    ap.add_argument("--exen-run", type=Path, default=ROOT / "build/exen-run.exe")
    a = ap.parse_args()
    size = tuple(int(v) for v in a.size.split("x"))
    if size not in PROFILES:
        raise SystemExit(f"gensimu has no colour profile of size {a.size}; known: {sorted(PROFILES)}")
    keys = parse_keys(a.keys)
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        ex, ex_presses, period, status = record_exen(a.exen_run, a.game.resolve(), size, a.seconds, keys, work)
        # reference frame for each key: what exen-reloaded showed just before the press
        # reference frames for each key: what exen-reloaded showed in the REF_WINDOW before it
        prev = [0.0] + ex_presses[:-1]
        # fallback when nothing matches (e.g. gameplay, which diverges): press after the exen gap
        # scaled by SLOWDOWN, counted from the previous press
        refs = [(ms, name, hold,
                 Signature([img for ft, img in ex if max(p, t - REF_WINDOW) <= ft < t], size),
                 min(KEY_TIMEOUT, max(3.0, SLOWDOWN * (t - p))), t - p)
                for (ms, name, hold), t, p in zip(keys, ex_presses, prev)]
        for (ms, name, _, sig, _, _) in refs:
            print(f"key {name}@{ms}ms: {len(sig.mask)} stable px{'' if sig.usable else ' (unusable: timed fallback)'}")
        tail = max(3.0, a.seconds - ex_presses[-1]) if ex_presses else a.seconds
        gs, gs_presses, gs_died = record_gensimu(a.game.resolve(), size, a.gensimu_seconds, refs, work, tail)
        compose(gs, ex, size, a.seconds, a.out, ("gensimu (2003)", "exen-reloaded"))
        synced = a.out.with_name(a.out.stem + "_synced" + a.out.suffix)
        compose_synced(gs, gs_presses, ex, ex_presses, size, a.seconds, synced)
    print(f"{a.out} (real time) and {synced} (time-aligned): gensimu {len(gs)} captures, "
          f"{len(gs_presses)}/{len(keys)} keys pressed{' (process exited)' if gs_died else ''}; "
          f"exen {len(ex)} frames, period {period} ms, {status}")
    if gs_presses:
        print("press times (s)  gensimu: " + " ".join(f"{t:.1f}" for t in gs_presses)
              + "   exen: " + " ".join(f"{t:.1f}" for t in ex_presses))
    return 0


if __name__ == "__main__":
    sys.exit(main())
