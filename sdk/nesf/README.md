# nesf: fceumm as a sync32 XIP cart

Same NES core as romdev (libretro-fceumm @ 3a84a6fd, the romdev pin).

## Setup
```
git clone https://github.com/libretro/libretro-fceumm.git fceumm
git -C fceumm checkout 3a84a6fd0ba20dd4877c06b1d58741172148395f
git -C fceumm apply ../fceumm-sync32.patch
make
```

## The patch (fceumm-sync32.patch)
Behavior-preserving surgery to fit a 520KB-SRAM chip:
- two-level CPU bus dispatch (page-uniform + shared byte subtables)
  replacing the flat 512KB ARead/BWrite function-pointer tables
- WaveHi (160KB hi-quality sound buffer) allocated lazily; quality
  falls back to 0 when there is no RAM for it
- XBuf static, XDBuf reduced to a dummy row (its only reader is the
  libretro driver, which this cart replaces with main.c)
- const on big lookup tables (sound filter coeffs, palettes, ines
  board map) so they stay in XIP flash instead of .data RAM
- mappers with half-megabyte static buffers stubbed out (JY ASIC,
  UNROM512, 3D Block, VRC7's OPLL synth): see board_stubs.c

ROM cap 76KB in v1 (file streams through the canvas buffer during
load). Bigger ROMs need the planned flash ROM-staging syscall.
