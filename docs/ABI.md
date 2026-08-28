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
| Game RAM | 304 KB contiguous at 0x20030000 (0x20030000..0x2007C000), zero-initialized, exclusively the game's, plus a 16 KB stack above it. The console reserves nothing inside it. |
| Video | 320x240 (mode 0, 4:3) or 320x180 letterbox (mode 1, 16:9), 60 Hz, square pixels |
| Color | 8bpp indexed sprites through a 256-entry RGB565 palette; solid ops in raw RGB565 |
| Sprites | display-list compositing, up to 128 sprite ops per frame, arbitrary WxH source rects, color-key index 0 |
| Sheets | up to 8 sheets, of any size, stored by the GAME (see 6.2): the console keeps no sheet memory of its own |
| Canvas | optional framebuffer surface at the active resolution, 8bpp indexed |
| Audio | 48 kHz signed 16-bit stereo ring buffer, at least 1024 frames deep (see 6.4) |
| Input | canonical pad is SNES-class digital: dpad, A/B/X/Y, LB/RB, Start/Select — games may assume nothing more. 1 pad guaranteed, up to 4 via hub. Analog axes are reported when hardware has them but NEVER required; the console synthesizes left-stick to dpad bits so any pad plays any game |
| Storage | per-game save blobs on SD (see 6.5); ROM read-only data via pointer |
| Game slot | flash-XIP ROMs up to 3 MB guaranteed on all boards; a game ships as one `.s32` file or as a folder holding `main.s32e` (see 3.2) |

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
colorkey). A game may instead ship an `icon.bmp` in its own namespace, which
takes precedence and needs no build step at all: see 3.3.

### 3.2 Game forms: folder and archive

A game reaches the console in one of two forms, and **a game cannot tell
which one it is running from**. Both give it the same two things: an
executable, and a private namespace of named resources it reads through the
disk API (6.6).

| Form | Is | Suits |
|---|---|---|
| Folder | a directory containing `main.s32e` | working on a game, and anything the player adds files to |
| Archive | one `NAME.s32` file: an uncompressed **tar** of that same directory | shipping a finished game as one file |

**A directory is a game if and only if it contains `main.s32e`.** The name is
fixed, the way `default.xbe` is on Xbox: the launcher does one stat rather
than walking the directory, which matters because it rescans the card
periodically. Everything else in the directory is that game's resources.

`.s32` and `.s32e` are different extensions because they are different kinds
of object. A `.s32` is a complete, launchable game wherever it sits. A
`.s32e` is an executable and is never launched on its own; on its own it is a
component, the way a `.xbe` is outside its disc image. One extension for both
would make position decide meaning ("a `.s32` at the root is a game, a `.s32`
in a folder is not"), a rule that cannot be stated in one sentence and breaks
as soon as a file is moved.

`main.s32e` carries the same 64-byte header as any `.s32`, so everything
about how the code is *loaded* comes from the executable: **`load_mode`
(RAM-load or flash-XIP)**, `video_mode`, `api_version`, `entry_offset` and
`code_size`. Those are properties of the code and cannot be anywhere else. A
folder-form game therefore chooses its load mode exactly as a single-file one
does.

What the launcher *displays* is separate, and comes from files in the
namespace: see 3.3. That split is what lets an author reuse a prebuilt
`main.s32e` and still ship their own game.

Packing is `tar cf game.s32 -C gamedir .` and unpacking is `tar xf`. The
console requires no tool of ours to produce a playable archive, and a player
can look inside one with software they already have. `mks32.py` still builds
the *executable*; it is not needed to package a game.

A card mixing both forms is the expected case:

```
/
  chromium.s32          a game, one file (a tar)
  playground.s32
  nes/                  a game, as a folder
    main.s32e             its executable
    smb.nes               the player's own files, dropped in from a PC
    metroid.nes
  doom/
    main.s32e
    doom1.wad
```

The launcher lists all of these together, each showing the title and icon
from its header.

### 3.3 Identity: what the launcher shows

A game's title, icon and save identity come from files in its own namespace,
not from the executable's header. That is deliberate: an author who reuses
somebody else's prebuilt `main.s32e` (a Lua or BASIC runtime, say, never
touching a compiler) cannot change a header they did not build, and the
header would otherwise name the runtime rather than their game.

Because identity lives in the namespace, it is identical in both forms. A
folder and a tar of that folder carry the same files and the launcher reads
them the same way, so `tar cf` stays a pure repack.

Everything here is optional and everything is by fixed name. There is
nothing to declare and no paths to get wrong:

| File | Is |
|---|---|
| `info.txt` | `key = value` lines, one per line. Only `title` is defined so far. |
| `icon.bmp` | a 16x16 BMP shown beside the title |

```
cavecrawl/
  main.s32e     the executable      (fixed name, required)
  info.txt      title = Cave Crawler
  icon.bmp      16x16
  main.lua      whatever else the game needs
```

A game with no resources at all is just `main.s32e` and perhaps `info.txt`.

**`game_id` is derived, never written.** It is the name of the game with any
extension removed: folder `cavecrawl/` and archive `cavecrawl.s32` both
yield `cavecrawl`. An opaque identifier an author has to invent is one they
will leave blank or duplicate, and it keys save files, so it has to be right
without anyone thinking about it. Renaming a game therefore moves its saves;
the `.s32id` marker (6.6) is the escape hatch when that is not wanted.

`info.txt` is parsed as literally as it looks: split each line at the first
`=`, trim spaces from both sides, ignore blank lines and anything starting
with `#`, ignore any line that does not parse or names a key we do not know.
There is no nesting, no quoting and no types. It is editable in any text
editor on any machine, and a malformed file costs a default rather than an
error. JSON and YAML were considered and rejected: both need a real parser
(JSON is 500+ lines done properly, and there is no small correct YAML
parser), and both fail hard on the kind of mistake a person makes in a text
editor, which is the wrong trade for a file whose only job is to supply a
display name.

`icon.bmp` is read only if it is a 16x16 uncompressed BMP with a
`BITMAPINFOHEADER` at 24 or 32 bits per pixel. Anything else, including a
missing or truncated file, is ignored and the launcher draws its default
glyph. An icon is decoration, so every failure is silent rather than an
error worth reporting. BMP is used because it is the format a person can
actually produce: every paint program on every desktop exports it, it is
uncompressed so decoding is a header check and a pixel loop, and unlike a
raw RGB565 blob it needs no tool of ours to create.

Where an executable does carry header values (title, and an icon at
`icon_offset`), they are used only when the corresponding file is absent, so
a cart that builds its own `main.s32e` needs no extra files and behaves
exactly as before.

`main.s32e`, `info.txt`, `icon.bmp` and `.s32id` are the console's own files
and are invisible to `disk_open` and `disk_list`.

### 3.4 The archive form is a tar

A `.s32` archive is an ordinary uncompressed tar (USTAR). No compression is
permitted: a compressed member cannot be executed in place and would have to
be inflated through RAM the console does not have to spare.

Tar is used because its layout already has the properties this console needs,
rather than because the format is familiar:

- Each member is preceded by its own 512-byte header and is **contiguous and
  512-aligned**. The XIP staging path streams `main.s32e` as one unbroken run
  and never seeks, exactly as it does for a bare `.s32` today.
- The headers are **inline, before each member**. There is no index at the end
  of the file to seek backwards for.
- Members are stored, never compressed, so there is no decompressor and no
  variant of the format to reject.

There is deliberately **no index member**. An index would have to be written
by our tooling, which would defeat the point of using a format anyone can
produce, and it would not pay for itself: an index large enough to matter
(200 entries is about 8 KB) does not fit the firmware's heap, so the console
would walk the tar regardless. Walking is cheap for what archives actually
contain. Locating a member costs one 512-byte header read plus a seek past
the member's data, so a game with twenty resources resolves a name in single-
digit milliseconds, once, at launch. The case that would be slow, a library of
hundreds of files, belongs in the folder form, where listing is a native
directory read.

`main.s32e` should be the first member so the loader finds it immediately,
but it is located by name, so an archive whose members are in any order still
works. A few details follow from how real `tar` behaves, and the loader
absorbs them rather than requiring the packer to get them right:

- A leading `./` on member names is ignored. `tar cf game.s32 -C dir .`
  writes `./main.s32e`; `cd dir && tar cf ../game.s32 *` writes
  `main.s32e`. Both are valid archives.
- Directory members (typeflag `5`) and any member that is not a regular
  file (typeflag `0` or `\0`) are skipped. Real tar emits directory
  entries; they carry no data and are not resources.
- Members named `PaxHeader/...`, or with typeflag `x`, `g`, `L` or `K`, are
  metadata some tar implementations emit. They are skipped too.

A `.s32` may therefore be either a tar or the older raw form (a bare header
followed by its code image). One 512-byte read tells them apart with no
guessing: the raw form has `"SY32"` at offset 0, and a tar has `"ustar"` at
offset 257 with a filename at offset 0. The loader accepts both:

```
read 512 bytes
  magic "SY32" at 0        -> raw cart: header is right here
  "ustar" at 257           -> tar: walk to main.s32e, use its header
  neither                  -> not a game
```

Both forms end at the same place: a 64-byte header describing one contiguous
code image, which is what the loader and the XIP staging path already
consume.

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

The load mode is a property of the executable, not of the form the game
shipped in: `load_mode` is header field 0x1C, and `main.s32e` carries the
same header as a bare `.s32`. A folder-form game therefore chooses RAM-load
or flash-XIP exactly as a single-file one does. In every case the loader
consumes one contiguous code image, whether it reads it from a raw `.s32`,
from a tar member, or from a file in a folder.

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
- `sheet_load(px, w, h)` registers 8bpp pixel data the GAME owns and returns
  a sheet id (0..7) or <0. The console copies nothing and keeps no arena of
  its own, so `px` must stay valid and unchanged for as long as the sheet is
  drawn. The usual case is a `const` array baked into the cart, which lives
  in flash and costs no RAM at all; that also means a sheet can be as large
  as the cart rather than capped by a console buffer. Sheets reset on exit.
  The palette applies to all sheets.
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
- Disk (api v2): read-only access to the game's own resource namespace
  (3.2). The same names work whichever form the game shipped in: against a
  folder they are paths beneath it, against an archive they are member names
  in the tar. A game never learns which.
  Names may contain `/`, so a game can organise its own resources
  (`roms/smb.nes`), but never `..`, a leading `/`, a drive letter or a
  backslash: the namespace is a sandbox with no route out of it. `main.s32e`
  and `.s32id` are the console's own files and are invisible to both
  `disk_open` and `disk_list`.
  For a folder-form game the namespace is that folder. A bare `.s32` may
  also carry a sidecar `<romname>/` beside it, and that directory can be
  bound by `game_id` instead of by name so renaming the ROM does not orphan
  its content: a directory claims a game by holding a file named `.s32id`
  with the 8 raw `game_id` bytes. The basename is tried first, so the
  ordinary layout needs no marker. Where a sidecar folder and an archive
  member could both answer a name, **the folder wins**, so an asset can be
  replaced for testing without repacking.
  Up to 4 files open. `disk_read` is synchronous and blocks the caller:
  streaming games read 16-32 KB per frame and stay inside frame budget.
  `disk_list(index)` enumerates ONE level from 0 until ENOENT. A directory
  is reported with a trailing `/` and a size of 0, which keeps the call's
  frozen signature; a game that wants a tree walks it deliberately, because
  a recursive scan of a large card is not something the console should do
  behind a game's back.
  Errors: EOK 0, ENOENT -1, ENFILE -2, EBADF -3, EIO -4, EINVAL -5. Games
  that require disk set header api_version=2 (`mks32.py --api 2`); the
  loader rejects them on v1 firmware, and v1 games run unchanged on v2.
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
