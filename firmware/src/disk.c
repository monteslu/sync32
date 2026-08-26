// sync32 api v2 disk: read-only streaming from the running game's data dir
// ("<romname>/" beside the .s32). Sandboxed: plain filenames only.
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "ff.h"
#include "sync32.h"
#include "sdcard.h"

extern volatile bool s32_long_op;      // watchdog guard for slow SD ops
int sd_mount(void);

static char data_dir[40];              // "" = no data dir (embedded demo etc)

static struct { FIL f; bool used; } files[S32_DISK_MAX_OPEN];

void s32_disk_set_dir(const char *rom_filename) {
    // "planes.s32" -> "planes"; NULL or no dot -> disabled
    data_dir[0] = 0;
    for (int i = 0; i < S32_DISK_MAX_OPEN; i++) files[i].used = false;
    if (!rom_filename) return;
    const char *dot = strrchr(rom_filename, '.');
    size_t n = dot ? (size_t)(dot - rom_filename) : 0;
    if (n == 0 || n >= sizeof data_dir) return;
    memcpy(data_dir, rom_filename, n);
    data_dir[n] = 0;
}

static bool name_ok(const char *name) {
    if (!name || !name[0]) return false;
    size_t n = 0;
    for (const char *p = name; *p; p++, n++)
        if (*p == '/' || *p == '\\' || *p == ':' || (*p == '.' && p[1] == '.'))
            return false;
    return n < S32_DISK_NAME_MAX;
}

static int build_path(char *out, size_t cap, const char *name) {
    if (!data_dir[0]) return S32_DISK_EIO;
    if (!name_ok(name)) return S32_DISK_EINVAL;
    int r = snprintf(out, cap, "%s/%s", data_dir, name);
    return (r > 0 && (size_t)r < cap) ? 0 : S32_DISK_EINVAL;
}

int s32_disk_list(int index, char *name_out, uint32_t *size_out) {
    if (index < 0 || !name_out) return S32_DISK_EINVAL;
    if (!data_dir[0] || sd_mount() != 0) return S32_DISK_EIO;
    DIR dir; FILINFO fi;
    if (f_opendir(&dir, data_dir) != FR_OK) return S32_DISK_EIO;
    int n = 0, rc = S32_DISK_ENOENT;
    s32_long_op = true;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        if (n++ == index) {
            strncpy(name_out, fi.fname, S32_DISK_NAME_MAX - 1);
            name_out[S32_DISK_NAME_MAX - 1] = 0;
            if (size_out) *size_out = fi.fsize;
            rc = S32_DISK_EOK;
            break;
        }
    }
    s32_long_op = false;
    f_closedir(&dir);
    return rc;
}

int s32_disk_open(const char *name) {
    char path[80];
    int rc = build_path(path, sizeof path, name);
    if (rc) return rc;
    if (sd_mount() != 0) return S32_DISK_EIO;
    int fd = -1;
    for (int i = 0; i < S32_DISK_MAX_OPEN; i++)
        if (!files[i].used) { fd = i; break; }
    if (fd < 0) return S32_DISK_ENFILE;
    s32_long_op = true;
    FRESULT fr = f_open(&files[fd].f, path, FA_READ);
    s32_long_op = false;
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) return S32_DISK_ENOENT;
    if (fr != FR_OK) return S32_DISK_EIO;
    files[fd].used = true;
    return fd;
}

static bool fd_ok(int fd) {
    return fd >= 0 && fd < S32_DISK_MAX_OPEN && files[fd].used;
}

int s32_disk_size(int fd) {
    if (!fd_ok(fd)) return S32_DISK_EBADF;
    return (int)f_size(&files[fd].f);
}

int s32_disk_seek(int fd, uint32_t offset) {
    if (!fd_ok(fd)) return S32_DISK_EBADF;
    s32_long_op = true;
    FRESULT fr = f_lseek(&files[fd].f, offset);
    s32_long_op = false;
    return fr == FR_OK ? S32_DISK_EOK : S32_DISK_EIO;
}

int s32_disk_read(int fd, void *dst, uint32_t len) {
    if (!fd_ok(fd)) return S32_DISK_EBADF;
    if (!dst) return S32_DISK_EINVAL;
    UINT br = 0;
    s32_long_op = true;
    FRESULT fr = f_read(&files[fd].f, dst, len, &br);
    s32_long_op = false;
    return fr == FR_OK ? (int)br : S32_DISK_EIO;
}

int s32_disk_close(int fd) {
    if (!fd_ok(fd)) return S32_DISK_EBADF;
    f_close(&files[fd].f);
    files[fd].used = false;
    return S32_DISK_EOK;
}
