# sync32 ROM Format and Syscall ABI (v0.2, pre-1.0 draft)

The permanent contract between games and the console. Everything else in
the platform (firmware, emulator, launcher, SDK tooling) is replaceable;
this document is not. After 1.0, changes are append-only: new API
functions are added to the END of the table with a bumped `api_version`,
new header fields consume reserved space, and old ROMs run forever.

Console name: **sync32** — the machine that waits on vsync, named in
homage to HeatSync Labs, Mesa AZ. ROM extension: **`.s32`**. Magic: `SY32`.

## 0. Reference console

sync32's reference hardware is a SINGLE RP2350: one board, one cable, one
SD card. **Any RP2350 board can qualify**: the console firmware is one
codebase with board support configs (video backend, SD pins, USB) per
board. Board configs may vary the system clock; the cycles-per-frame floor
below is the promise. Games cannot tell boards apart: the ABI is the
console.

## 1. Principles

- **The game is a guest.** It receives an API table and a memory region.
  It never touches hardware registers, never assumes a board, never sees
  whether it runs on one chip or an emulator.
- **One file, everywhere.** A ROM is byte-identical on real hardware and
  in emulators.
- **Native code, no interpreter.** ROM code is ARMv8-M Thumb-2
  (Cortex-M33, DSP + single-precision FPU allowed), AAPCS calling
  convention.

## 2. Platform guarantees

| Resource | Guarantee |
|---|---|
| CPU | at least 2.5 million CPU cycles per 60 Hz frame (150 MHz-equivalent), Cortex-M33 with FPU/DSP. The system clock is a board-config detail; games pace by vsync, never wall-clock |
| Game RAM | 229 KB contiguous at 0x20030000, zero-initialized, exclusively the game's, plus a 16 KB stack above it. The 75 KB above it (0x20069400..0x2007C000) is the console's second scanout buffer, which is what makes present() tear-free. |
| Video | 320x240 (mode 0, 4:3) or 320x180 letterbox (mode 1, 16:9), 60 Hz, square pixels |
| Color | 8bpp indexed sprites through a 256-entry RGB565 palette; solid ops in raw RGB565 |
| Sprites | display-list compositing, up to 128 sprite ops per frame, arbitrary WxH source rects, color-key index 0 |
| Sheets | up to 64 KB of uploaded sprite sheet(s) resident |
| Canvas | optional framebuffer surface at the active resolution, 8bpp indexed |
| Audio | 48 kHz signed 16-bit stereo ring buffer, at least 1024 frames deep (see 6.4) |
| Input | canonical pad is SNES-class digital: dpad, A/B/X/Y, LB/RB, Start/Select — games may assume nothing more. 1 pad guaranteed, up to 4 via hub. Analog axes are reported when hardware has them but NEVER required; the console synthesizes left-stick to dpad bits so any pad plays any game |
| Storage | per-game save blobs on SD (see 6.5); ROM read-only data via pointer |
| Game slot | flash-XIP ROMs up to 3 MB guaranteed on all boards |

The console reserve (compositor, USB, SD, audio, launcher services) lives
OUTSIDE the game region and is not the game's concern.

## 3. ROM file format

Little-endian throughout. File = 64-byte header + payload.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0x00 | 4 | magic | ASCII `"SY32"` |
| 0x04 | 2 | header_version | 1 |
| 0x06 | 2 | api_version | minimum API version required (see 7) |
| 0x08 | 4 | rom_size | total file size in bytes |
| 0x0C | 4 | crc32 | CRC-32 (IEEE) of everything after the header |
| 0x10 | 4 | code_offset | file offset of the binary image |
| 0x14 | 4 | code_size | bytes of code+rodata image |
| 0x18 | 4 | entry_offset | entry point offset within the image (Thumb bit clear; loader sets it) |
| 0x1C | 1 | load_mode | 0 = RAM-load, 1 = flash-XIP (see 4) |
| 0x1D | 1 | video_mode | 0 = 320x240, 1 = 320x180 letterbox |
| 0x1E | 1 | flags | RESERVED, must be 0 |
| 0x1F | 1 | reserved0 | 0 |
| 0x20 | 16 | title | UTF-8, NUL-padded, shown by launcher |
| 0x30 | 8 | game_id | unique id (publisher-chosen); keys save files |
| 0x38 | 4 | icon_offset | file offset of the launcher icon, 0 = none |
| 0x3C | 4 | reserved1 | 0 |

Payload: the flat binary image (code + rodata + data-init template),
produced by the SDK linker script. Assets ship inside the image as rodata.

### 3.1 Launcher icon (optional)

`icon_offset`, when nonzero, points at 512 bytes appended after the code
image (inside the CRC, outside `code_size`, so the loader never maps it):
a 16x16 RGB565 image, row-major, little-endian. The value `0xF81F`
(pure magenta) is reserved as the transparency colorkey; encoders must
nudge a genuinely magenta pixel to `0xF83F`. The launcher shows the icon
at 16x16 (or pixel-doubled to 32x32 for the selected entry) to the left
of the title text; a ROM without an icon gets a default glyph. Existing
ROMs carry zeros here, so the field is optional by construction.
`mks32.py --icon icon.png` embeds one (alpha below 50% becomes the
colorkey).

## 4. Loading model

Two load modes; `mks32.py` picks by linker script, developers choose at
build time (`make MODE=ram|xip`).

**Mode 0: RAM-load.** The launcher copies the image from SD into the
BOTTOM of game RAM and jumps. Zero flash wear, instant start. The image
and the game's state SHARE game RAM: the split is the developer's budget
call; the launcher enforces only physics (image + 16 KB stack must fit).
Link base: `0x20030000`.

**Mode 1: flash-XIP.** For images up to the game slot size. The launcher
streams the image into the flash slot (verified by CRC), then jumps. Code
and rodata execute/read in place from flash (cached); the full game RAM
remains free for state. Repeat launches of the same ROM skip staging.
Link base: `0x10100000`.

In both modes `.data` is copied and `.bss` zeroed by the ROM's own crt0.
There is no dynamic relocation: the link addresses are part of this ABI.

## 5. Entry, lifetime, frame contract

```c
// The one symbol a ROM exports (at entry_offset):
void game_main(const sync32_api_t *api);
```

- `game_main` is called once, on the game's stack (top of game RAM, 16 KB
  guaranteed depth). It never returns normally: it runs the game loop and
  leaves via `api->exit()`.
- **Frame contract:** the game builds a display list and calls
  `api->present()`, which blocks until the next 60 Hz vblank and makes
  that list visible atomically (tear-free). Games pace themselves on
  `present()`; a slow frame drops to 30/20 Hz gracefully rather than
  tearing.
- **Exit paths:** `api->exit()` returns to launcher (clean chip reset).
  A crash/hang trips the watchdog: reboot to launcher. Games cannot brick
  the console.
- Games MUST NOT: disable interrupts, touch the watchdog, write outside
  game RAM and their canvas/sheet handles, or assume addresses beyond
  this document.
- **Games MAY use, as part of the CPU grant:** DSP/SIMD instructions,
  FPU, hardware divide, the DCP double-precision coprocessor, and the
  SIO interpolators (INTERP0/INTERP1) of their core.

## 6. The API table

`sync32_api_t` is a struct of function pointers passed to `game_main`.
Field order is FROZEN; additions append. All functions are called from
the game's context (no reentrancy, no callbacks: games poll). The
authoritative C definition is `include/sync32.h`.

Semantics notes:
- `sprite` flags: bit0 flip-X, bit1 flip-Y. Source rect entirely within
  the sheet; off-screen destinations are clipped.
- `sheet_load` copies the 8bpp data into console sheet memory (64 KB
  total); returns id or <0. Sheets persist until exit. The palette
  applies to all sheets.
- `clear()` fills the canvas IMMEDIATELY: clear, then canvas drawing,
  then present shows the drawing. `canvas()` returns the
  active-resolution 8bpp surface composited UNDER sprites.
  `canvas_mark(y0, y1)` bounds the rows the console must re-composite:
  full-screen marks are legal.
- `save_write` is atomic; slots 0..7, each variable size 0..64 KB (a
  ceiling, not an allocation). `save_read` returns the stored length.
  Keyed by header `game_id`.
- Audio underrun plays silence; games poll `audio_space` and top up.
  48 kHz fixed. The ring holds 1024 frames (~21 ms) guaranteed; a console
  may offer more but never less. **`audio_push` accepts at most
  `audio_space()` frames and silently discards the rest.** That is not an
  error and there is no return value, so a game that pushes one video frame
  of audio (48000/60 = 800 frames) in a single call and moves on will lose
  whatever did not fit and run *under* rate. Because the console's HDMI
  clock-regeneration packets declare 48 kHz, an under-rate stream is a clock
  mismatch, and a sink typically resolves it by muting. Top up in several
  smaller pushes spread across the frame, including while waiting on
  `present()`, so the ring is refilled as the console drains it.
- `video_mode` 1 = 320x180 letterbox: the canvas is interpreted as
  320x180 (rows 0..179), displayed centered with 30-row black bars.
- Disk (api v2): read-only streaming from the game's own data directory,
  `<romname>/` beside the `.s32` file. Plain filenames only — no paths,
  no dotdot. Up to 4 files open. `disk_read` is synchronous: streaming
  games read 16-32 KB per frame and stay inside frame budget.
  `disk_list(index)` enumerates the dir from 0 until ENOENT. Errors:
  EOK 0, ENOENT -1, ENFILE -2, EBADF -3, EIO -4, EINVAL -5. Games that
  require disk set header api_version=2 (`mks32.py --api 2`); the loader
  rejects them on v1 firmware, and v1 games run unchanged on v2.
  Rationale: streaming beats RAM ceilings — the game region plus an SD
  card gives working sets thousands of times larger than RAM.

## 7. Constants

Button bits (XInput order): DPAD_UP 0x0001, DPAD_DOWN 0x0002, DPAD_LEFT
0x0004, DPAD_RIGHT 0x0008, START 0x0010, BACK/SELECT 0x0020, THUMB_L
0x0040, THUMB_R 0x0080, LB 0x0100, RB 0x0200, A 0x1000, B 0x2000,
X 0x4000, Y 0x8000.

api_version 1 = the base table; api_version 2 appends the disk functions.

Deterministic RNG: PCG32, frozen exactly: state advances
`state = state * 6364136223846793005ULL + 1442695040888963407ULL`; output
is `rot32((uint32_t)(((state >> 18) ^ state) >> 27), state >> 59)`
computed on the PRE-advance state, where rot32 is a right-rotate.
`rng_seed(s)` sets `state = 0`, advances once, adds `s`, advances again
(the PCG reference init with the default stream constant).
Implementations MUST match bit-for-bit. `random()` is true hardware
entropy, unrelated to the deterministic stream.

## 8. Deliberately NOT in the ABI (yet)

Tile/scroll layers (sprites cover it; a layer op may append later),
multiple palettes, network, rumble, sheet memory beyond 64 KB, callbacks,
threads.

## 8b. Designed appends (signatures reserved, not yet implemented)

The affine family — implemented in the compositor so every game gets
SNES-Mode-7-class effects as console features:

```c
  // scaled/rotated sprite: 2x2 fixed-point matrix (8.8), source rect as sprite()
  void (*sprite_affine)(int sheet, int sx, int sy, int w, int h,
                        int x, int y, int16_t m[4], uint8_t flags);
  // full-width textured plane between rows y0..y1: per-ROW table of
  // {u0, v0, du, dv} in 12.4 fixed point — raster effects, horizons,
  // and perspective come from varying the table per row.
  void (*plane_affine)(int sheet, int y0, int y1, const int16_t *row_uv);
```
