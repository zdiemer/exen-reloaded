#!/usr/bin/env python3
"""Headless compatibility sweep for exen-reloaded.

    python tools/harness/compat.py run  [GAMES_DIR] --out OUT [--jobs N] [--only NAME...]
    python tools/harness/compat.py page --out OUT                 # (re)write OUT/COMPATIBILITY.md
    python tools/harness/compat.py publish --out OUT --repo OWNER/NAME   # one issue per game

GAMES_DIR defaults to $EXEN_GAMES_DIR. For every .exn the sweep runs build/exen-run twice:
  * idle:  no input, to see what the game does by itself (boot, attract loop)
  * input: a generic key script (select / fire / directions) to see whether it responds and
           reaches gameplay
and scores the result into a tier:

  incompatible  never presented a frame
  crashed       the VM stopped after the game had started (fatal error, uncaught exception)
  black         frames were presented but every sampled frame is a single flat colour
  renders       draws real screens but showed no response to input
  playable      responds to input and reaches several distinct screens without stopping

Per game it writes OUT/shots/<slug>.png (most detailed frame), OUT/clips/<slug>.gif (the input
run) and an entry in OUT/report.json. Standard library + Pillow. A sweep of the 47-game
corpus takes about a minute.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "build" / ("exen-run.exe" if os.name == "nt" else "exen-run")
SIM = ROOT / "vendor/gensimu/gensimu.exe"

IDLE_TICKS = 400
INPUT_TICKS = 1500
DUMP_EVERY = 5
DEFAULT_SIZE = "132x176"  # what exen-run uses for games missing from data/gamedb.tsv

TIERS = ["incompatible", "crashed", "black", "renders", "playable"]


def input_script() -> str:
    """A generic script: get through splash screens and menus, start the game, then move
    around. 'clear' is avoided: many games treat it as quit."""
    keys = []
    for t in range(40, 400, 30):
        keys.append(f"{t}:ok")
    seq = ["fire", "right/6", "fire", "left/6", "up/4", "fire", "down/4", "ok", "5", "2/3", "8/3",
           "4/3", "6/3"]
    t = 420
    while t < INPUT_TICKS - 20:
        for k in seq:
            keys.append(f"{t}:{k}")
            t += 12
    return ",".join(keys)


def slug(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-")


def run_exen(game: Path, size: str | None, ticks: int, keys: str | None, dump: Path) -> dict:
    dump.mkdir(parents=True, exist_ok=True)
    cmd = [str(EXE), "--sim", str(SIM), "--quiet", "--frames", str(ticks),
           "--dump", str(dump), "--dump-every", str(DUMP_EVERY)]
    if keys:
        cmd += ["--keys", keys]
    if size:  # otherwise exen-run picks the game's handset from data/gamedb.tsv
        cmd += ["--size", size]
    t0 = time.time()
    try:
        r = subprocess.run(cmd + [str(game)], capture_output=True, text=True, timeout=300)
        out, rc = r.stdout, r.returncode
    except subprocess.TimeoutExpired:
        out, rc = "", -1
    m = re.search(r"ticks: (\d+)\s+frames presented: (\d+)\s+timer period: (-?\d+)", out)
    status = re.search(r"status: (.*)", out)
    unimpl = re.findall(r"#(\d+)\s+(\S+)\s+(\S+)\s+(\d+)\s+\(unimplemented\)", out)
    frames = sorted(dump.glob("frame_*.png"))
    scr = re.search(r"screen: (\d+x\d+)", out)
    return {
        "size": scr.group(1) if scr else (size or DEFAULT_SIZE),
        "rc": rc, "seconds": round(time.time() - t0, 2),
        "ticks": int(m.group(1)) if m else 0,
        "presented": int(m.group(2)) if m else 0,
        "period_ms": int(m.group(3)) if m else None,
        "status": status.group(1).strip() if status else ("timeout" if rc == -1 else f"no report (rc {rc})"),
        "unimplemented": [f"{c}.{n} (#{i})" for i, c, n, _ in unimpl],
        "frames": [str(f) for f in frames],
    }


def fingerprint(img: Image.Image) -> bytes:
    """Coarse perceptual fingerprint: 16x16 greyscale, quantised. Distinct screens differ."""
    g = img.convert("L").resize((16, 16), Image.BOX)
    return bytes(v >> 4 for v in g.tobytes())


def detail(img: Image.Image) -> int:
    b = img.resize((img.width // 2 or 1, img.height // 2 or 1), Image.NEAREST).tobytes()
    return len({b[i:i + 3] for i in range(0, len(b), 3)})


def analyse(paths: list[str]) -> dict:
    imgs = [Image.open(p).convert("RGB") for p in paths]
    flat = [len(set(i.resize((32, 32), Image.NEAREST).tobytes()[k:k + 3]
                    for k in range(0, 32 * 32 * 3, 3))) <= 1 for i in imgs]
    fps = [fingerprint(i) for i in imgs if True]
    distinct: list[bytes] = []
    for f in fps:
        if all(sum(abs(a - b) for a, b in zip(f, d)) > 40 for d in distinct):
            distinct.append(f)
    best = max(range(len(imgs)), key=lambda k: detail(imgs[k])) if imgs else None
    return {"n": len(imgs), "flat_ratio": (sum(flat) / len(flat)) if flat else 1.0,
            "screens": distinct, "best": paths[best] if best is not None else None}


def score(game: Path, forced_size: str | None, outdir: Path) -> dict:
    name = game.stem
    with tempfile.TemporaryDirectory() as tmp:
        idle = run_exen(game, forced_size, IDLE_TICKS, None, Path(tmp) / "idle")
        inp = run_exen(game, forced_size, INPUT_TICKS, input_script(), Path(tmp) / "input")
        size = inp["size"]
        a_idle, a_inp = analyse(idle["frames"]), analyse(inp["frames"])
        new_screens = [s for s in a_inp["screens"]
                       if all(sum(abs(x - y) for x, y in zip(s, t)) > 40 for t in a_idle["screens"])]
        stopped = inp["status"] != "running"
        if inp["presented"] == 0 and idle["presented"] == 0:
            tier = "incompatible"
        elif a_inp["n"] and a_inp["flat_ratio"] >= 0.999 and a_idle["flat_ratio"] >= 0.999:
            tier = "black"
        elif stopped and inp["presented"] < 50:
            tier = "crashed"
        elif len(new_screens) >= 2 and not stopped:
            tier = "playable"
        elif stopped:
            tier = "crashed"
        else:
            tier = "renders"
        group = classify(tier, inp, idle)
        shots, clips = outdir / "shots", outdir / "clips"
        shots.mkdir(parents=True, exist_ok=True)
        clips.mkdir(parents=True, exist_ok=True)
        s = slug(name)
        if a_inp["best"] or a_idle["best"]:
            Image.open(a_inp["best"] or a_idle["best"]).convert("RGB").resize(
                _scaled(size), Image.NEAREST).save(shots / f"{s}.png")
        if inp["frames"]:
            fr = [Image.open(p).convert("RGB").resize(_scaled(size), Image.NEAREST)
                  for p in inp["frames"][::2]]
            period = inp["period_ms"] or 100
            fr[0].save(clips / f"{s}.gif", save_all=True, append_images=fr[1:],
                       duration=max(20, period * DUMP_EVERY * 2), loop=0, optimize=True)
    return {
        "game": name, "file": game.name, "slug": s, "size": size, "tier": tier, "group": group,
        "status": inp["status"], "idle_status": idle["status"],
        "presented": inp["presented"], "ticks": inp["ticks"], "period_ms": inp["period_ms"],
        "distinct_screens": len(a_inp["screens"]), "screens_from_input": len(new_screens),
        "flat_ratio": round(a_inp["flat_ratio"], 3),
        "unimplemented": sorted(set(inp["unimplemented"] + idle["unimplemented"])),
        "run_seconds": round(idle["seconds"] + inp["seconds"], 2),
    }


def _scaled(size: str) -> tuple[int, int]:
    w, h = (int(v) for v in size.split("x"))
    return w * 2, h * 2


def classify(tier: str, inp: dict, idle: dict) -> str:
    st = inp["status"]
    if tier == "playable":
        return "playable"
    if "sms" in st.lower() or "network" in st.lower():
        return "sms"
    if "uncatchable exception" in st or "uncaught" in st:
        m = re.search(r"exception (\S+)", st)
        return "exception-" + (m.group(1).split(".")[-1] if m else "unknown")
    if inp["unimplemented"] or idle["unimplemented"]:
        return "unimplemented-native"
    if tier == "black":
        return "black-screen"
    if tier == "incompatible":
        return "no-frames"
    if tier == "renders":
        return "no-input-response"
    return "vm-stopped"


def cmd_run(args) -> int:
    gdir = args.games or os.environ.get("EXEN_GAMES_DIR")
    if not gdir:
        raise SystemExit("no games directory: pass one or set EXEN_GAMES_DIR")
    games = sorted(Path(gdir).glob("*.exn"))
    if args.only:
        games = [g for g in games if g.stem in args.only]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    with ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(lambda g: score(g, args.size, out), games))
    prev = {}
    rep = out / "report.json"
    if rep.exists() and args.only:
        prev = {r["game"]: r for r in json.loads(rep.read_text())["games"]}
    for r in results:
        prev[r["game"]] = r
    allres = sorted(prev.values(), key=lambda r: r["game"].lower())
    rep.write_text(json.dumps({"generated": time.strftime("%Y-%m-%d"), "games": allres}, indent=1) + "\n",
                   encoding="utf-8", newline="\n")
    for r in results:
        print(f"{r['game'][:24]:24} {r['size']:8} {r['tier']:12} {r['group']:22} "
              f"screens={r['distinct_screens']:2}/{r['screens_from_input']:2} {r['status'][:50]}")
    counts = {t: sum(r["tier"] == t for r in allres) for t in TIERS}
    print(f"\n{len(allres)} games in {time.time() - t0:.0f}s: " +
          ", ".join(f"{counts[t]} {t}" for t in reversed(TIERS)))
    write_page(out, allres)
    return 0


def write_page(out: Path, games: list[dict]) -> None:
    counts = {t: sum(g["tier"] == t for g in games) for t in TIERS}
    groups: dict[str, list[str]] = {}
    for g in games:
        if g["tier"] != "playable":
            groups.setdefault(g["group"], []).append(g["game"])
    lines = [
        "# exen-reloaded compatibility",
        "",
        "Every ExEn game in the corpus, booted headlessly by `tools/harness/compat.py` in the",
        "exen-reloaded repository and scored from the frames it presents. Regenerate with",
        "`python tools/harness/compat.py run --out <this repo>`.",
        "",
        "## Summary",
        "",
        "| Games | Playable | Renders | Black | Crashed | Incompatible |",
        "|--:|--:|--:|--:|--:|--:|",
        f"| {len(games)} | {counts['playable']} | {counts['renders']} | {counts['black']} | "
        f"{counts['crashed']} | {counts['incompatible']} |",
        "",
        "### What the tiers mean",
        "",
        "| Tier | Meaning |",
        "|---|---|",
        "| `playable` | Responds to input and reaches several distinct screens without the VM stopping |",
        "| `renders` | Draws real screens, but the generic key script got no response |",
        "| `black` | Frames were presented, but every sampled frame was one flat colour |",
        "| `crashed` | The VM stopped after the game started (fatal error or uncaught exception) |",
        "| `incompatible` | Never presented a frame |",
        "",
        "The input test is a generic script (select, fire, directions). A `renders` game may be",
        "fine and just need different keys; check its clip.",
        "",
    ]
    if groups:
        lines += ["## Not yet playable, by cause", "", "| Cause | Games |", "|---|---|"]
        for grp, names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            lines.append(f"| `{grp}` | {', '.join(sorted(names))} |")
        lines.append("")
    lines += ["## Games", "", "| Game | Tier | Screen | Cause | Shot |", "|---|---|---|---|---|"]
    for g in games:
        lines.append(f"| {g['game']} | `{g['tier']}` | {g['size']} | "
                     f"{'' if g['tier'] == 'playable' else '`' + g['group'] + '`'} | "
                     f"[shot](shots/{g['slug']}.png) · [clip](clips/{g['slug']}.gif) |")
    (out / "COMPATIBILITY.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def cmd_page(args) -> int:
    out = Path(args.out)
    write_page(out, json.loads((out / "report.json").read_text())["games"])
    return 0


TIER_TEXT = {
    "playable": "This game runs: it responds to input and reaches several distinct screens "
                "without the VM stopping. Tracked here so regressions have somewhere to land.",
    "renders": "The game draws real screens, but the generic key script got no response. It may "
               "need different keys; check the clip.",
    "black": "Frames were presented, but every sampled frame was a single flat colour.",
    "crashed": "The VM stopped after the game had started.",
    "incompatible": "The game never presented a frame.",
}


def issue_body(g: dict, repo: str) -> str:
    raw = f"https://github.com/{repo}/blob/main"
    rows = [("Tier", f"`{g['tier']}`"), ("Cause", f"`{g['group']}`"),
            ("Screen", g["size"]), ("VM status after the input run", f"`{g['status']}`"),
            ("Frames presented", g["presented"]), ("Timer ticks", g["ticks"]),
            ("Tick period", f"{g['period_ms']} ms"), ("Distinct screens", g["distinct_screens"]),
            ("Screens reached only with input", g["screens_from_input"]),
            ("Flat-colour frame ratio", g["flat_ratio"])]
    body = [f"**Tier:** `{g['tier']}`  |  **Cause:** `{g['group']}`  |  **Screen:** {g['size']}", "",
            f"![representative frame]({raw}/shots/{g['slug']}.png?raw=true)", "",
            f"*Most detailed frame captured. Clip: [clips/{g['slug']}.gif]({raw}/clips/{g['slug']}.gif)*",
            "", "## What happens", "", TIER_TEXT[g["tier"]], "", "## Run metrics", "",
            "| Metric | Value |", "|---|---|"] + [f"| {k} | {v} |" for k, v in rows]
    if g["unimplemented"]:
        body += ["", "## Unimplemented natives hit", ""] + [f"- `{u}`" for u in g["unimplemented"]]
    body += ["", "## Reproduce", "", "```sh",
             f"build/exen-run --frames {INPUT_TICKS} --dump out/ \"$EXEN_GAMES_DIR/{g['file']}\"",
             f"python tools/harness/compat.py run --only \"{g['game']}\" --out <compat repo>", "```"]
    return "\n".join(body) + "\n"


def gh(*args: str, input: str | None = None) -> str:
    r = subprocess.run(["gh", *args], capture_output=True, text=True, input=input, encoding="utf-8")
    if r.returncode:
        raise SystemExit(f"gh {' '.join(args[:3])}: {r.stderr.strip()}")
    return r.stdout


def cmd_publish(args) -> int:
    """One issue per game in the compat repo: created, or edited in place when it exists.
    Labels: status:<tier> and group:<cause>; the issue title carries the tier."""
    repo, out = args.repo, Path(args.out)
    games = json.loads((out / "report.json").read_text())["games"]
    colours = {"playable": "0e8a16", "renders": "fbca04", "black": "5319e7", "crashed": "d93f0b",
               "incompatible": "b60205"}
    for t, c in colours.items():
        gh("label", "create", f"status: {t}", "-R", repo, "--color", c, "--force")
    for grp in sorted({g["group"] for g in games}):
        gh("label", "create", f"group: {grp}", "-R", repo, "--color", "c5def5", "--force")
    existing = json.loads(gh("issue", "list", "-R", repo, "--state", "all", "--limit", "1000",
                             "--json", "number,title,labels"))
    by_game = {i["title"].split(":")[0]: i for i in existing}
    for g in games:
        title = f"{g['game']}: {g['tier'].capitalize()}"
        labels = [f"status: {g['tier']}", f"group: {g['group']}"]
        body = issue_body(g, repo)
        cur = by_game.get(g["game"])
        if cur:
            stale = [l["name"] for l in cur["labels"]
                     if l["name"].startswith(("status: ", "group: ")) and l["name"] not in labels]
            cmd = ["issue", "edit", str(cur["number"]), "-R", repo, "--title", title, "--body-file", "-",
                   "--add-label", ",".join(labels)]
            if stale:
                cmd += ["--remove-label", ",".join(stale)]
            gh(*cmd, input=body)
            print(f"updated #{cur['number']} {title}")
        else:
            url = gh("issue", "create", "-R", repo, "--title", title, "--body-file", "-",
                     "--label", ",".join(labels), input=body).strip()
            print(f"created {url.rsplit('/', 1)[-1]} {title}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    pb = sub.add_parser("publish", help="create/update one issue per game in the compat repo")
    pb.add_argument("--out", required=True)
    pb.add_argument("--repo", required=True, help="owner/name of the compat repository")
    r = sub.add_parser("run")
    r.add_argument("games", nargs="?")
    r.add_argument("--out", required=True)
    r.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    r.add_argument("--size", help="force one screen size for every game")
    r.add_argument("--only", nargs="*")
    p = sub.add_parser("page")
    p.add_argument("--out", required=True)
    a = ap.parse_args()
    return {"run": cmd_run, "page": cmd_page, "publish": cmd_publish}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
