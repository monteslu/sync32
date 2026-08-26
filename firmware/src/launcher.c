// sync32 launcher: boot menu. Lists .s32 files from SD, pad/serial navigation,
// loads the selection. Renders with the console's own display-list engine.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "sync32.h"
#include "video.h"
#include "sdcard.h"

extern const uint8_t Font8_Table[];   // 5x8 small font
extern const uint8_t Font16_Table[];  // 11x16 brand font
extern const uint8_t Font12_Table[];  // 7x12 body font, 12 bytes/glyph
extern volatile s32_pad_t s32_pads[4];
extern int s32_launch(const uint8_t *rom, uint32_t size);
extern const uint8_t embedded_rom[];
extern const unsigned embedded_rom_len;

#define ROM_BUF ((uint8_t *)0x20030000)          // stage into game RAM
#define ROM_BUF_MAX (0x50000 - 0x4000)

// UI palette indexes (set once in launcher_run)
enum { C_BG = 0, C_TEXT = 1, C_TITLE = 2, C_WARN = 3, C_DIM = 4,
       C_BAR = 5, C_SEL = 6, C_SELTEXT = 7, C_OK = 8 };

static void px_rect(int x, int y, int w, int h, uint8_t ci) {
    uint8_t *fb = video_canvas();
    for (int yy = y; yy < y + h && yy < 240; yy++) {
        if (yy < 0) continue;
        int xa = x < 0 ? 0 : x, xb = x + w > 320 ? 320 : x + w;
        if (xa < xb) memset(&fb[yy * 320 + xa], ci, xb - xa);
    }
}
static void draw_char(int x, int y, char c, uint8_t ci) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = &Font8_Table[(c - 32) * 8];
    uint8_t *fb = video_canvas();
    for (int r = 0; r < 8; r++) {
        if (y + r < 0 || y + r >= 240) continue;
        for (int b = 0; b < 5; b++)
            if (g[r] & (0x80 >> b) && x + b >= 0 && x + b < 320)
                fb[(y + r) * 320 + x + b] = ci;
    }
}
static void draw_text(int x, int y, const char *s, uint8_t ci) {
    for (; *s; s++, x += 6) draw_char(x, y, *s, ci);
}
static void draw_char16(int x, int y, char c, uint8_t ci) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = &Font16_Table[(c - 32) * 32];
    uint8_t *fb = video_canvas();
    for (int r = 0; r < 16; r++) {
        if (y + r < 0 || y + r >= 240) continue;
        uint16_t row = (g[r * 2] << 8) | g[r * 2 + 1];
        for (int b = 0; b < 11; b++)
            if (row & (0x8000 >> b) && x + b >= 0 && x + b < 320)
                fb[(y + r) * 320 + x + b] = ci;
    }
}
static void draw_text16(int x, int y, const char *s, uint8_t ci) {
    for (; *s; s++, x += 11) draw_char16(x, y, *s, ci);
}
static void draw_char12(int x, int y, char c, uint8_t ci) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = &Font12_Table[(c - 32) * 12];
    uint8_t *fb = video_canvas();
    for (int r = 0; r < 12; r++) {
        if (y + r < 0 || y + r >= 240) continue;
        for (int b = 0; b < 7; b++)
            if (g[r] & (0x80 >> b) && x + b >= 0 && x + b < 320)
                fb[(y + r) * 320 + x + b] = ci;
    }
}
static void draw_text12(int x, int y, const char *s, uint8_t ci) {
    for (; *s; s++, x += 8) draw_char12(x, y, *s, ci);
}

static uint16_t pad_edge(void) {
    static uint16_t prev;
    uint16_t now = s32_pads[0].buttons;
    // stick synthesis for menu too
    if (s32_pads[0].lx < -40) now |= S32_PAD_LEFT;
    if (s32_pads[0].lx >  40) now |= S32_PAD_RIGHT;
    if (s32_pads[0].ly >  40) now |= S32_PAD_UP;
    if (s32_pads[0].ly < -40) now |= S32_PAD_DOWN;
    uint16_t edge = now & ~prev;
    prev = now;
    return edge;
}

void launcher_run(void) {
    static uint16_t pal[256];
    pal[C_BG] = 0x10A2;      // deep blue-gray
    pal[C_TEXT] = 0xFFFF;    // white
    pal[C_TITLE] = 0x07FF;   // cyan
    pal[C_WARN] = 0xFD20;    // amber
    pal[C_DIM] = 0x8410;     // gray
    pal[C_BAR] = 0x2124;     // header bar
    pal[C_SEL] = 0x2965;     // selection bar
    pal[C_SELTEXT] = 0xFFE0; // yellow
    pal[C_OK] = 0x07E0;      // green
    video_palette(pal);
    static rom_entry_t roms[32];   // 2.2KB: off the stack
    int n = -1, sel = 0, scroll = 0;
    int sd_state = -99;                            // force first scan
    void s32_xip_boot_check(void);
    s32_xip_boot_check();                 // reboot-from-staging lands here
    uint32_t rescan_at = 0;
    // dual-role usb plumbing (usb_input.c / tinyusb)
    bool s32_usb_host_active(void);
    int s32_usb_pads_mounted(void);
    uint32_t s32_usb_last_mount_ms(void);
    bool tud_connected(void);
    while (1) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (!s32_usb_host_active()) {
            // device probe: no PC on the USB port after 2.5s -> pad mode
            if (now_ms > 2500 && !tud_connected()) {
                watchdog_hw->scratch[3] = 0x505AD000u;
                watchdog_reboot(0, 0, 200);
                while (1) tight_loop_contents();
            }
        } else if (s32_usb_pads_mounted() == 0 &&
                   now_ms - s32_usb_last_mount_ms() > 15000) {
            // host mode but nothing attached for a while: the cable may now
            // be a PC again; bounce through the device probe to find out
            watchdog_reboot(0, 0, 200);
            while (1) tight_loop_contents();
        }
        if (video_frame_count() >= rescan_at) {
            rescan_at = video_frame_count() + 120;  // rescan every 2s
            int r = sd_list_roms(roms, 32);
            if (r != sd_state) { sd_state = r; sel = 0; scroll = 0; }
            n = r;
        }
        uint16_t e = pad_edge();
        int cc = getchar_timeout_us(0);            // serial drive for the lab
        if (cc == 'h') {                       // diag: force host mode now
            watchdog_hw->scratch[3] = 0x505AD000u;
            watchdog_reboot(0, 0, 200);
            while (1) tight_loop_contents();
        }
        if (e & S32_PAD_DOWN || cc == 'j') sel++;
        if (e & S32_PAD_UP || cc == 'k') sel--;
        if (n > 0) { if (sel < 0) sel = n - 1; if (sel >= n) sel = 0; }
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + 13) scroll = sel - 12;

        px_rect(0, 0, 320, 240, C_BG);
        px_rect(0, 0, 320, 22, C_BAR);
        draw_text16(6, 4, "sync32", C_TITLE);
        draw_text12(96, 6, "select a game", C_TEXT);
        if (s32_usb_pads_mounted() > 0) draw_text12(292, 6, "PAD", C_OK);
        else if (!s32_usb_host_active() && tud_connected()) draw_text12(299, 6, "PC", C_DIM);
        char buf[64];
        snprintf(buf, sizeof buf, "boot %lu %s crash %08lx",
                 (unsigned long)watchdog_hw->scratch[2],
                 s32_usb_host_active() ? "pad-mode" : "pc-mode",
                 (unsigned long)watchdog_hw->scratch[7]);
        draw_text(6, 214, buf, C_DIM);
        if (n < 0) {
            draw_text12(64, 102, "insert an SD card with .s32 files", C_WARN);
            draw_text12(48, 126, "(FAT32 or exFAT, format on a PC)", C_DIM);
        } else if (n == 0) {
            draw_text12(76, 108, "no .s32 files on card", C_WARN);
        } else {
            for (int i = scroll; i < n && i < scroll + 13; i++) {
                int y = 30 + (i - scroll) * 15;
                if (i == sel) px_rect(4, y - 2, 312, 15, C_SEL);
                snprintf(buf, sizeof buf, "%-16.16s", roms[i].title);
                draw_text12(10, y, buf, i == sel ? C_SELTEXT : C_TEXT);
                snprintf(buf, sizeof buf, "%4luK", (unsigned long)(roms[i].size + 1023) / 1024);
                draw_text12(276, y, buf, i == sel ? C_SELTEXT : C_DIM);
            }
        }
        draw_text12(6, 224, "A: play   X: demo   Y: usb drive", C_DIM);
        video_present();
        if ((video_frame_count() % 120) == 0)
            watchdog_hw->scratch[0] = 0;   // forensics: alive
            printf("HEARTBEAT vframe=%lu roms=%d\n",
                   (unsigned long)video_frame_count(), n);

        if ((e & S32_PAD_X) || cc == 'd') {        // built-in demo
            s32_launch(embedded_rom, embedded_rom_len);
        }
        if (((e & S32_PAD_Y) || cc == 'u') && !s32_usb_host_active()) {   // USB thumb-drive mode (needs a PC)
            void s32_msc_reset_eject(void);
            extern volatile bool s32_msc_ejected_flag;
            extern volatile uint32_t s32_msc_writes;
            void sd_unmount_for_msc(void);
            extern volatile bool s32_msc_mode;
            sd_unmount_for_msc();
            s32_msc_reset_eject();
            s32_msc_mode = true;
            printf("USB drive mode: copy .s32 files, eject, then press B\n");
            while (1) {
                uint16_t e2 = pad_edge();
                int c2 = getchar_timeout_us(0);
                px_rect(0, 0, 320, 240, C_BG);
                px_rect(0, 0, 320, 22, C_BAR);
                draw_text16(6, 4, "sync32", C_TITLE);
                draw_text12(96, 6, "USB drive mode", C_SELTEXT);
                draw_text12(48, 88, "this console is now a thumb drive", C_TEXT);
                draw_text12(48, 104, "copy .s32 games onto it from a PC", C_TEXT);
                char st[48];
                extern volatile uint32_t s32_msc_writes;
                snprintf(st, sizeof st, "writes: %lu   %s",
                         (unsigned long)s32_msc_writes,
                         s32_msc_ejected_flag ? "EJECTED: safe to leave" : "mounted");
                draw_text(61, 130, st, s32_msc_ejected_flag ? C_OK : C_DIM);
                draw_text(73, 160, "eject on the PC, then press B", C_WARN);
                video_present();
                if ((e2 & S32_PAD_B) || c2 == 'b') break;
            }
            s32_msc_mode = false;
            sd_state = -99; rescan_at = 0;         // force remount + rescan
        }
        if (((e & S32_PAD_A) || cc == '\r' || cc == '\n' || cc == 'p') && n > 0) {
            uint32_t size;
            printf("loading %s...\n", roms[sel].name);
            if (roms[sel].xip) {
                void s32_disk_set_dir(const char *rom_filename);
                s32_disk_set_dir(roms[sel].name);
                sd_set_game_id(roms[sel].game_id);
                int s32_xip_stage_and_launch(const char *filename);
                int xr = s32_xip_stage_and_launch(roms[sel].name);
                printf("xip launch failed: %d\n", xr);
                continue;
            }
            if (sd_read_rom(roms[sel].name, ROM_BUF, ROM_BUF_MAX, &size) == 0) {
                const s32_header_t *h = (const s32_header_t *)ROM_BUF;
                sd_set_game_id(h->game_id);
                void s32_disk_set_dir(const char *rom_filename);
                s32_disk_set_dir(roms[sel].name);
                int r = s32_launch(ROM_BUF, size);  // no return on success
                printf("launch failed: %d\n", r);
            } else printf("read failed\n");
        }
    }
}
