// sync32 SD: mount, list .s32 ROMs, read them, and per-game save slots.
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "ff.h"
#include "f_util.h"
#include "sdcard.h"
#include "sync32.h"
#include "s32tar.h"

static FATFS fs;
static int mounted;
static char game_id_hex[17];

void sd_unmount_for_msc(void) {
    if (mounted) { f_unmount(""); mounted = 0; }
}

int sd_mount(void) {
    if (mounted) return 0;
    printf("SD: mounting...\n");
    FRESULT fr = f_mount(&fs, "", 1);
    printf("SD: f_mount -> %d\n", (int)fr);
    if (fr != FR_OK) return -1;
    mounted = 1;
    return 0;
}

// Read `key = value` out of info.txt (ABI 3.3). Deliberately literal:
// split at the first '=', trim, ignore blank lines, '#' comments and any
// line that does not parse. A malformed file costs a default, not an error.
static bool info_txt_get(const char *dir, const char *key, char *out, size_t cap) {
    char path[80];
    if (snprintf(path, sizeof path, "%s/info.txt", dir) <= 0) return false;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    char line[128];
    bool hit = false;
    while (f_gets(line, sizeof line, &f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        char *ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        if (*k == '#' || strcmp(k, key)) continue;
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' ||
                          ve[-1] == ' '  || ve[-1] == '\t')) *--ve = 0;
        strncpy(out, v, cap - 1);
        out[cap - 1] = 0;
        hit = out[0] != 0;
        break;
    }
    f_close(&f);
    return hit;
}

// game_id is DERIVED from the game's name with any extension removed
// (ABI 3.3): an id an author has to invent is one they leave blank or
// duplicate, and it keys save files.
static void derive_game_id(const char *name, uint8_t id8[8]) {
    memset(id8, 0, 8);
    for (int i = 0; i < 8 && name[i] && name[i] != '.'; i++)
        id8[i] = (uint8_t)name[i];
}

// Fill title/xip/game_id from a 64-byte header read at `off`.
static bool read_hdr(FIL *f, uint32_t off, rom_entry_t *e) {
    uint8_t hdr[64]; UINT br;
    if (f_lseek(f, off) != FR_OK) return false;
    if (f_read(f, hdr, 64, &br) != FR_OK || br != 64) return false;
    if (*(uint32_t *)hdr != S32_MAGIC) return false;
    memcpy(e->title, hdr + 0x20, 16);
    e->title[16] = 0;
    e->xip = hdr[28] == 1;
    memcpy(e->game_id, hdr + 48, 8);
    e->code_at = off;
    return true;
}

int sd_list_roms(rom_entry_t *out, int max) {
    if (sd_mount() != 0) return -1;
    static DIR dir; static FILINFO fi;   // ~1.3KB of exFAT state: off the stack
    static FIL f;
    int n = 0;
    if (f_opendir(&dir, "/") != FR_OK) return -2;
    while (n < max && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        rom_entry_t *e = &out[n];
        memset(e, 0, sizeof *e);

        if (fi.fattrib & AM_DIR) {
            // A directory is a game if and only if it holds main.s32e.
            char exe[80];
            if (snprintf(exe, sizeof exe, "%s/main.s32e", fi.fname) <= 0) continue;
            if (f_open(&f, exe, FA_READ) != FR_OK) continue;
            bool ok = read_hdr(&f, 0, e);
            e->size = f_size(&f);
            f_close(&f);
            if (!ok) continue;
            e->form = S32_FORM_DIR;
            strncpy(e->name, fi.fname, sizeof e->name - 1);
            // identity comes from the namespace, not the executable header
            char t[17];
            if (info_txt_get(fi.fname, "title", t, sizeof t)) {
                strncpy(e->title, t, 16); e->title[16] = 0;
            }
            derive_game_id(fi.fname, e->game_id);
            char ic[80];
            if (snprintf(ic, sizeof ic, "%s/icon.bmp", fi.fname) > 0 &&
                f_stat(ic, &fi) == FR_OK) e->has_icon = true;
        } else {
            int len = strlen(fi.fname);
            if (len < 5 || strcasecmp(fi.fname + len - 4, ".s32")) continue;
            if (f_open(&f, fi.fname, FA_READ) != FR_OK) continue;
            e->size = f_size(&f);
            bool ok;
            if (s32tar_is_tar(&f)) {
                // a tar .s32: the header is main.s32e's, inside the archive
                s32tar_entry_t m;
                ok = s32tar_find(&f, "main.s32e", &m) && read_hdr(&f, m.data_off, e);
                if (ok) e->form = S32_FORM_TAR;
            } else {
                ok = read_hdr(&f, 0, e);       // the original raw form
                if (ok) e->form = S32_FORM_RAW;
            }
            f_close(&f);
            if (!ok) continue;
            strncpy(e->name, fi.fname, sizeof e->name - 1);
            derive_game_id(fi.fname, e->game_id);
        }

        if (!out[n].title[0]) {
            strncpy(out[n].title, out[n].name, 16);
            out[n].title[16] = 0;
        }
        n++;
    }
    f_closedir(&dir);
    return n;
}

// 16x16 BMP -> RGB565. Accepts only BITMAPINFOHEADER, 16x16, 24 or 32bpp,
// uncompressed. Anything else is ignored: an icon is decoration (ABI 3.3).
bool sd_load_icon_bmp(const rom_entry_t *r, uint16_t out[256]) {
    if (!r->has_icon || r->form != S32_FORM_DIR) return false;
    if (sd_mount() != 0) return false;
    char path[80];
    if (snprintf(path, sizeof path, "%s/icon.bmp", r->name) <= 0) return false;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    uint8_t h[54]; UINT br;
    bool ok = false;
    if (f_read(&f, h, 54, &br) == FR_OK && br == 54 &&
        h[0] == 'B' && h[1] == 'M') {
        uint32_t off  = h[10] | (h[11]<<8) | (h[12]<<16) | (h[13]<<24);
        uint32_t hsz  = h[14] | (h[15]<<8) | (h[16]<<16) | (h[17]<<24);
        int32_t  w    = (int32_t)(h[18] | (h[19]<<8) | (h[20]<<16) | (h[21]<<24));
        int32_t  hgt  = (int32_t)(h[22] | (h[23]<<8) | (h[24]<<16) | (h[25]<<24));
        uint16_t bpp  = h[28] | (h[29]<<8);
        uint32_t comp = h[30] | (h[31]<<8) | (h[32]<<16) | (h[33]<<24);
        bool flip = hgt > 0;                 // positive height = bottom-up
        if (hgt < 0) hgt = -hgt;
        if (hsz >= 40 && w == 16 && hgt == 16 && comp == 0 &&
            (bpp == 24 || bpp == 32)) {
            int bypp = bpp / 8;
            int stride = ((16 * bypp) + 3) & ~3;   // rows pad to 4 bytes
            uint8_t row[16 * 4 + 3];
            ok = true;
            for (int y = 0; y < 16 && ok; y++) {
                int dy = flip ? (15 - y) : y;
                if (f_lseek(&f, off + (uint32_t)y * stride) != FR_OK ||
                    f_read(&f, row, stride, &br) != FR_OK || br != (UINT)stride) {
                    ok = false; break;
                }
                for (int x = 0; x < 16; x++) {
                    uint8_t b = row[x*bypp], g = row[x*bypp+1], rr = row[x*bypp+2];
                    out[dy*16 + x] = (uint16_t)(((rr >> 3) << 11) |
                                                ((g >> 2) << 5) | (b >> 3));
                }
            }
        }
    }
    f_close(&f);
    return ok;
}

// Read a ROM image starting at `off` (0 for a raw .s32, the member offset
// for a tar). RAM-load games come through here.
int sd_read_rom_at(const char *name, uint32_t off, uint8_t *buf, uint32_t max,
                   uint32_t *size) {
    if (sd_mount() != 0) return -1;
    static FIL f;
    if (f_open(&f, name, FA_READ) != FR_OK) return -2;
    if (off && f_lseek(&f, off) != FR_OK) { f_close(&f); return -3; }
    UINT br;
    FRESULT fr = f_read(&f, buf, max, &br);
    f_close(&f);
    if (fr != FR_OK) return -3;
    *size = br;
    return 0;
}

int sd_read_rom(const char *name, uint8_t *buf, uint32_t max, uint32_t *size) {
    return sd_read_rom_at(name, 0, buf, max, size);
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

// Where a game's code image lives, and what its namespace is, for all three
// forms. The loader wants one contiguous run and a 64-byte header; the
// disk API wants a folder, an archive, or both. Every form ends here.
//
//   RAW  path = NAME.s32,          header at 0,           sidecar folder
//   TAR  path = NAME.s32,          header at main.s32e,   archive members
//   DIR  path = NAME/main.s32e,    header at 0,           the folder
void s32_game_paths(const rom_entry_t *r, char *file, size_t cap,
                    const char **dir, const char **tar) {
    static char dirbuf[48];
    *dir = NULL; *tar = NULL;
    if (r->form == S32_FORM_DIR) {
        snprintf(file, cap, "%s/main.s32e", r->name);
        strncpy(dirbuf, r->name, sizeof dirbuf - 1);
        dirbuf[sizeof dirbuf - 1] = 0;
        *dir = dirbuf;
    } else {
        snprintf(file, cap, "%s", r->name);
        if (r->form == S32_FORM_TAR) *tar = file;
        // RAW keeps its sidecar folder, resolved by name or .s32id
    }
}
