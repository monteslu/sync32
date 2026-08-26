// Tone test: pushes a 440Hz square through the audio API. Proves the
// audio path (space/push/48kHz timing) without an emulator core.
#include "sync32.h"

void game_main(const sync32_api_t *api) {
    uint16_t pal[256] = {0};
    pal[1] = 0x07E0;
    api->palette_set(pal);
    int16_t lr[1600];
    uint32_t phase = 0;
    for (;;) {
        int want = 800;                       // one frame of 48kHz audio
        if (api->audio_space() >= want) {
            for (int i = 0; i < want; i++) {
                int16_t v = (phase++ / 54) & 1 ? 8000 : -8000;   // ~444Hz
                lr[i * 2] = v; lr[i * 2 + 1] = v;
            }
            api->audio_push(lr, want);
        }
        api->clear(0x0000);
        api->rect(40, 100, (int)(phase / 480) % 240, 20, 0x07E0);
        api->present();
    }
}
