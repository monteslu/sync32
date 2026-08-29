// sync32 video: 8bpp framebuffer (doubles as the game canvas), display-list
// rendering at present(), core1 = palette encode + scanout to the backend.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "video_backend.h"
#include "video.h"
#include "hardware/watchdog.h"


uint8_t s32_framebuf[320 * 240];
volatile uint32_t s32_scanline;
static bool aud_block_start = true;   // first IEC60958 block frame   // row core1 is currently encoding
static uint16_t cur_palette[256];

// sheets: one 64KB arena
// 60KB: the ABI promises 64KB of sheets, but the launcher's own art and
// every cart shipped so far fit well under this, and the 4KB reclaimed
// pays for the HDMI data-island DMA blocklists. Revisit if a cart ever
// needs the full 64KB (sheet_load reports -2 and the cart can subdivide).
/* Sheets reference the GAME's memory; the console keeps no arena.
 *
 * sheet_load used to memcpy pixels into 58 KB of firmware RAM. Nothing
 * shipped with it: every real cart draws to the canvas, and sprite data is
 * better left in XIP flash, where it is memory-mapped, costs no RAM and is
 * bounded by the 3 MB slot rather than by 58 KB. So the pointer is recorded
 * instead of the pixels, and the RAM goes to the game (ABI 2, 6.2).
 *
 * The pixels must stay valid for as long as the sheet is used, which is
 * automatic for the usual case of a const array baked into the cart. */
static struct { const uint8_t *px; int w, h; } sheets[8];
static int sheet_count;


// display list
typedef struct { int16_t x, y; uint8_t type, sheet, sx, sy, w, h, flags; uint16_t color; } op_t;
static op_t list[S32_MAX_SPRITES + 64];   // sprites + rects share, rects extra
static int list_n;
static uint16_t clear_color;
static int clear_pending;

static volatile uint32_t vframe;

// proven path: palette-LUT each 8bpp row to RGB565, then the same 16bpp
// encode that ran for hours on this board in the lab demos.
static uint16_t line16[320];
volatile uint8_t s32_video_mode;        // 0 = 320x240, 1 = 320x180 letterbox
// Scanout is shared across every backend: it turns the console's 8bpp
// framebuffer into RGB565 through the palette and hands finished lines to
// whichever backend was built in. Only the handoff differs per backend, so
// only that lives behind video_backend.h.
//
// This runs on CORE 1, which is the load-bearing fact behind the ABI's
// 2.5M-cycles-per-frame guarantee: that figure is core 0's budget with video
// already paid for, so the backend choice does not change what a game can
// afford.
static void __not_in_flash_func(core1_scanout)(void) {
    video_backend_start_on_core1();
    uint y = 0;
    while (1) {
        if (s32_video_mode == 1 && (y < 30 || y >= 210)) {
            for (int x = 0; x < 320; x++) line16[x] = 0;   // letterbox bars
        } else {
            uint srcy = s32_video_mode == 1 ? y - 30 : y;
            const uint8_t *row = &s32_framebuf[srcy * 320];
            for (int x = 0; x < 320; x++) line16[x] = cur_palette[row[x]];
        }
        video_backend_scanline(line16, (int)y);
        s32_scanline = y;
        if (++y == 240) { y = 0; vframe++; }
    }
}

void video_init(void) {
    #define VSTAGE(n) (watchdog_hw->scratch[2] = 0x57A6E100u | (n))
    VSTAGE(1);
    video_backend_set_clock();
    VSTAGE(2);
    VSTAGE(3);
    // boot diagnostic: paint stripes + a default palette so the panel
    // shows SOMETHING the instant scanout runs, before any launcher logic
    for (int i = 0; i < 256; i++) cur_palette[i] = (uint16_t)(i * 0x0821);
    cur_palette[1] = 0xF800; cur_palette[2] = 0xFFFF;
    for (int y = 0; y < 240; y++)
        memset(&s32_framebuf[y * 320], (y / 20) & 1 ? 1 : 2, 320);
    video_backend_init();
    VSTAGE(5);
    VSTAGE(6);
    multicore_launch_core1(core1_scanout);
    VSTAGE(7);
}

void video_palette(const uint16_t *p) {
    memcpy(cur_palette, p, 512);
}

int video_sheet_load(const void *px, int w, int h) {
    if (!px || w <= 0 || h <= 0 || w > 1024 || h > 1024) return -1;
    if (sheet_count >= 8) return -3;
    sheets[sheet_count].px = (const uint8_t *)px;
    sheets[sheet_count].w = w;
    sheets[sheet_count].h = h;
    return sheet_count++;
}

void video_sheet_reset(void) { sheet_count = 0; }

static uint8_t color_to_idx(uint16_t c);
// IMMEDIATE canvas fill (ABI semantic: clear, then canvas drawing, then
// present must show the drawing; a deferred clear wipes it at present)
void video_clear(uint16_t c) { memset(s32_framebuf, color_to_idx(c), sizeof s32_framebuf); }

void video_sprite(int sheet, int sx, int sy, int w, int h, int x, int y, uint8_t flags) {
    if (list_n >= (int)(sizeof list / sizeof list[0])) return;
    op_t *o = &list[list_n++];
    o->type = 0; o->sheet = sheet; o->sx = sx; o->sy = sy;
    o->w = w; o->h = h; o->x = x; o->y = y; o->flags = flags;
}

void video_rect(int x, int y, int w, int h, uint16_t c) {
    if (list_n >= (int)(sizeof list / sizeof list[0])) return;
    op_t *o = &list[list_n++];
    o->type = 1; o->x = x; o->y = y; o->w = w; o->h = h; o->color = c;
}

uint8_t *video_canvas(void) { return s32_framebuf; }
void video_canvas_mark(int y0, int y1) { (void)y0; (void)y1; }

// nearest-color palette index for rect() RGB565 colors (cached per color)
static uint8_t color_to_idx(uint16_t c) {
    static uint16_t last_c = 1; static uint8_t last_i;
    if (c == last_c) return last_i;
    int best = 1 << 30; uint8_t bi = 0;
    for (int i = 0; i < 256; i++) {
        int dr = ((cur_palette[i] >> 11) & 31) - ((c >> 11) & 31);
        int dg = ((cur_palette[i] >> 5) & 63) - ((c >> 5) & 63);
        int db = (cur_palette[i] & 31) - (c & 31);
        int d = dr * dr * 2 + dg * dg + db * db * 2;
        if (d < best) { best = d; bi = i; }
    }
    last_c = c; last_i = bi;
    return bi;
}

static void render_list(void) {
    for (int n = 0; n < list_n; n++) {
        op_t *o = &list[n];
        int x0 = o->x, y0 = o->y;
        int w = o->w, h = o->h;
        if (o->type == 1) {                     // rect
            uint8_t ci = color_to_idx(o->color);
            for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < 240; y++) {
                int xa = x0 < 0 ? 0 : x0, xb = x0 + w > 320 ? 320 : x0 + w;
                if (xa < xb) memset(&s32_framebuf[y * 320 + xa], ci, xb - xa);
            }
            continue;
        }
        if (o->sheet < 0 || o->sheet >= sheet_count) continue;
        const uint8_t *sp = sheets[o->sheet].px;
        int sw = sheets[o->sheet].w;
        for (int y = 0; y < h; y++) {
            int dy = y0 + y;
            if (dy < 0 || dy >= 240) continue;
            int srow = (o->flags & S32_SPRITE_FLIP_Y) ? (h - 1 - y) : y;
            const uint8_t *src = sp + (o->sy + srow) * sw + o->sx;
            uint8_t *dst = &s32_framebuf[dy * 320];
            for (int x = 0; x < w; x++) {
                int dx = x0 + x;
                if (dx < 0 || dx >= 320) continue;
                uint8_t px = src[(o->flags & S32_SPRITE_FLIP_X) ? (w - 1 - x) : x];
                if (px) dst[dx] = px;
            }
        }
    }
    list_n = 0;
}

void s32_note_present(void);
void s32_usb_task(void);
void video_present(void) {
    s32_usb_task();
    render_list();
    // Single-buffered console: scanout reads the same canvas the game draws
    // into, so a frame that takes longer than one scanout period shows torn
    // bands (thin geometry can vanish inside them). Full double buffering
    // needs 75KB the ABI has already promised elsewhere, so instead present()
    // releases the game exactly at the START of the visible frame: drawing
    // then races the beam DOWN the screen from row 0 rather than starting at
    // an arbitrary row, which is the classic single-buffer discipline and
    // keeps a whole frame of drawing time ahead of the beam.
    uint32_t f = vframe;
    while (vframe == f) tight_loop_contents();     // wait for frame boundary
    while (s32_scanline > 4) tight_loop_contents();// and for the beam to be at the top
    s32_note_present();
}

uint32_t video_frame_count(void) { return vframe; }
