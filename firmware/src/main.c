// sync32 console firmware: M1 = boot -> embedded template ROM.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "sync32.h"
#include "video.h"

extern int s32_launch(const uint8_t *rom, uint32_t size);
extern const uint8_t embedded_rom[];
extern const unsigned embedded_rom_len;

int main(void) {
    // crash safety: watchdog reboot with breadcrumb set = crashed game/firmware.
    // scratch[5]==0 means a clean exit-to-launcher reboot: don't BOOTSEL.
    if (watchdog_caused_reboot() && watchdog_hw->scratch[5] == 0xDEAD5732u)
        reset_usb_boot(0, 0);
    watchdog_hw->scratch[5] = 0xDEAD5732u;   // armed: cleared by clean exit
    watchdog_enable(3000, true);

    video_init();          // also sets sysclk to the board's video clock
    stdio_init_all();      // M1 dev build: PC on native port, no USB host

    void launcher_run(void);
    launcher_run();   // never returns: games exit via chip reset
}

// watchdog feeding while game runs: the game calls present() every frame;
// feed the dog from the vblank path via a repeating timer on core0 IRQs.
// (A hard game hang stops present() but IRQs still run: so tie feeding to
// the SCANOUT progressing AND a liveness flag present() sets.)
#include "pico/time.h"
static volatile uint32_t last_present_frame;
void s32_note_present(void) { last_present_frame = video_frame_count(); }
static bool dog_cb(struct repeating_timer *t) {
    (void)t;
    // feed as long as scanout runs and the game presented in the last ~2s
    if (video_frame_count() - last_present_frame < 150) watchdog_update();
    return true;
}
static struct repeating_timer dog_timer;
__attribute__((constructor)) static void dog_init(void) {
    add_repeating_timer_ms(250, dog_cb, NULL, &dog_timer);
}
