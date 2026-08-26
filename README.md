# sync32

A maker-friendly game console: one RP2350, one cable, one SD card.
Native ARM games in `.s32` ROM files: same file runs on hardware and emulator.

- `firmware/` : console firmware: boot launcher, display-list compositor,
  SD games + saves, USB pads, crash-safe. Board: Waveshare RP2350-PiZero
  (more boards to come: the ABI is the console, not the board).
- `sdk/` : `sync32.h` (the frozen ABI), crt0 + linker scripts, `mks32.py`
  ROM packer, template game, Formation demo.
- `emulator/` : HLE emulator (Unicorn + pygame): `sync32emu.py game.s32`.
- `roms/` : built games.

Build a game: `cd sdk/template && make` -> `template.s32`.
Run it: `emulator/sync32emu.py sdk/template/template.s32`.
