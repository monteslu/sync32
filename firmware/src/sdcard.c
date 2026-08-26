// sync32 SD: mount, list .s32 ROMs, read them, and per-game save slots.
#include <string.h>
#include <stdio.h>
#include "ff.h"
#include "f_util.h"
#include "sdcard.h"
#include "sync32.h"

static FATFS fs;
static int mounted;
static char game_id_hex[17];

int sd_mount(void) {
    if (mounted) return 0;
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) return -1;
    mounted = 1;
    return 0;
}

int sd_list_roms(rom_entry_t *out, int max) {
    if (sd_mount() != 0) return -1;
    DIR dir; FILINFO fi;
    int n = 0;
    if (f_opendir(&dir, "/") != FR_OK) return -2;
    while (n < max && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        int len = strlen(fi.fname);
        if (len < 5 || strcasecmp(fi.fname + len - 4, ".s32")) continue;
        strncpy(out[n].name, fi.fname, sizeof out[n].name - 1);
        out[n].name[sizeof out[n].name - 1] = 0;
        out[n].size = fi.fsize;
        // read the title out of the header
        FIL f;
        out[n].title[0] = 0;
        if (f_open(&f, fi.fname, FA_READ) == FR_OK) {
            uint8_t hdr[64]; UINT br;
            if (f_read(&f, hdr, 64, &br) == FR_OK && br == 64 &&
                *(uint32_t *)hdr == S32_MAGIC) {
                memcpy(out[n].title, hdr + 0x20, 16);
                out[n].title[16] = 0;
            }
            f_close(&f);
        }
        if (!out[n].title[0]) strncpy(out[n].title, out[n].name, 16);
        n++;
    }
    f_closedir(&dir);
    return n;
}

int sd_read_rom(const char *name, uint8_t *buf, uint32_t max, uint32_t *size) {
    if (sd_mount() != 0) return -1;
    FIL f;
    if (f_open(&f, name, FA_READ) != FR_OK) return -2;
    UINT br;
    FRESULT fr = f_read(&f, buf, max, &br);
    f_close(&f);
    if (fr != FR_OK) return -3;
    *size = br;
    return 0;
}

void sd_set_game_id(const uint8_t *id8) {
    for (int i = 0; i < 8; i++)
        sprintf(game_id_hex + i * 2, "%02x", id8[i]);
}

// save slots: /saves/<gameid>/slotN.bin: atomic via temp+rename
int s32_save_read(int slot, void *buf, int max) {
    if (slot < 0 || slot > 7 || sd_mount() != 0) return -1;
    char path[64];
    snprintf(path, sizeof path, "/saves/%s/slot%d.bin", game_id_hex, slot);
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return -1;
    UINT br; 
    f_read(&f, buf, max, &br);
    f_close(&f);
    return (int)br;
}

int s32_save_write(int slot, const void *buf, int len) {
    if (slot < 0 || slot > 7 || len > S32_SAVE_MAX || sd_mount() != 0) return -1;
    char dir[40], path[64], tmp[64];
    f_mkdir("/saves");
    snprintf(dir, sizeof dir, "/saves/%s", game_id_hex);
    f_mkdir(dir);
    snprintf(path, sizeof path, "%s/slot%d.bin", dir, slot);
    snprintf(tmp, sizeof tmp, "%s/slot%d.tmp", dir, slot);
    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
    UINT bw;
    FRESULT fr = f_write(&f, buf, len, &bw);
    f_close(&f);
    if (fr != FR_OK || (int)bw != len) return -1;
    f_unlink(path);
    return f_rename(tmp, path) == FR_OK ? len : -1;
}
