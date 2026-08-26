// "Formation" : the sync32 launch demo. Kenney Pixel Shmup art (CC0).
#include "sync32.h"
#include "sheet.h"

#define NSPR 8
static struct { float x, y, vx, vy; uint8_t sx, sy, w, h; } s[NSPR];

void game_main(const sync32_api_t *api) {
    api->palette_set(sheet_palette);
    int sh = api->sheet_load(sheet_pixels, SHEET_W, SHEET_H);
    static const uint8_t cast[NSPR][4] = {
        {0, 0, 32, 32},  {32, 0, 32, 32}, {64, 0, 32, 32}, {96, 0, 32, 32},
        {128, 0, 32, 32}, {0, 32, 16, 16}, {16, 32, 16, 16}, {80, 32, 16, 16},
    };
    api->rng_seed(0xF0421);
    for (int i = 0; i < NSPR; i++) {
        s[i].x = 20 + (api->rng_next() % 240);
        s[i].y = 20 + (api->rng_next() % 180);
        s[i].vx = 0.7f + 0.35f * i; s[i].vy = 1.9f - 0.3f * i;
        s[i].sx = cast[i][0]; s[i].sy = cast[i][1];
        s[i].w = cast[i][2]; s[i].h = cast[i][3];
    }
    uint32_t frame = 0;
    static const uint8_t pships[4] = {0, 32, 64, 96};
    int pc = 0; uint16_t prev = 0;
    for (;;) {
        s32_pad_t pad; api->pad(0, &pad);
        float vx = 0, vy = 0;
        if (pad.buttons & S32_PAD_LEFT) vx = -3;
        if (pad.buttons & S32_PAD_RIGHT) vx = 3;
        if (pad.buttons & S32_PAD_UP) vy = -3;
        if (pad.buttons & S32_PAD_DOWN) vy = 3;
        if ((pad.buttons & S32_PAD_A) && !(prev & S32_PAD_A)) pc = (pc + 1) & 3;
        prev = pad.buttons;
        s[0].vx = vx; s[0].vy = vy; s[0].sx = pships[pc];
        for (int i = 0; i < NSPR; i++) {
            s[i].x += s[i].vx; s[i].y += s[i].vy;
            int pl = (i == 0);
            if (s[i].x < 0) { s[i].x = 0; if (!pl) s[i].vx = -s[i].vx; }
            if (s[i].y < 0) { s[i].y = 0; if (!pl) s[i].vy = -s[i].vy; }
            if (s[i].x + s[i].w > 320) { s[i].x = 320 - s[i].w; if (!pl) s[i].vx = -s[i].vx; }
            if (s[i].y + s[i].h > 240) { s[i].y = 240 - s[i].h; if (!pl) s[i].vy = -s[i].vy; }
        }
        s[5].sx = (frame / 12) & 1 ? 16 : 0;
        s[6].sx = (frame / 12) & 1 ? 0 : 16;
        api->clear(0x1082);
        for (int gx = 0; gx < 320; gx += 32) api->rect(gx, 0, 1, 240, 0x18E3);
        for (int gy = 0; gy < 240; gy += 32) api->rect(0, gy, 320, 1, 0x18E3);
        for (int i = 1; i < NSPR; i++)
            api->sprite(sh, s[i].sx, s[i].sy, s[i].w, s[i].h, (int)s[i].x, (int)s[i].y, 0);
        api->sprite(sh, s[0].sx, s[0].sy, 32, 32, (int)s[0].x, (int)s[0].y, 0);
        api->present();
        frame++;
        if ((pad.buttons & S32_PAD_START) && (pad.buttons & S32_PAD_SELECT))
            api->exit();
    }
}
