# sync32

A maker-friendly game console: one RP2350, one HDMI cable, one SD card.
Native ARM games in `.s32` ROM files — the same file runs on hardware
and in the emulator.

- `firmware/` — the console: boot launcher, display-list compositor, SD
  games + saves, USB gamepads, crash-safe boot with persistent event log.
  Reference board: Waveshare RP2350-PiZero (the ABI is the console, not
  the board; more board configs welcome).
- `carts/` — bigger example carts: `nes` (agnes core) and `nesf`
  (libretro-fceumm, XIP mode) — NES emulators running as sync32 games.
- `roms/` — prebuilt `.s32` games.
- `docs/ABI.md` — the ROM format and syscall contract.
- `sdk/` — a symlink to a [sync32-sdk](https://github.com/monteslu/sync32-sdk)
  checkout beside this one: the "copy this repo and make a game" starter.

Related repos, all expected to be cloned into the SAME parent directory
(the carts and tooling find each other by relative path):
[sync32-sdk](https://github.com/monteslu/sync32-sdk) ·
[sync32-gl](https://github.com/monteslu/sync32-gl) ·
[sync32-emulator](https://github.com/monteslu/sync32-emulator)

```
somewhere/
  sync32/           # this repo: firmware, launcher, docs, carts
  sync32-sdk/       # cart headers, crt0, linker scripts, mks32.py
  sync32-gl/        # 3D renderer for carts
  sync32-emulator/  # the C emulator: s32run, s32play, libretro, wasm
```

## Building the firmware

Requires the [pico-sdk](https://github.com/raspberrypi/pico-sdk) 2.2.0
(with the tinyusb submodule) and `arm-none-eabi-gcc`:

```
git clone https://github.com/monteslu/sync32
git clone https://github.com/monteslu/sync32-sdk   # sdk/ symlinks to this
cd sync32/firmware
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build -j8
```

Flash `build/sync32.uf2` over BOOTSEL (hold the button, plug in, copy the
file), with `picotool load -x`, or over SWD with a debugprobe.

## Using the console

- Boot with a PC on the USB port: serial console + USB thumb-drive mode
  (`Y` in the menu) for copying games to the SD card, plus YMODEM
  receive (`r`, then `sz --ymodem game.s32` from any terminal).
- Boot with a gamepad on the USB port: pad mode. Xbox-family (XInput)
  pads work today; hot-plug is fine. Changing what's plugged in =
  power-cycle, like any console.
- A UART serial adapter on GP0/GP1 (115200) gives the serial console and
  YMODEM even while a gamepad owns the USB port.
## What goes on the SD card

> **Specified, not yet built.** The firmware today lists bare `.s32` files
> and reads streamed data from a `<romname>/` folder beside them. The folder
> and tar forms below are settled in `docs/ABI.md` 3.2 and are what the
> loader is being changed to.

A game is a **folder containing `main.s32e`**, or that same folder tarred up
as a single `NAME.s32`. Both run identically; a game cannot tell which it
came from. Mix them freely at the top level:

```
/
  chromium.s32          a game, one file
  nes/                  a game, as a folder
    main.s32e             its executable
    info.txt              title = NES
    icon.bmp              16x16
    smb.nes               drop your own files in
```

The folder form is the one to use while making a game, and for anything the
player adds files to: an emulator ships as a folder precisely so ROMs can be
dragged in beside it. To hand someone a finished game as one file:

```
tar cf mygame.s32 -C mygame .
```

No tool of ours is needed to pack or inspect one. `info.txt` (a line of
`title = ...`) and `icon.bmp` are both optional; without them a game shows
its folder name and a default icon. See `docs/ABI.md` 3.2 for the details.

## License

Firmware: MIT (see LICENSE), with vendored third-party components under
their own licenses: `firmware/libdvi` (BSD-3-Clause, from PicoDVI),
`firmware/tusb_xinput` (MIT), `firmware/fatfs` (no-OS-FatFS, Apache-2.0),
`firmware/tusb_patch` (MIT, patched files from TinyUSB). The `carts/nesf`
example builds against GPL-licensed libretro-fceumm (cloned separately;
see its README).
