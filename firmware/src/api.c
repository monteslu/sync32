// sync32 API implementation + game launch/exit machinery.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "hardware/watchdog.h"
#include "hdmi_island.h"
#include "hardware/structs/scb.h"
#include "sync32.h"
#include "video.h"

// ---- input (filled by usb host or stub) ----
volatile s32_pad_t s32_pads[4];

// ---- system-level exit combo (SELECT+START, ~1s) ----
// Console-level, deliberately: a game-level handler cannot rescue a hung
// game, and every cart would have to remember to implement it. Called from
// the USB input path on every controller report.
#define EXIT_HOLD_REPORTS 120          // xinput reports ~125/s => ~1 second
void s32_check_exit_combo(uint16_t buttons) {
    static uint16_t held;
    if ((buttons & (S32_PAD_SELECT | S32_PAD_START)) ==
        (S32_PAD_SELECT | S32_PAD_START)) {
        if (++held >= EXIT_HOLD_REPORTS) {
            held = 0;
            watchdog_hw->scratch[3] = 0x505AD000u;   // come back in pad mode
            watchdog_reboot(0, 0, 10);               // -> launcher
            while (1) tight_loop_contents();
        }
    } else {
        held = 0;
    }
}

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
    // clean chip reset back into the launcher, same usb mode
    bool s32_usb_host_active(void);
    watchdog_hw->scratch[3] = s32_usb_host_active() ? 0x505AD000u : 0;
    watchdog_hw->scratch[5] = 0;          // no crash breadcrumb
    watchdog_reboot(0, 0, 200);
    while (1) tight_loop_contents();
}

// ---- audio: 48kHz stereo out over HDMI data islands ----
// The game pushes PCM into this ring; the video scanline builder drains it
// one 4-frame packet at a time into the horizontal blanking interval.
// Sized to the largest that still fits firmware RAM. The game bursts ~800
// frames every 16.7ms while the scanline pump drains at most 4 frames per
// active line, so this ring is the buffer across the vertical blanking gap
// where no islands are built.
#define AUD_RING 704                       // stereo frames = 14.7ms at 48kHz
static int16_t aud_ring[AUD_RING * 2];
static volatile uint32_t aud_w, aud_r;

static int api_audio_space(void) {
    uint32_t used = aud_w - aud_r;
    return (int)(AUD_RING - used);
}
static void api_audio_push(const int16_t *lr, int frames) {
    if (frames <= 0) return;
    uint32_t space = AUD_RING - (aud_w - aud_r);
    if ((uint32_t)frames > space) frames = (int)space;
    for (int i = 0; i < frames; i++) {
        uint32_t s = (aud_w + i) % AUD_RING;
        aud_ring[s * 2] = lr[i * 2];
        aud_ring[s * 2 + 1] = lr[i * 2 + 1];
    }
    aud_w += frames;
    hdmi_island_arm(true);                 // audio is live: schedule islands
}

// Called from the scanline builder: pull up to 4 frames for one packet.
// Returns the number of frames written to out (0 = ring empty).
int s32_audio_take(int16_t *out, int max_frames) {
    uint32_t avail = aud_w - aud_r;
    int n = (int)(avail < (uint32_t)max_frames ? avail : (uint32_t)max_frames);
    for (int i = 0; i < n; i++) {
        uint32_t s = (aud_r + i) % AUD_RING;
        out[i * 2] = aud_ring[s * 2];
        out[i * 2 + 1] = aud_ring[s * 2 + 1];
    }
    aud_r += n;
    return n;
}

// ---- saves: wired to SD when present (weak stubs replaced by sdcard.c) ----
int __attribute__((weak)) s32_save_read(int slot, void *buf, int max) {
    (void)slot; (void)buf; (void)max; return -1;
}
int __attribute__((weak)) s32_save_write(int slot, const void *buf, int len) {
    (void)slot; (void)buf; (void)len; return -1;
}

static int api_sheet_load(const void *p, int w, int h) { return video_sheet_load(p, w, h); }

// api v2 disk (disk.c)
int s32_disk_list(int index, char *name_out, uint32_t *size_out);
int s32_disk_open(const char *name);
int s32_disk_size(int fd);
int s32_disk_seek(int fd, uint32_t offset);
int s32_disk_read(int fd, void *dst, uint32_t len);
int s32_disk_close(int fd);

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
    .disk_list = s32_disk_list,
    .disk_open = s32_disk_open,
    .disk_size = s32_disk_size,
    .disk_seek = s32_disk_seek,
    .disk_read = s32_disk_read,
    .disk_close = s32_disk_close,
};

// jump to a loaded game: PSP = game stack, thread mode switches to PSP so
// IRQs keep running on the firmware's MSP stack.
__attribute__((noreturn)) static void enter_game_asm(uint32_t entry);
void s32_enter_game(uint32_t entry) {   // also used by the XIP boot path
    video_sheet_reset();
    enter_game_asm(entry);
}
__attribute__((noreturn)) static void enter_game_asm(uint32_t entry) {
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
extern volatile uint8_t s32_video_mode;
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
        s32_video_mode = h->video_mode == 1 ? 1 : 0;
        if (code_size > 0x50000 - 0x4000) return -6;
        memmove((void *)0x20030000, rom + code_offset, code_size);
        enter_game_asm(0x20030000 + entry_offset);
    }
    return -7;   // XIP mode handled by flash-slot path (M3)
}
