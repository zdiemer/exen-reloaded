# Vendored: In-Fusio EXEN-V2 Generic Simulator

The original simulator from the In-Fusio ExEn SDK, vendored unmodified for preservation and as the
reference implementation that exen-reloaded is checked against. It was distributed as freeware and
is abandoned; this copy comes from the archive.org distribution.

- Product: EXEN-V2 Generic Simulator (`gensimu.exe`), build date 2003-05-15 (from its own log)
- Format: PE32, i386, Win32 GUI (GDI drawing, WinMM MIDI out, Winsock for the SMS server emulation)

| File | SHA-256 |
|---|---|
| `gensimu.exe` | `4a07fa5ee8961497c8e323e077867fdfd0ff7ce7001797bfe8dcb2ce9f3d2189` |
| `flash/catalog.exn` | `f36dbfae76f91ea7e5c3793e3a7a64816e4de74838fc2d3e672f59bce90b78a5` |
| `flash/download1.exn` | `1b99017473850da0c36a1b6fa07ec0974d6f6b57932f3d272273cfbbe6e495bb` |
| `flash/eeprom.dat` | `4bd57db8a650a716bdaa1dcfeaa69e669bdcd72549cdedb8ea47d17cc338df4e` |

`simulator.ini` and `language/` are the stock configuration. The `[Recent]` paths in
`simulator.ini` are the original authors' and are kept as shipped.

## What exen-reloaded uses it for

- The ExEn **framework bytecode** (the `4CVP` class blocks in `.rdata`) is extracted from
  `gensimu.exe` at build time rather than redistributed separately.
- **Reference behaviour.** It runs a game from the command line:
  `gensimu.exe <game.exn>`. Run it from a scratch copy of this directory, because it writes
  `simulator.ini`, `simulator.log` and `flash/eeprom.dat` beside itself.
