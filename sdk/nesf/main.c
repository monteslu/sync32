// sync32 "NES fceumm" cart: romdev's pinned fceumm core, XIP mode.
// Picker lists .nes files from nesf/ via the disk API, streams the pick
// into RAM, hands it to FCEUI_LoadGame, renders XBuf straight onto the
// canvas (both are 8bpp palette indices: zero conversion cost).
#include <string.h>
#include <stdio.h>
#include "sync32.h"
#include "font8.h"
#include "fceu-types.h"
#include "driver.h"
#include "fceu.h"

#define ROM_CAP   (76 * 1024)     // file loads via the canvas buffer
#define MAX_ROMS  64
#define NES_X     32
static const sync32_api_t *ab;

#include <stdlib.h>
static struct { char name[S32_DISK_NAME_MAX]; uint32_t size; } roms[MAX_ROMS];

// ---- core-config globals the libretro driver normally owns ----
unsigned overclock_enabled = 0, overclocked = 0, skip_7bit_overclocking = 1;
unsigned normal_scanlines = 240, totalscanlines = 240;
unsigned extrascanlines = 0, vblankscanlines = 0;
unsigned dendy = 0, swapDuty = 0;

// ---- FCEUD driver callbacks ----
static uint16_t pal565[256];
static int pal_dirty;

void FCEUD_SetPalette(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    pal565[index] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    pal_dirty = 1;
}
void FCEUD_GetPalette(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint16_t c = pal565[index];
    *r = (c >> 11) << 3; *g = ((c >> 5) & 63) << 2; *b = (c & 31) << 3;
}
const uint8_t *GetKeyboard(void) {      // family keyboard: not present
    static const uint8_t none[256];
    return none;
}
static char last_err[48];
static int err_frames;
void FCEUD_PrintError(const char *s) {
    int i = 0;
    for (; s[i] && i < 47; i++) last_err[i] = s[i];
    last_err[i] = 0;
    err_frames = 240;                    // show for 4 seconds
}
void FCEUD_Message(const char *s) { (void)s; }
void FCEUD_DispMessage(enum retro_log_level level, unsigned duration, const char *str) {
    (void)level; (void)duration; (void)str;
}

// ---- UI text (same Font8 as the launcher) ----
enum { UI_TEXT = 250, UI_SEL, UI_DIM, UI_ERR };   // park UI colors high
static void draw_text(int x, int y, const char *s, uint8_t ci) {
    uint8_t *fb = ab->canvas();
    for (; *s; s++, x += 6) {
        char c = (*s < 32 || *s > 126) ? '?' : *s;
        const uint8_t *g = &Font8_Table[(c - 32) * 8];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] & (0x80 >> b) && x + b >= 0 && x + b < 320 && y + r < 240)
                    fb[(y + r) * 320 + x + b] = ci;
    }
}
static void ui_palette(void) {
    uint16_t pal[256];
    memcpy(pal, pal565, sizeof pal);
    pal[UI_TEXT] = 0xFFFF; pal[UI_SEL] = 0xFFE0;
    pal[UI_DIM] = 0x8410; pal[UI_ERR] = 0xF800;
    ab->palette_set(pal);
}

static int ends_nes(const char *n) {
    int l = 0;
    while (n[l]) l++;
    return l > 4 && n[l-4] == '.' &&
           (n[l-3] | 32) == 'n' && (n[l-2] | 32) == 'e' && (n[l-1] | 32) == 's';
}

static int scan_roms(void) {
    int n = 0;
    char name[S32_DISK_NAME_MAX]; uint32_t sz;
    for (int i = 0; n < MAX_ROMS; i++) {
        if (ab->disk_list(i, name, &sz) != S32_DISK_EOK) break;
        if (!ends_nes(name)) continue;
        memcpy(roms[n].name, name, S32_DISK_NAME_MAX);
        roms[n].size = sz;
        n++;
    }
    return n;
}

static uint16_t pad_edge(void) {
    static uint16_t prev;
    s32_pad_t p;
    ab->pad(0, &p);
    uint16_t e = p.buttons & ~prev;
    prev = p.buttons;
    return e;
}

static uint32_t js;                     // FCEU joypad state (per-byte per player)

static void run_game(int idx) {
    int fd = ab->disk_open(roms[idx].name);
    if (fd < 0) return;
    uint32_t size = roms[idx].size, got = 0;
    // the 76.8KB canvas doubles as the file buffer during load: fceumm
    // copies what it keeps, and the first emulated frame repaints anyway
    uint8_t *rombuf = ab->canvas();
    while (got < size) {
        int r = ab->disk_read(fd, rombuf + got, 16384);
        if (r <= 0) break;
        got += r;
        ab->rect(60, 116, 200, 8, 0x8410);
        ab->rect(60, 116, (int)(200 * got / size), 8, 0xFFE0);
        ab->present();
    }
    ab->disk_close(fd);
    if (got != size) return;

    FCEUGI *gi = FCEUI_LoadGame(roms[idx].name, rombuf, size, NULL);
    if (!gi) {
        for (int f = 0; f < 120; f++) {
            ab->clear(0x0000);
            draw_text(96, 116, "fceumm: load failed", UI_ERR);
            ab->present();
        }
        return;
    }
    js = 0;
    FCEUI_SetInput(0, SI_GAMEPAD, &js, 0);
    FCEUI_PowerNES();

    for (;;) {
        s32_pad_t p;
        ab->pad(0, &p);
        if ((p.buttons & S32_PAD_L) && (p.buttons & S32_PAD_R)) break;
        uint32_t b = 0;                  // FCEU: A B SEL START U D L R
        if (p.buttons & S32_PAD_A) b |= 0x01;
        if (p.buttons & S32_PAD_B) b |= 0x02;
        if (p.buttons & S32_PAD_SELECT) b |= 0x04;
        if (p.buttons & S32_PAD_START) b |= 0x08;
        if (p.buttons & S32_PAD_UP) b |= 0x10;
        if (p.buttons & S32_PAD_DOWN) b |= 0x20;
        if (p.buttons & S32_PAD_LEFT) b |= 0x40;
        if (p.buttons & S32_PAD_RIGHT) b |= 0x80;
        js = b;

        uint8_t *gfx; int32_t *snd; int32_t ssize;
        FCEUI_Emulate(&gfx, &snd, &ssize, 0);

        if (pal_dirty) { pal_dirty = 0; ui_palette(); }
        if (err_frames > 0) err_frames--;
        uint8_t *fb = ab->canvas();
        for (int y = 0; y < 240; y++)
            memcpy(fb + y * 320 + NES_X, gfx + y * 256, 256);
        if (err_frames > 0) draw_text(8, 230, last_err, UI_ERR);
        ab->canvas_mark(0, 240);
        ab->present();

        if (ssize > 0) {                 // core gives int32 mono: ABI wants int16 stereo
            static int16_t lr[2048];
            int n = ssize > 1024 ? 1024 : ssize;
            for (int i = 0; i < n; i++) {
                int16_t v = (int16_t)snd[i];
                lr[i * 2] = v; lr[i * 2 + 1] = v;
            }
            if (ab->audio_space() >= n) ab->audio_push(lr, n);
        }
    }
    FCEUI_CloseGame();
}

void game_main(const sync32_api_t *api) {
    ab = api;
    ui_palette();
    if (ab->api_version < 2) {
        for (;;) { ab->clear(0xF800); ab->present(); }
    }
    if (!FCEUI_Initialize()) {
        for (;;) { ab->clear(0xF800); ab->present(); }
    }
    FCEUI_Sound(48000);                 // ABI rate; rate 0 wedges sound-table init

    int n = scan_roms();
    int sel = 0, scroll = 0;
    for (;;) {
        uint16_t e = pad_edge();
        if (e & S32_PAD_DOWN) sel++;
        if (e & S32_PAD_UP) sel--;
        if (n > 0) { if (sel < 0) sel = n - 1; if (sel >= n) sel = 0; }
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + 24) scroll = sel - 23;
        if ((e & S32_PAD_A) && n > 0 && roms[sel].size <= ROM_CAP) {
            run_game(sel);
            ui_palette();
            pad_edge();
        }
        ab->clear(0x0000);
        draw_text(8, 4, "sync32 NES (fceumm)", UI_SEL);
        draw_text(218, 4, "A: play L+R: quit", UI_DIM);
        if (n <= 0) {
            draw_text(70, 112, "no .nes files in nesf/", UI_ERR);
        } else {
            for (int i = scroll; i < n && i < scroll + 24; i++) {
                int y = 16 + (i - scroll) * 9;
                int fits = roms[i].size <= ROM_CAP;
                draw_text(16, y, roms[i].name, i == sel ? UI_SEL : (fits ? UI_TEXT : UI_DIM));
                if (i == sel) draw_text(8, y, ">", UI_SEL);
                if (!fits) draw_text(240, y, "too big", UI_DIM);
            }
        }
        ab->canvas_mark(0, 240);
        ab->present();
    }
}
