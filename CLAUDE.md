@AGENTS.md

## Claude-specific notes

- When you reverse-engineer something in `gensimu.exe`, record the address, the finding and how
  you confirmed it in `docs/`. The next session starts from those notes, not from your context.
- Capstone is the available disassembler (`python -c "import capstone"`). The image base is
  `0x400000`. `.text` is at `0x401000`, `.rdata` at `0x449000`, `.data` at `0x45a000`.
