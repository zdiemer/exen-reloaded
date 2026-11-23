# AGENTS.md — working on exen-reloaded

Instructions for any coding agent (and human) working in this repository. `CLAUDE.md` imports
this file.

## What this project is

A portable reimplementation of In-Fusio's ExEn mobile games platform (2002–2004). We implement
the bytecode VM and the native platform layer in C. The ExEn framework classes are bytecode taken
from the vendored original simulator (`vendor/gensimu/gensimu.exe`) and run unmodified on our VM.
Read `README.md` for the goals and `docs/FORMAT.md` for what is known about the file formats.

## Ground rules

- **Correctness is judged against the original.** When behaviour is unclear, check what
  `vendor/gensimu/gensimu.exe` does, by disassembly or by running it, and write down what you
  learn in `docs/`. Do not guess and move on.
- **No per-game hacks.** Fix the VM or the native layer. A game-specific workaround needs a
  comment explaining why no general fix exists.
- **Performance is a goal.** No busy-waiting: the frontend sleeps until the next tick or input.
- **Keep the core portable.** No OS headers in the VM or native layer. Platform code lives in
  frontends.
- **Write format knowledge into code and docs together.** `tools/exn.py` and `docs/FORMAT.md`
  must agree.

## Building

```sh
cmake -S . -B build -G Ninja
cmake --build build
build/exen-run --frames 300 game.exn     # headless: boot, tick, report natives used
```

Needs CMake 3.20 or newer and a C11 compiler. The core (`src/core/`) is plain C with no OS or
SDL dependency. `src/core/natives_gen.h` is generated from the vendored simulator by
`tools/gen_natives.py`; regenerate it rather than editing it.

## Tests

```sh
cmake -S . -B build -G Ninja -DEXEN_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build --output-on-failure      # C unit tests: asset-free, about a second
python -m unittest discover -s tests/python     # tool parsers, generated headers
```

The unit tests need no games (they use the vendored simulator, flash files and SoundFont). Corpus-level
checks live in `tools/harness/compat.py` and `exen-run --selftest-state`. CI runs the unit tests on
Linux, macOS and Windows, plus sanitizers and a check that generated headers are current.

## Tracking work

Work is tracked in GitHub issues, labelled by area (`area:vm`, `area:natives`, `area:graphics`,
...) and grouped into milestones M1-M5. Open an issue for new work or bugs you find, reference it
from PRs (`Closes #N`), and close issues when the work lands.

## Environment

- Nothing environment-specific is committed: no absolute paths, machine names or local config.
  Local settings go in `local.env` (gitignored; see `local.env.example`).
- Games are never committed. The harness reads them from `EXEN_GAMES_DIR`.
- Run the original simulator from a scratch copy of `vendor/gensimu/`, since it writes files
  beside itself.
- The development machine is shared. Run long jobs (corpus sweeps, builds) in the background.

## Git and pull requests

- `main` is protected by convention: all changes land through pull requests, **squash-merged**.
  Branches are deleted on merge.
- Do not add `Co-Authored-By` trailers to commits.
- Python tools must run with the standard library only, unless a dependency is clearly worth it.
