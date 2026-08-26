// sync32 API implementation + game launch/exit machinery.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/watchdog.h"
#include "hardware/structs/scb.h"
#include "sync32.h"
#include "video.h"

// ---- input (filled by usb host or stub) ----
volatile s32_pad_t s32_pads[4];

static void api_pad(int player, s32_pad_t *out) {
    if (player < 0 || player > 3) { memset(out, 0, sizeof *out); return; }
    *out = *(const s32_pad_t *)&s32_pads[player];
    // stick-to-dpad synthesis: any pad plays any game
    if (out->lx < -40) out->buttons |= S32_PAD_LEFT;
    if (out->lx >  40) out->buttons |= S32_PAD_RIGHT;
    if (out->ly >  40) out->buttons |= S32_PAD_UP;     // xinput Y up = +
    if (out->ly < -40) out->buttons |= S32_PAD_DOWN;
}

// ---- deterministic PCG32 (frozen algorithm, see spec) ----
static uint64_t pcg_state;
static uint32_t pcg_out(uint64_t s) {
    uint32_t x = (uint32_t)(((s >> 18) ^ s) >> 27);
    uint32_t r = (uint32_t)(s >> 59);
    return (x >> r) | (x << ((32 - r) & 31));
}
static void api_rng_seed(uint64_t seed) {
    pcg_state = 0;
    pcg_state = pcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    pcg_state += seed;
    pcg_state = pcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
}
static uint32_t api_rng_next(void) {
    uint64_t old = pcg_state;
    pcg_state = pcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return pcg_out(old);
}

static uint32_t api_random(void) { return get_rand_32(); }
static uint32_t api_ticks_us(void) { return time_us_32(); }

static void api_exit(void) {
    // clean chip reset back into the launcher
    watchdog_hw->scratch[5] = 0;          // no crash breadcrumb
    watchdog_reboot(0, 0, 10);
    while (1) tight_loop_contents();
}

// ---- audio stub (v1: silent console; ring accepts and discards) ----
static int api_audio_space(void) { return 4096; }
static void api_audio_push(const int16_t *lr, int frames) { (void)lr; (void)frames; }

// ---- saves: wired to SD when present (weak stubs replaced by sdcard.c) ----
int __attribute__((weak)) s32_save_read(int slot, void *buf, int max) {
    (void)slot; (void)buf; (void)max; return -1;
}
int __attribute__((weak)) s32_save_write(int slot, const void *buf, int len) {
    (void)slot; (void)buf; (void)len; return -1;
}

static int api_sheet_load(const void *p, int w, int h) { return video_sheet_load(p, w, h); }

static const sync32_api_t api_table = {
    .api_version = S32_API_VERSION,
    .exit = api_exit,
    .ticks_us = api_ticks_us,
    .random = api_random,
    .rng_seed = api_rng_seed,
    .rng_next = api_rng_next,
    .palette_set = video_palette,
    .sheet_load = api_sheet_load,
    .clear = video_clear,
    .sprite = video_sprite,
    .rect = video_rect,
    .canvas = video_canvas,
    .canvas_mark = video_canvas_mark,
    .present = video_present,
    .pad = api_pad,
    .audio_space = api_audio_space,
    .audio_push = api_audio_push,
    .save_read = s32_save_read,
    .save_write = s32_save_write,
};

// jump to a loaded game: PSP = game stack, thread mode switches to PSP so
// IRQs keep running on the firmware's MSP stack.
__attribute__((noreturn)) static void enter_game(uint32_t entry) {
    // ensure FPU (CP10/CP11 full access) for the game context
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
    __asm volatile("dsb; isb");
    __asm volatile(
        "msr psp, %[stack]\n"
        "mrs r3, CONTROL\n"
        "orr r3, r3, #2\n"
        "msr CONTROL, r3\n"
        "isb\n"
        "mov r0, %[api]\n"
        "bx  %[entry]\n"
        :: [stack] "r" (0x20080000),
           [api]   "r" (&api_table),
           [entry] "r" (entry | 1)
        : "r0", "r3");
    __builtin_unreachable();
}

// validate + launch a .s32 image sitting in a buffer (RAM mode) or XIP slot
#include "s32crc.h"
int s32_launch(const uint8_t *rom, uint32_t size) {
    if (size < 64) return -1;
    const s32_header_t *h = (const s32_header_t *)rom;
    if (h->magic != S32_MAGIC) return -2;
    if (h->header_version != 1 || h->api_version > S32_API_VERSION) return -3;
    if (h->rom_size != size || h->code_offset + h->code_size > size) return -4;
    if (s32_crc32(rom + 64, size - 64) != h->crc32) return -5;
    video_sheet_reset();
    if (h->load_mode == S32_LOAD_RAM) {
        // rom may BE the game region (SD stages there): the memmove clobbers
        // the header, so lift every field we still need first
        uint32_t code_offset = h->code_offset, code_size = h->code_size;
        uint32_t entry_offset = h->entry_offset;
        if (code_size > 0x50000 - 0x4000) return -6;
        memmove((void *)0x20030000, rom + code_offset, code_size);
        enter_game(0x20030000 + entry_offset);
    }
    return -7;   // XIP mode handled by flash-slot path (M3)
}
