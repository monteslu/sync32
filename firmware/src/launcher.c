// sync32 launcher: boot menu. Lists .s32 files from SD, pad/serial navigation,
// loads the selection. Renders with the console's own display-list engine.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "sync32.h"
#include "video.h"
#include "sdcard.h"

extern const uint8_t Font8_Table[];   // 5x8 STM32 BSD font, 8 bytes/glyph
extern volatile s32_pad_t s32_pads[4];
extern int s32_launch(const uint8_t *rom, uint32_t size);
extern const uint8_t embedded_rom[];
extern const unsigned embedded_rom_len;

#define ROM_BUF ((uint8_t *)0x20030000)          // stage into game RAM
#define ROM_BUF_MAX (0x50000 - 0x4000)

static void draw_char(int x, int y, char c, uint16_t color) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = &Font8_Table[(c - 32) * 8];
    for (int r = 0; r < 8; r++)
        for (int b = 0; b < 5; b++)
            if (g[r] & (0x80 >> b))
                video_rect(x + b, y + r, 1, 1, color);
}
static void draw_text(int x, int y, const char *s, uint16_t color) {
    for (; *s; s++, x += 6) draw_char(x, y, *s, color);
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
    rom_entry_t roms[32];
    int n = -1, sel = 0, scroll = 0;
    int sd_state = -99;                            // force first scan
    uint32_t rescan_at = 0;
    while (1) {
        if (video_frame_count() >= rescan_at) {
            rescan_at = video_frame_count() + 120;  // rescan every 2s
            int r = sd_list_roms(roms, 32);
            if (r != sd_state) { sd_state = r; sel = 0; scroll = 0; }
            n = r;
        }
        uint16_t e = pad_edge();
        int cc = getchar_timeout_us(0);            // serial drive for the lab
        if (e & S32_PAD_DOWN || cc == 'j') sel++;
        if (e & S32_PAD_UP || cc == 'k') sel--;
        if (n > 0) { if (sel < 0) sel = n - 1; if (sel >= n) sel = 0; }
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + 18) scroll = sel - 17;

        video_clear(0x10A2);
        video_rect(0, 0, 320, 14, 0x2124);
        draw_text(4, 3, "sync32", 0x07FF);
        draw_text(48, 3, "select a game", 0xFFFF);
        char buf[64];
        if (n < 0) {
            draw_text(60, 110, "insert SD card with .s32 files", 0xFD20);
        } else if (n == 0) {
            draw_text(70, 110, "no .s32 files on card", 0xFD20);
        } else {
            for (int i = scroll; i < n && i < scroll + 18; i++) {
                int y = 20 + (i - scroll) * 12;
                if (i == sel) video_rect(6, y - 2, 308, 11, 0x2965);
                snprintf(buf, sizeof buf, "%-16.16s %6lu", roms[i].title,
                         (unsigned long)roms[i].size);
                draw_text(12, y, buf, i == sel ? 0xFFE0 : 0xC618);
            }
        }
        draw_text(4, 228, "A/enter: play   demo: press X", 0x8410);
        video_present();

        if ((e & S32_PAD_X) || cc == 'd') {        // built-in demo
            s32_launch(embedded_rom, embedded_rom_len);
        }
        if (((e & S32_PAD_A) || cc == '\r' || cc == '\n' || cc == 'p') && n > 0) {
            uint32_t size;
            printf("loading %s...\n", roms[sel].name);
            if (sd_read_rom(roms[sel].name, ROM_BUF, ROM_BUF_MAX, &size) == 0) {
                const s32_header_t *h = (const s32_header_t *)ROM_BUF;
                sd_set_game_id(h->game_id);
                int r = s32_launch(ROM_BUF, size);  // no return on success
                printf("launch failed: %d\n", r);
            } else printf("read failed\n");
        }
    }
}
