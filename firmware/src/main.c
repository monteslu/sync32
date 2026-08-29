// sync32 console firmware: M1 = boot -> embedded template ROM.
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "sync32.h"
#include "video.h"
#include "video_backend.h"
#include "audio_pwm.h"
#include "crash.h"
#include "log.h"

// USB quarantine: scratch[4] carries a rapid-death counter across warm
// reboots (tag 0x5B007A00 | n). If several boots in a row die young, the
// next boot runs with the USB host stack DISABLED so a hostile/broken
// device can never reboot-loop the console. Power-cycling clears it.
bool s32_usb_quarantined;

// sticky pad-mode marker: survives every warm reset (including debugger
// resets), cleared only by a real power cycle. See the host_boot decision.
uint32_t s32_pad_mode_sticky __attribute__((section(".uninitialized_data.s32pad")));

extern int s32_launch(const uint8_t *rom, uint32_t size);
extern const uint8_t embedded_rom[];
extern const unsigned embedded_rom_len;

// panic interceptor: record why + who across the reboot (scratch 5/6),
// plus a flash-range stack scan for a poor-man's backtrace (scratch 7)
void __attribute__((noreturn)) sync32_panic(const char *fmt, ...) {
    // into the persistent ring FIRST: this line survives the reboot
    s32_log("PANIC \"%s\" from %p", fmt ? fmt : "?", __builtin_return_address(0));
    watchdog_hw->scratch[5] = (uint32_t)fmt;
    watchdog_hw->scratch[6] = (uint32_t)__builtin_return_address(0);
    uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
    watchdog_hw->scratch[7] = 0;
    for (int i = 0; i < 48; i++) {
        uint32_t v = sp[i];
        if (v >= 0x10000100 && v < 0x10100000 && (v & 1)) {   // thumb flash ret addr
            watchdog_hw->scratch[7] = v;
            if (watchdog_hw->scratch[7] != watchdog_hw->scratch[6]) break;
        }
    }
    watchdog_reboot(0, 0, 200);
    while (1) __asm volatile("nop");
}

int main(void) {
    // FIRST: disarm the watchdog inherited from the previous boot.
    // watchdog_reboot(0,0,10) leaves a 10ms fuse ARMED into the next boot;
    // crt0 (clocks + zeroing ~170KB bss) takes several ms, so any build
    // with enough bss lost the race and died before main: the entire
    // "layout-dependent dead boot" was this line missing.
    hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);
    // boot forensics: previous boot's stage + panic info -> flash sentinel,
    // auto-BOOTSEL when the previous boot died before going alive
    {
        extern void flash_range_erase(uint32_t, unsigned int);
        extern void flash_range_program(uint32_t, const uint8_t *, unsigned int);
        uint32_t page[64];
        for (int i = 0; i < 64; i++) page[i] = 0xFFFFFFFFu;
        page[0] = watchdog_hw->scratch[2];   // stage
        page[1] = 0xCAFED00Du;               // sentinel-integrity constant
        page[8] = *(const volatile uint32_t *)0x100FE008 + 1;   // boot count
        const volatile uint32_t *cr = (const volatile uint32_t *)0x20000120;
        page[2] = cr[0]; page[3] = cr[10]; page[4] = cr[9];   // crash ram
        page[5] = watchdog_hw->scratch[5];   // panic fmt
        page[6] = watchdog_hw->scratch[6];   // panic caller
        page[7] = watchdog_hw->scratch[7];   // panic caller's caller (scan)
        if (page[8] == 0) page[8] = 1;       // first boot after erase-all
        {   // IRQs off: a timer IRQ mid-program executes flash code
            // while XIP is suspended = fault + torn sentinel
            extern uint32_t save_and_disable_interrupts(void);
            extern void restore_interrupts(uint32_t);
            uint32_t ints = save_and_disable_interrupts();
            flash_range_erase(0xFE000, 4096);
            flash_range_program(0xFE000, (const uint8_t *)page, 256);
            restore_interrupts(ints);
        }
        // scrub scratch [2..7]: picotool reboots write through scratch[2]/[3]
        // and stale boot-vector magic in [4..7] diverts the bootrom.
        // [0] = canary, [1] = xip flag: preserved. [3] survives ONLY with the
        // exact usb-host-mode magic (launcher/api_exit set it before their
        // watchdog_reboot; scrubbing it made pad mode unreachable: the 2.5s
        // device-probe reboot looped forever whenever no PC was attached)
        uint32_t usb_flag = watchdog_hw->scratch[3];
        for (int i = 2; i <= 7; i++) watchdog_hw->scratch[i] = 0;
        if (usb_flag == 0x505AD000u) watchdog_hw->scratch[3] = usb_flag;
        if (watchdog_hw->scratch[0] == 0xDEADB007u) {   // last boot died pre-alive
            watchdog_hw->scratch[0] = 0;
            reset_usb_boot(0, 0);
        }
        watchdog_hw->scratch[0] = 0xDEADB007u;
    }
    #define STAGE(n) (watchdog_hw->scratch[2] = 0x57B0E000u | (n))
    STAGE(1);
    s32_log_init();
    // quarantine decision from reset-surviving RAM: probe hops count as
    // deaths too (they never mark alive), so require a few extra
    s32_usb_quarantined = s32_log_rapid_deaths() >= 6;
    if (s32_usb_quarantined) s32_log("QUARANTINE: usb host disabled (rapid deaths)");
    // watchdog: reboot-to-launcher on hang. (Was disabled hunting "Killer
    // B", which turned out to be the dvi_init heap panic — safe again.)
    watchdog_enable(5000, true);
    STAGE(2);

    video_init();
    // Backends with no in-band audio channel (composite: the yellow RCA is
    // video only) need an analog path. HDMI builds skip this entirely.
    if (!video_backend_has_inband_audio()) s32_audio_pwm_init();
    STAGE(3);
    s32_log("video up");
    // dual-role USB: scratch[3] flag = boot straight into pad (host) mode;
    // otherwise device-probe mode (PC serial/MSC), launcher flips if no PC.
    void s32_usb_host_start(void);
    // Pad mode is STICKY: once the console has entered host mode it stays
    // there across resets, kept in a RAM word that survives reset but not
    // power-off. Without this, any reset that does not go through the
    // launcher (a debugger reset, a watchdog reboot mid-game) landed in
    // device-probe mode and the attached gamepad went dark until someone
    // set the flag by hand.
    extern uint32_t s32_pad_mode_sticky;   // .uninitialized_data, see below
    bool host_boot = watchdog_hw->scratch[3] == 0x505AD000u ||
                     s32_pad_mode_sticky == 0x50414421u;   // "PAD!"
    watchdog_hw->scratch[3] = 0;
    STAGE(4);
    if (s32_usb_quarantined) {
        // host stack off; run as a normal usb DEVICE (serial still works,
        // and the launcher must treat this as pc-mode: no probe-hop reboot)
        s32_log("usb: host OFF (quarantined), device mode");
        stdio_init_all();
    } else if (host_boot) {
        s32_pad_mode_sticky = 0x50414421u;   // remember across resets
        STAGE(0x41); s32_log("usb: host mode"); s32_usb_host_start(); STAGE(0x43);
    } else {
        STAGE(0x42); s32_log("usb: device probe"); stdio_init_all(); STAGE(0x44);
    }
    STAGE(5);
    watchdog_hw->scratch[2]++;               // boot counter (survives reboots)
    crash_handler_init();
    const crash_info_t *ci = crash_handler_get_info();
    if (ci && ci->magic == crash_magic_hard_fault)
        s32_log("CRASH prev boot: pc=%08lx lr=%08lx",
                (unsigned long)ci->cy_faultFrame.pc,
                (unsigned long)ci->cy_faultFrame.lr);
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
void s32_note_present(void) {
    last_present_frame = video_frame_count();
    // any presented frame proves this boot alive: GAMES present too, and the
    // marker being launcher-only meant every exit/reset from a game looked
    // like a dead boot and false-ejected the console to BOOTSEL (black TV)
    watchdog_hw->scratch[0] = 0;
}
static bool dog_cb(struct repeating_timer *t) {
    (void)t;
    // feed only while ALIVE: long op in progress, early boot grace, or
    // scanout advancing with a recent present. A frozen vframe used to
    // satisfy the old check forever (0 - 0 < 150): hangs lived for good.
    static uint32_t last_seen_vframe;
    static bool starve_logged;
    uint32_t vf = video_frame_count();
    bool scanning = vf != last_seen_vframe;
    last_seen_vframe = vf;
    if (s32_long_op || to_ms_since_boot(get_absolute_time()) < 8000 ||
        (scanning && vf - last_present_frame < 150)) {
        watchdog_update();
        starve_logged = false;
    } else if (!starve_logged) {
        // last words before the watchdog fires: WHY feeding stopped
        s32_log("WATCHDOG STARVING scan=%d vf=%lu last_present=%lu",
                scanning, (unsigned long)vf, (unsigned long)last_present_frame);
        starve_logged = true;

#ifdef S32_FREEZE_HOLD
        // DIAGNOSTIC BUILD ONLY: hold the frozen state alive so the SWD
        // probe can autopsy core1 instead of the watchdog destroying it
        s32_log("FREEZE HOLD: waiting for debugger");
        for (;;) watchdog_update();
#endif
    }
    return true;
}
static struct repeating_timer dog_timer;
__attribute__((constructor)) static void dog_init(void) {
    add_repeating_timer_ms(250, dog_cb, NULL, &dog_timer);
}

