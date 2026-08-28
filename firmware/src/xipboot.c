// sync32 XIP flash-slot boot: streams a big .s32 from SD into the flash
// game slot (0x10100000 / flash offset 0x100000), verifies it in place,
// stamps a meta sector, and launches. Carts far larger than RAM run XIP.
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "ff.h"
#include "sync32.h"
#include "sdcard.h"
#include "s32crc.h"

#define SLOT_FLASH_OFF   0x100000u              // 1MB in: firmware lives below
#define SLOT_XIP_ADDR    0x10100000u
#define SLOT_MAX         (12u * 1024 * 1024)
#define META_FLASH_OFF   0x0FF000u              // last 4KB below the slot
#define META_XIP_ADDR    (0x10000000u + META_FLASH_OFF)
#define META_MAGIC       0x58495053u            // "SPIX"
#define BOOT_SLOT_FLAG   0x58495042u            // scratch[1]: boot staged slot

extern volatile uint8_t s32_video_mode;
typedef struct {
    uint32_t magic, crc32, code_size, entry_offset;
    char filename[40];                  // for the game's disk dir + saves
    uint8_t game_id[8];
    uint8_t video_mode;
    uint8_t pad_[3];
} xip_meta_t;

extern volatile bool s32_long_op;
void s32_enter_game(uint32_t entry);            // api.c
int sd_mount(void);

static const volatile xip_meta_t *meta = (const xip_meta_t *)META_XIP_ADDR;

static void flash_op(bool erase, uint32_t off, const uint8_t *data, uint32_t len) {
    uint32_t ints = save_and_disable_interrupts();
    if (erase) flash_range_erase(off, len);
    else flash_range_program(off, data, len);
    restore_interrupts(ints);
    watchdog_update();
}

// boot path: launch the staged slot if the reboot flag is set
void s32_xip_boot_check(void) {
    if (watchdog_hw->scratch[1] != BOOT_SLOT_FLAG) return;
    watchdog_hw->scratch[1] = 0;
    if (meta->magic != META_MAGIC) return;
    if (s32_crc32((const void *)SLOT_XIP_ADDR, meta->code_size) != meta->crc32) return;
    void s32_disk_set_dir(const char *rom_filename);
    s32_disk_set_dir((const char *)meta->filename);
    sd_set_game_id((const uint8_t *)meta->game_id);
    s32_video_mode = meta->video_mode == 1 ? 1 : 0;
    printf("xip: booting staged slot entry=%08lx\n",
           (unsigned long)(SLOT_XIP_ADDR + meta->entry_offset));
    s32_enter_game(SLOT_XIP_ADDR + meta->entry_offset);
}

// returns only on error
int s32_xip_stage_and_launch(const char *filename) {
    if (sd_mount() != 0) return -1;
    static FIL f;
    if (f_open(&f, filename, FA_READ) != FR_OK) return -2;

    uint8_t hdr[64]; UINT br;
    if (f_read(&f, hdr, 64, &br) != FR_OK || br != 64) { f_close(&f); return -3; }
    const s32_header_t *h = (const s32_header_t *)hdr;
    if (h->magic != S32_MAGIC || h->load_mode != S32_LOAD_XIP) { f_close(&f); return -4; }
    s32_video_mode = h->video_mode == 1 ? 1 : 0;
    // XIP contract: the code image starts at 64; trailing sections after it
    // (e.g. the optional 16x16 launcher icon) are allowed and ignored here
    if (h->code_offset != 64 || h->rom_size < 64 + h->code_size ||
        h->code_size > SLOT_MAX) { f_close(&f); return -5; }

    if (meta->magic == META_MAGIC && meta->crc32 == h->crc32 &&
        meta->code_size == h->code_size) {
        // already staged: instant launch, no reboot needed
        f_close(&f);
        printf("xip: slot hit, launching\n");
        s32_enter_game(SLOT_XIP_ADDR + h->entry_offset);
    }

    printf("xip: staging %lu bytes to flash...\n", (unsigned long)h->code_size);
    uint32_t entry_offset = h->entry_offset, code_size = h->code_size;
    uint32_t crc_want = h->crc32;

    // core1 scanout executes from flash: it MUST be dead during flash ops
    // (a core1 fault mid-program would watchdog-reboot into a corrupt slot)
    s32_long_op = true;
    multicore_reset_core1();

    // stage through game RAM: no game is loaded and core1 is about to die,
    // so the region is free; keeps 4KB out of firmware bss (heap pressure)
    uint8_t *chunk = (uint8_t *)0x20060000;
    uint32_t done = 0;
    int rc = 0;
    while (done < code_size) {
        UINT want = code_size - done > 4096 ? 4096 : code_size - done;
        if (f_read(&f, chunk, want, &br) != FR_OK || br == 0) { rc = -6; break; }
        if (br < 4096) memset(chunk + br, 0xff, 4096 - br);
        if ((done & 0xFFFF) == 0) {
            uint32_t left = code_size - done;
            uint32_t esz = left > 0x10000 ? 0x10000 : ((left + 0xFFFu) & ~0xFFFu);
            flash_op(true, SLOT_FLASH_OFF + done, NULL, esz);
        }
        flash_op(false, SLOT_FLASH_OFF + done, chunk, 4096);
        done += br;
    }
    f_close(&f);

    if (rc == 0 && s32_crc32((const void *)SLOT_XIP_ADDR, code_size) != crc_want)
        rc = -7;

    if (rc == 0) {
        xip_meta_t m = { META_MAGIC, crc_want, code_size, entry_offset, {0}, {0}, 0, {0} };
        strncpy(m.filename, filename, sizeof m.filename - 1);
        memcpy(m.game_id, hdr + 48, 8);
        m.video_mode = hdr[29];
        uint8_t page[256];
        memset(page, 0xff, sizeof page);
        memcpy(page, &m, sizeof m);
        flash_op(true, META_FLASH_OFF, NULL, 4096);
        flash_op(false, META_FLASH_OFF, page, 256);
        // reboot into the slot: cleanest way back to full video + usb
        watchdog_hw->scratch[1] = BOOT_SLOT_FLAG;
        watchdog_reboot(0, 0, 200);
        while (1) tight_loop_contents();
    }
    // staging failed: video/core1 are down: reboot back to the launcher
    printf("xip: stage failed %d\n", rc);
    watchdog_reboot(0, 0, 200);
    while (1) tight_loop_contents();
}
