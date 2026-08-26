// sync32 video: 8bpp framebuffer (doubles as the game canvas), display-list
// rendering at present(), core1 = fullres TMDS palette encode scanout.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include "video.h"

#define DVI_TIMING dvi_timing_640x480p_60hz

uint8_t s32_framebuf[320 * 240];
static uint16_t cur_palette[256];
static uint32_t tmds_palette[6 * 256];
static volatile int palette_dirty;

// sheets: one 64KB arena
static uint8_t sheet_arena[64 * 1024];
static struct { int off, w, h; } sheets[8];
static int sheet_count;
static int arena_used;

// display list
typedef struct { int16_t x, y; uint8_t type, sheet, sx, sy, w, h, flags; uint16_t color; } op_t;
static op_t list[S32_MAX_SPRITES + 64];   // sprites + rects share, rects extra
static int list_n;
static uint16_t clear_color;
static int clear_pending;

static struct dvi_inst dvi0;
static volatile uint32_t vframe;

static void __not_in_flash_func(core1_scanout)(void) {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    uint y = 0;
    while (1) {
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        const uint32_t *row = (const uint32_t *)&s32_framebuf[y * 320];
        tmds_encode_palette_data(row, tmds_palette, tmdsbuf, 320, 8);
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
        if (++y == 240) {
            y = 0;
            vframe++;
            if (palette_dirty) {
                palette_dirty = 0;
                tmds_setup_palette_symbols(cur_palette, tmds_palette, 256);
            }
        }
    }
}

void video_init(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);
    memset(s32_framebuf, 0, sizeof s32_framebuf);
    tmds_setup_palette_symbols(cur_palette, tmds_palette, 256);
    pio_set_gpio_base(DVI_DEFAULT_SERIAL_CONFIG.pio, 16);
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    multicore_launch_core1(core1_scanout);
}

void video_palette(const uint16_t *p) {
    memcpy(cur_palette, p, 512);
    palette_dirty = 1;
}

int video_sheet_load(const void *px, int w, int h) {
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return -1;
    int bytes = w * h;
    if (arena_used + bytes > (int)sizeof sheet_arena) return -2;
    if (sheet_count >= 8) return -3;
    memcpy(sheet_arena + arena_used, px, bytes);
    sheets[sheet_count].off = arena_used;
    sheets[sheet_count].w = w;
    sheets[sheet_count].h = h;
    arena_used += bytes;
    return sheet_count++;
}

void video_sheet_reset(void) { sheet_count = 0; arena_used = 0; }

void video_clear(uint16_t c) { clear_color = c; clear_pending = 1; }

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
    if (clear_pending) {
        clear_pending = 0;
        memset(s32_framebuf, color_to_idx(clear_color), sizeof s32_framebuf);
    }
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
        const uint8_t *sp = sheet_arena + sheets[o->sheet].off;
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
void video_present(void) {
    render_list();
    uint32_t f = vframe;
    while (vframe == f) tight_loop_contents();
    s32_note_present();
}

uint32_t video_frame_count(void) { return vframe; }
