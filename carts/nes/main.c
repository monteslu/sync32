// sync32 NES: the agnes core as an .s32 cart. Lists .nes files from the
// cart's data dir (nes/ beside nes.s32) via the api v2 disk calls, picker
// on the pad, streams the chosen ROM into RAM and plays it.
// Mappers 0/1/2/4 (agnes). ROMs up to ROM_CAP load; bigger ones are marked.
#include "sync32.h"
#include "font8.h"

// pull the core in whole so we can reach agnes_t internals (screen_buffer)
#include "agnes.c"

#define ROM_CAP   (128 * 1024)
#define MAX_ROMS  64
#define NES_X     32                   // center 256 wide in 320
static const sync32_api_t *ab;

static uint8_t rombuf[ROM_CAP];
static struct { char name[S32_DISK_NAME_MAX]; uint32_t size; } roms[MAX_ROMS];

// ---- tiny libc for agnes (overrides newlib's sbrk-backed malloc) ----
static uint8_t arena[88 * 1024] __attribute__((aligned(8)));
static uint32_t arena_used;
void *malloc(size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (arena_used + n > sizeof arena) return 0;
    void *p = arena + arena_used;
    arena_used += n;
    return p;
}
void free(void *p) { (void)p; }
void *calloc(size_t a, size_t b) {
    void *p = malloc(a * b);
    if (p) memset(p, 0, a * b);
    return p;
}

// ---- text on the console canvas (Font8, same format as the launcher) ----
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

// palette: 0..63 = NES colors, 64.. = UI colors
enum { UI_BG = 64, UI_TEXT, UI_SEL, UI_DIM, UI_ERR };
static void set_palette(void) {
    uint16_t pal[256];
    for (int i = 0; i < 256; i++) pal[i] = 0;
    for (int i = 0; i < 64; i++) {
        agnes_color_t c = g_colors[i];
        pal[i] = (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    }
    pal[UI_BG] = 0x0000; pal[UI_TEXT] = 0xFFFF; pal[UI_SEL] = 0xFFE0;
    pal[UI_DIM] = 0x8410; pal[UI_ERR] = 0xF800;
    ab->palette_set(pal);
}

static int ends_nes(const char *n) {
    int l = 0;
    while (n[l]) l++;
    return l > 4 && (n[l-4] == '.') &&
           (n[l-3] == 'n' || n[l-3] == 'N') &&
           (n[l-2] == 'e' || n[l-2] == 'E') &&
           (n[l-1] == 's' || n[l-1] == 'S');
}

static int scan_roms(void) {
    int n = 0;
    char name[S32_DISK_NAME_MAX]; uint32_t sz;
    for (int i = 0; n < MAX_ROMS; i++) {
        int r = ab->disk_list(i, name, &sz);
        if (r != S32_DISK_EOK) break;
        if (!ends_nes(name)) continue;
        for (int k = 0; k < S32_DISK_NAME_MAX; k++) roms[n].name[k] = name[k];
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

static void run_nes(int idx) {
    // stream the ROM in 16KB chunks with a loading bar
    int fd = ab->disk_open(roms[idx].name);
    if (fd < 0) return;
    uint32_t size = roms[idx].size, got = 0;
    while (got < size) {
        int r = ab->disk_read(fd, rombuf + got, 16384);
        if (r <= 0) break;
        got += r;
        ab->clear(0x0000);
        ab->rect(60, 116, 200, 8, 0x8410);
        ab->rect(60, 116, (int)(200 * got / size), 8, 0xFFE0);
        ab->present();
    }
    ab->disk_close(fd);
    if (got != size) return;

    arena_used = 0;                       // fresh core per game
    agnes_t *nes = agnes_make();
    if (!nes || !agnes_load_ines_data(nes, rombuf, size)) {
        for (int f = 0; f < 120; f++) {
            ab->clear(0x0000);
            draw_text(100, 116, "unsupported mapper", UI_ERR);
            ab->present();
        }
        return;
    }

    for (;;) {
        s32_pad_t p;
        ab->pad(0, &p);
        if ((p.buttons & S32_PAD_L) && (p.buttons & S32_PAD_R)) return;  // L+R = back
        agnes_input_t in = {
            .a = !!(p.buttons & S32_PAD_A), .b = !!(p.buttons & S32_PAD_B),
            .select = !!(p.buttons & S32_PAD_SELECT), .start = !!(p.buttons & S32_PAD_START),
            .up = !!(p.buttons & S32_PAD_UP), .down = !!(p.buttons & S32_PAD_DOWN),
            .left = !!(p.buttons & S32_PAD_LEFT), .right = !!(p.buttons & S32_PAD_RIGHT),
        };
        agnes_set_input(nes, &in, 0);
        if (!agnes_next_frame(nes)) return;

        uint8_t *fb = ab->canvas();
        const uint8_t *src = nes->ppu.screen_buffer;
        for (int y = 0; y < 240; y++) {
            uint8_t *row = fb + y * 320 + NES_X;
            const uint8_t *srow = src + y * 256;
            for (int x = 0; x < 256; x++) row[x] = srow[x] & 0x3f;
        }
        ab->canvas_mark(0, 240);
        ab->present();
    }
}

void game_main(const sync32_api_t *api) {
    ab = api;
    set_palette();
    if (ab->api_version < 2) {
        for (;;) { ab->clear(0xF800); ab->present(); }
    }
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
            run_nes(sel);
            set_palette();               // game clobbered nothing, but be sure
            pad_edge();                  // eat stale edges
        }

        ab->clear(0x0000);
        uint8_t *fb = ab->canvas();
        for (int i = 0; i < 320 * 240; i++) fb[i] = UI_BG;
        draw_text(8, 4, "sync32 NES", UI_SEL);
        draw_text(212, 4, "A: play  L+R: quit", UI_DIM);
        if (n <= 0) {
            draw_text(60, 112, n < 0 ? "no sd card / data dir" : "no .nes files in nes/", UI_ERR);
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
