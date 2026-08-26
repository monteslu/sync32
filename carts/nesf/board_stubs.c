// Stubs for mappers whose static buffers (0.5-1.5MB) cannot fit on-chip:
// JY Company ASIC (90/209/211/...), UNROM512 (30), 3D Block (355), and the
// VRC7 OPLL synth. Loading such a ROM shows fceumm's "unsupported" path
// instead of linking half a megabyte of dead bss.
#include <stddef.h>
#include "fceu-types.h"
#include "fceu.h"
#include "cart.h"

static void no_board(CartInfo *info) {
    (void)info;
    FCEU_PrintError("mapper not in this build");
}
#define STUB(n) void n(CartInfo *info) { no_board(info); }
STUB(Mapper90_Init) STUB(Mapper209_Init) STUB(Mapper211_Init)
STUB(Mapper281_Init) STUB(Mapper282_Init) STUB(Mapper295_Init)
STUB(Mapper358_Init) STUB(Mapper35_Init) STUB(Mapper386_Init)
STUB(Mapper387_Init) STUB(Mapper388_Init) STUB(Mapper394_Init)
STUB(Mapper397_Init) STUB(Mapper421_Init)
STUB(UNL3DBlock_Init) STUB(UNROM512_Init)

// VRC7 audio chip: silent stub (VRC7 games run without expansion audio)
typedef struct OPLL OPLL;
OPLL *OPLL_new(uint32_t clk, uint32_t rate) { (void)clk; (void)rate; return NULL; }
void OPLL_delete(OPLL *o) { (void)o; }
void OPLL_reset(OPLL *o) { (void)o; }
void OPLL_set_rate(OPLL *o, uint32_t r) { (void)o; (void)r; }
void OPLL_writeReg(OPLL *o, uint32_t reg, uint32_t val) { (void)o; (void)reg; (void)val; }
void OPLL_forceRefresh(OPLL *o) { (void)o; }
void OPLL_fillbuf(OPLL *o, int32_t *buf, int32_t len, int shift) { (void)o; (void)buf; (void)len; (void)shift; }
