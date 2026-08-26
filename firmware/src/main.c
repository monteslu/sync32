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
    // BOOTBUG stage harness: previous boot's last stage -> flash sentinel,
    // auto-BOOTSEL when the previous boot died before going alive
    {
        extern void flash_range_erase(uint32_t, unsigned int);
        extern void flash_range_program(uint32_t, const uint8_t *, unsigned int);
        uint32_t page[64];
        for (int i = 0; i < 64; i++) page[i] = 0xFFFFFFFFu;
        page[0] = watchdog_hw->scratch[2];
        flash_range_erase(0xFE000, 4096);
        flash_range_program(0xFE000, (const uint8_t *)page, 256);
        if (watchdog_hw->scratch[0] == 0xDEADB007u) {
            watchdog_hw->scratch[0] = 0;
            reset_usb_boot(0, 0);
        }
        watchdog_hw->scratch[0] = 0xDEADB007u;
    }
    #define STAGE(n) (watchdog_hw->scratch[2] = 0x57A6E000u | (n))
    STAGE(1);
    // watchdog: reboot-to-launcher on hang (no BOOTSEL redirect in dev)
    watchdog_enable(5000, true);
    STAGE(2);

    video_init();
    STAGE(3);
    // dual-role USB: scratch[3] flag = boot straight into pad (host) mode;
    // otherwise device-probe mode (PC serial/MSC), launcher flips if no PC.
    void s32_usb_host_start(void);
    bool host_boot = watchdog_hw->scratch[3] == 0x505AD000u;
    watchdog_hw->scratch[3] = 0;
    STAGE(4);
    if (host_boot) s32_usb_host_start();
    else stdio_init_all();
    STAGE(5);
    watchdog_hw->scratch[2]++;               // boot counter (survives reboots)
    crash_handler_init();
    const crash_info_t *ci = crash_handler_get_info();
    if (ci && ci->magic == crash_magic_hard_fault) {
        watchdog_hw->scratch[6] = ci->magic;  // breadcrumb for the on-screen diag
        watchdog_hw->scratch[7] = ci->cy_faultFrame.pc;
        for (int i = 0; i < 8; i++) {
            watchdog_update();
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
    // feed only while ALIVE: long op in progress, early boot grace, or
    // scanout advancing with a recent present. A frozen vframe used to
    // satisfy the old check forever (0 - 0 < 150): hangs lived for good.
    static uint32_t last_seen_vframe;
    uint32_t vf = video_frame_count();
    bool scanning = vf != last_seen_vframe;
    last_seen_vframe = vf;
    if (s32_long_op || to_ms_since_boot(get_absolute_time()) < 8000 ||
        (scanning && vf - last_present_frame < 150))
        watchdog_update();
    return true;
}
static struct repeating_timer dog_timer;
__attribute__((constructor)) static void dog_init(void) {
    add_repeating_timer_ms(250, dog_cb, NULL, &dog_timer);
}

