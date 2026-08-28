// sync32 api v2 disk: read-only access to the running game's resource
// namespace (ABI 3.2, 6.6).
//
// The namespace has two backings and a game cannot tell them apart:
//
//   FOLDER   the game's own directory (a folder-form game), or a sidecar
//            "<romname>/" beside a bare .s32. Names are paths beneath it.
//   ARCHIVE  members of the tar the game shipped as. Names are member names.
//
// Where both could answer a name the folder wins, so an asset can be
// replaced for testing without repacking.
//
// A sidecar folder may be bound by game_id rather than by name, so renaming
// a .s32 does not orphan its data: a directory claims a game by holding
// ".s32id" with the 8 raw id bytes. The basename is tried first, so the
// ordinary layout needs no marker.
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "ff.h"
#include "sync32.h"
#include "sdcard.h"
#include "s32tar.h"

extern volatile bool s32_long_op;      // watchdog guard for slow SD ops
int sd_mount(void);

#define S32_DISK_ID_FILE ".s32id"      // 8 raw game_id bytes

static char data_dir[48];              // "" = no folder backing
static char archive[64];               // "" = no archive backing

// An open resource. For a folder it is just the file. For an archive member
// it is the archive file plus the member's byte range, so reads and seeks
// stay inside it.
static struct {
    FIL  f;
    bool used;
    bool member;                       // true = a range inside `archive`
    uint32_t base, size, pos;
} files[S32_DISK_MAX_OPEN];

// ---- binding -------------------------------------------------------------

static bool dir_claims_id(const char *dir, const uint8_t id8[8]) {
    char path[80];
    if (snprintf(path, sizeof path, "%s/%s", dir, S32_DISK_ID_FILE) <= 0)
        return false;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    uint8_t got[8]; UINT br = 0;
    bool ok = (f_read(&f, got, 8, &br) == FR_OK && br == 8 &&
               memcmp(got, id8, 8) == 0);
    f_close(&f);
    return ok;
}

static bool id_is_set(const uint8_t id8[8]) {
    for (int i = 0; i < 8; i++) if (id8[i]) return true;
    return false;
}

// Point the namespace at a folder, an archive, or both.
void s32_disk_set_source(const char *dir, const char *tar_path) {
    data_dir[0] = 0;
    archive[0] = 0;
    for (int i = 0; i < S32_DISK_MAX_OPEN; i++) files[i].used = false;
    if (dir && dir[0] && strlen(dir) < sizeof data_dir) strcpy(data_dir, dir);
    if (tar_path && tar_path[0] && strlen(tar_path) < sizeof archive)
        strcpy(archive, tar_path);
}

void s32_disk_set_dir_id(const char *rom_filename, const uint8_t game_id[8]) {
    data_dir[0] = 0;
    archive[0] = 0;
    for (int i = 0; i < S32_DISK_MAX_OPEN; i++) files[i].used = false;

    // 1. "planes.s32" -> "planes/", if it exists. The ordinary layout, and
    //    it costs no marker file.
    if (rom_filename) {
        const char *dot = strrchr(rom_filename, '.');
        size_t n = dot ? (size_t)(dot - rom_filename) : 0;
        if (n > 0 && n < sizeof data_dir) {
            char cand[sizeof data_dir];
            memcpy(cand, rom_filename, n);
            cand[n] = 0;
            FILINFO fi;
            if (f_stat(cand, &fi) == FR_OK && (fi.fattrib & AM_DIR)) {
                memcpy(data_dir, cand, n + 1);
                return;
            }
        }
    }

    // 2. Otherwise a directory that claims this game_id: survives renaming.
    if (!game_id || !id_is_set(game_id)) return;
    if (sd_mount() != 0) return;
    DIR dir; FILINFO fi;
    if (f_opendir(&dir, "/") != FR_OK) return;
    s32_long_op = true;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (!(fi.fattrib & AM_DIR)) continue;
        if (strlen(fi.fname) >= sizeof data_dir) continue;
        if (dir_claims_id(fi.fname, game_id)) { strcpy(data_dir, fi.fname); break; }
    }
    s32_long_op = false;
    f_closedir(&dir);
}

void s32_disk_set_dir(const char *rom_filename) {
    s32_disk_set_dir_id(rom_filename, NULL);
}

// ---- name sandbox --------------------------------------------------------

// A name may contain '/' so a game can organise its own resources
// ("roms/smb.nes"), but never "..", a leading '/', a drive letter or a
// backslash: there is no route out of the namespace. The console's own
// files are invisible.
static bool name_ok(const char *name) {
    if (!name || !name[0]) return false;
    if (name[0] == '/') return false;
    if (!strcmp(name, S32_DISK_ID_FILE)) return false;
    if (!strcmp(name, "main.s32e") || !strcmp(name, "info.txt") ||
        !strcmp(name, "icon.bmp")) return false;
    size_t n = 0;
    for (const char *p = name; *p; p++, n++) {
        if (*p == '\\' || *p == ':') return false;
        if (*p == '.' && p[1] == '.') return false;
    }
    return n < S32_DISK_NAME_MAX;
}

// ---- open / list ---------------------------------------------------------

static int alloc_fd(void) {
    for (int i = 0; i < S32_DISK_MAX_OPEN; i++)
        if (!files[i].used) return i;
    return -1;
}

int s32_disk_open(const char *name) {
    if (!name_ok(name)) return S32_DISK_EINVAL;
    if (!data_dir[0] && !archive[0]) return S32_DISK_EIO;
    if (sd_mount() != 0) return S32_DISK_EIO;
    int fd = alloc_fd();
    if (fd < 0) return S32_DISK_ENFILE;

    // folder first: it wins over an archive member of the same name
    if (data_dir[0]) {
        char path[128];
        int r = snprintf(path, sizeof path, "%s/%s", data_dir, name);
        if (r > 0 && (size_t)r < sizeof path) {
            s32_long_op = true;
            FRESULT fr = f_open(&files[fd].f, path, FA_READ);
            s32_long_op = false;
            if (fr == FR_OK) {
                files[fd].used = true;
                files[fd].member = false;
                return fd;
            }
            if (fr != FR_NO_FILE && fr != FR_NO_PATH) return S32_DISK_EIO;
        }
    }

    if (archive[0]) {
        s32_long_op = true;
        FRESULT fr = f_open(&files[fd].f, archive, FA_READ);
        s32_long_op = false;
        if (fr != FR_OK) return S32_DISK_EIO;
        s32tar_entry_t e;
        if (s32tar_find(&files[fd].f, name, &e) &&
            f_lseek(&files[fd].f, e.data_off) == FR_OK) {
            files[fd].used = true;
            files[fd].member = true;
            files[fd].base = e.data_off;
            files[fd].size = e.size;
            files[fd].pos = 0;
            return fd;
        }
        f_close(&files[fd].f);
    }
    return S32_DISK_ENOENT;
}

int s32_disk_list(int index, char *name_out, uint32_t *size_out) {
    if (index < 0 || !name_out) return S32_DISK_EINVAL;
    if (sd_mount() != 0) return S32_DISK_EIO;

    // Folder entries come first, then archive members, so the folder-wins
    // rule holds for enumeration too. One level only: a directory is
    // reported with a trailing '/' and size 0 (ABI 6.6).
    int n = 0;
    if (data_dir[0]) {
        DIR dir; FILINFO fi;
        if (f_opendir(&dir, data_dir) == FR_OK) {
            s32_long_op = true;
            while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
                if (!strcmp(fi.fname, S32_DISK_ID_FILE)) continue;
                if (!strcmp(fi.fname, "main.s32e") ||
                    !strcmp(fi.fname, "info.txt")  ||
                    !strcmp(fi.fname, "icon.bmp")) continue;
                if (n++ == index) {
                    int len = snprintf(name_out, S32_DISK_NAME_MAX, "%s%s",
                                       fi.fname, (fi.fattrib & AM_DIR) ? "/" : "");
                    (void)len;
                    if (size_out) *size_out = (fi.fattrib & AM_DIR) ? 0 : fi.fsize;
                    s32_long_op = false;
                    f_closedir(&dir);
                    return S32_DISK_EOK;
                }
            }
            s32_long_op = false;
            f_closedir(&dir);
        }
    }
    if (archive[0]) {
        FIL f;
        if (f_open(&f, archive, FA_READ) != FR_OK) return S32_DISK_ENOENT;
        s32tar_entry_t e;
        bool hit = s32tar_list(&f, index - n, &e);
        f_close(&f);
        if (hit) {
            strncpy(name_out, e.name, S32_DISK_NAME_MAX - 1);
            name_out[S32_DISK_NAME_MAX - 1] = 0;
            if (size_out) *size_out = e.size;
            return S32_DISK_EOK;
        }
    }
    return S32_DISK_ENOENT;
}

// ---- read / seek ---------------------------------------------------------

static bool fd_ok(int fd) {
    return fd >= 0 && fd < S32_DISK_MAX_OPEN && files[fd].used;
}

int s32_disk_size(int fd) {
    if (!fd_ok(fd)) return S32_DISK_EBADF;
    return files[fd].member ? (int)files[fd].size : (int)f_size(&files[fd].f);
}

int s32_disk_seek(int fd, uint32_t offset) {
    if (!fd_ok(fd)) return S32_DISK_EBADF;
    s32_long_op = true;
    FRESULT fr;
    if (files[fd].member) {
        if (offset > files[fd].size) offset = files[fd].size;
        files[fd].pos = offset;
        fr = f_lseek(&files[fd].f, files[fd].base + offset);
    } else {
        fr = f_lseek(&files[fd].f, offset);
    }
    s32_long_op = false;
    return fr == FR_OK ? S32_DISK_EOK : S32_DISK_EIO;
}

int s32_disk_read(int fd, void *dst, uint32_t len) {
    if (!fd_ok(fd)) return S32_DISK_EBADF;
    if (!dst) return S32_DISK_EINVAL;
    if (files[fd].member) {                 // never read past the member
        uint32_t left = files[fd].size - files[fd].pos;
        if (len > left) len = left;
        if (!len) return 0;
    }
    UINT br = 0;
    s32_long_op = true;
    FRESULT fr = f_read(&files[fd].f, dst, len, &br);
    s32_long_op = false;
    if (fr != FR_OK) return S32_DISK_EIO;
    if (files[fd].member) files[fd].pos += br;
    return (int)br;
}

int s32_disk_close(int fd) {
    if (!fd_ok(fd)) return S32_DISK_EBADF;
    f_close(&files[fd].f);
    files[fd].used = false;
    return S32_DISK_EOK;
}
