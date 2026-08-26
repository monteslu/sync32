// sync32 console firmware: M1 = boot -> embedded template ROM.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "sync32.h"
#include "video.h"
#include "crash.h"

extern int s32_launch(const uint8_t *rom, uint32_t size);
extern const uint8_t embedded_rom[];
extern const unsigned embedded_rom_len;

int main(void) {
    // watchdog: reboot-to-launcher on hang (no BOOTSEL redirect in dev)
    watchdog_enable(5000, true);

    video_init();
    // dual-role USB: scratch[3] flag = boot straight into pad (host) mode;
    // otherwise device-probe mode (PC serial/MSC), launcher flips if no PC.
    void s32_usb_host_start(void);
    bool host_boot = watchdog_hw->scratch[3] == 0x505AD000u;
    watchdog_hw->scratch[3] = 0;
    if (host_boot) s32_usb_host_start();
    else stdio_init_all();
    crash_handler_init();
    const crash_info_t *ci = crash_handler_get_info();
    if (ci && ci->magic == crash_magic_hard_fault) {
        for (int i = 0; i < 25; i++) {
            printf("CRASH hardfault pc=%08lx lr=%08lx psr=%08lx r0=%08lx r12=%08lx\n",
                   (unsigned long)ci->cy_faultFrame.pc,
                   (unsigned long)ci->cy_faultFrame.lr,
                   (unsigned long)ci->cy_faultFrame.psr,
                   (unsigned long)ci->cy_faultFrame.r0,
                   (unsigned long)ci->cy_faultFrame.r12);
            sleep_ms(200);
        }
    }
    // BOOTLOG: give the lab eyes into every stage

    void launcher_run(void);
    launcher_run();   // never returns: games exit via chip reset
}

// watchdog feeding while game runs: the game calls present() every frame;
// feed the dog from the vblank path via a repeating timer on core0 IRQs.
// (A hard game hang stops present() but IRQs still run: so tie feeding to
// the SCANOUT progressing AND a liveness flag present() sets.)
#include "pico/time.h"
static volatile uint32_t last_present_frame;
volatile bool s32_long_op = false;      // set around mkfs/flash-writes etc
void s32_note_present(void) { last_present_frame = video_frame_count(); }
static bool dog_cb(struct repeating_timer *t) {
    (void)t;
    if (s32_long_op || video_frame_count() - last_present_frame < 150)
        watchdog_update();
    return true;
}
static struct repeating_timer dog_timer;
__attribute__((constructor)) static void dog_init(void) {
    add_repeating_timer_ms(250, dog_cb, NULL, &dog_timer);
}

