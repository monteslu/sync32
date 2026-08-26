/* sync32 flash-XIP ROM: code+rodata run in place from the game slot,
   .data copied to game RAM by crt0, .bss zeroed there */
MEMORY {
  SLOT (rx)  : ORIGIN = 0x10100000, LENGTH = 12M
  GAME (rwx) : ORIGIN = 0x20030000, LENGTH = 0x50000 - 0x4000
}
ENTRY(_start)
SECTIONS {
  .text : { KEEP(*(.text._start)) *(.text*) *(.rodata*) . = ALIGN(4); } > SLOT
  .data : { __data_start__ = .; *(.data*) . = ALIGN(4); __data_end__ = .; } > GAME AT > SLOT
  __data_load__ = LOADADDR(.data);
  .bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end__ = .; } > GAME
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
