// sync32 SDK template game: bouncing sprites + player square + palette art.
#include "sync32.h"

// tiny built-in sheet: 32x16, two 16x16 cells (smiley, star), index 0 clear
static uint8_t sheet[32 * 16];
static void build_sheet(void) {
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            int dx = x - 8, dy = y - 8;
            int d2 = dx * dx + dy * dy;
            // cell 0: smiley
            uint8_t c = 0;
            if (d2 <= 49) c = 1;                       // face
            if (d2 <= 49 && d2 >= 36) c = 2;           // outline
            if ((x==5||x==10) && y==6) c = 2;          // eyes
            if (y==10 && x>=5 && x<=10 && d2<=42) c=2; // mouth
            sheet[y * 32 + x] = c;
            // cell 1: star-ish diamond
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            uint8_t s = (ax + ay <= 7) ? 3 : 0;
            if (ax + ay <= 3) s = 4;
            sheet[y * 32 + 16 + x] = s;
        }
}

void game_main(const sync32_api_t *api) {
    build_sheet();
    static uint16_t pal[256];
    pal[1] = 0xFFE0; pal[2] = 0x0000;      // yellow face, black lines
    pal[3] = 0x07FF; pal[4] = 0xFFFF;      // cyan star, white core
    api->palette_set(pal);
    int sh = api->sheet_load(sheet, 32, 16);

    struct { int x, y, vx, vy, cell; } s[6];
    api->rng_seed(0x5EED);
    for (int i = 0; i < 6; i++) {
        s[i].x = 20 + (api->rng_next() % 260);
        s[i].y = 20 + (api->rng_next() % 180);
        s[i].vx = (i & 1) ? 2 : -1; s[i].vy = (i & 2) ? 1 : -2;
        s[i].cell = i & 1;
    }
    int px = 152, py = 112;
    uint32_t frame = 0;
    for (;;) {
        s32_pad_t pad;
        api->pad(0, &pad);
        if (pad.buttons & S32_PAD_LEFT)  px -= 3;
        if (pad.buttons & S32_PAD_RIGHT) px += 3;
        if (pad.buttons & S32_PAD_UP)    py -= 3;
        if (pad.buttons & S32_PAD_DOWN)  py += 3;
        if (px < 0) px = 0; if (px > 304) px = 304;
        if (py < 0) py = 0; if (py > 224) py = 224;
        for (int i = 0; i < 6; i++) {
            s[i].x += s[i].vx; s[i].y += s[i].vy;
            if (s[i].x < 0 || s[i].x > 304) s[i].vx = -s[i].vx;
            if (s[i].y < 0 || s[i].y > 224) s[i].vy = -s[i].vy;
        }
        api->clear(0x0841);
        // frame counter bar: proves liveness in captures
        api->rect(0, 0, (frame >> 2) % 320, 3, 0x07E0);
        for (int i = 0; i < 6; i++)
            api->sprite(sh, s[i].cell * 16, 0, 16, 16, s[i].x, s[i].y, 0);
        api->rect(px, py, 16, 16, 0xF800);            // player
        api->sprite(sh, 0, 0, 16, 16, px, py, 0);     // wears the smiley
        api->present();
        frame++;
        if (pad.buttons & S32_PAD_START && pad.buttons & S32_PAD_SELECT)
            api->exit();
    }
}
