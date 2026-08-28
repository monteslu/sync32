// Uncompressed tar walking: see s32tar.h.
#include <string.h>
#include <stdio.h>
#include "s32tar.h"
#include "sync32.h"

extern volatile bool s32_long_op;      // watchdog guard for slow SD ops

// Files the console owns. A game never sees these through the disk API, the
// same way the sidecar marker is hidden (ABI 3.3).
static bool console_own_file(const char *n) {
    return !strcmp(n, "main.s32e") || !strcmp(n, "info.txt") ||
           !strcmp(n, "icon.bmp")  || !strcmp(n, ".s32id");
}

// tar stores sizes as octal ASCII, NUL- or space-terminated.
static uint32_t octal(const char *p, int len) {
    uint32_t v = 0;
    for (int i = 0; i < len && p[i] >= '0' && p[i] <= '7'; i++)
        v = (v << 3) | (uint32_t)(p[i] - '0');
    return v;
}

// "./foo" and "foo" are the same member: `tar cf x -C dir .` writes the
// first form, `cd dir && tar cf ../x *` the second, and a person will type
// either one.
static const char *strip_dot(const char *n) {
    return (n[0] == '.' && n[1] == '/') ? n + 2 : n;
}

bool s32tar_is_tar(FIL *f) {
    uint8_t blk[512]; UINT br;
    if (f_lseek(f, 0) != FR_OK) return false;
    if (f_read(f, blk, 512, &br) != FR_OK || br != 512) return false;
    return memcmp(blk + 257, "ustar", 5) == 0;
}

// Step the walk one member on. Returns 0 at end of archive, 1 on a member,
// -1 on a read error. `skip` is set for members the console does not expose.
static int next_member(FIL *f, uint32_t *off, s32tar_entry_t *e, bool *skip) {
    uint8_t h[512]; UINT br;
    if (f_lseek(f, *off) != FR_OK) return -1;
    if (f_read(f, h, 512, &br) != FR_OK || br != 512) return -1;
    if (h[0] == 0) return 0;                    // end-of-archive block

    uint32_t size = octal((const char *)h + 124, 12);
    char type = (char)h[156];

    h[99] = 0;                                  // name field is 100 bytes
    const char *nm = strip_dot((const char *)h);

    e->data_off = *off + 512;
    e->size = size;
    strncpy(e->name, nm, S32TAR_NAME_MAX);
    e->name[S32TAR_NAME_MAX] = 0;

    // typeflag '0' and '\0' are regular files. '5' is a directory, and
    // 'x'/'g'/'L'/'K' plus PaxHeader/ names are metadata some tar
    // implementations emit; none of those are resources.
    *skip = !(type == '0' || type == '\0') ||
            !strncmp(nm, "PaxHeader/", 10) ||
            console_own_file(nm);

    *off = e->data_off + ((size + 511u) & ~511u);
    return 1;
}

bool s32tar_find(FIL *f, const char *name, s32tar_entry_t *out) {
    const char *want = strip_dot(name);
    uint32_t off = 0;
    s32tar_entry_t e; bool skip;
    s32_long_op = true;
    int rc;
    while ((rc = next_member(f, &off, &e, &skip)) == 1) {
        if (!strcmp(e.name, want)) {            // exact hit, skipped or not:
            s32_long_op = false;                // the loader needs main.s32e
            *out = e;
            return true;
        }
    }
    s32_long_op = false;
    return false;
}

bool s32tar_list(FIL *f, int index, s32tar_entry_t *out) {
    uint32_t off = 0;
    s32tar_entry_t e; bool skip;
    int n = 0, rc;
    s32_long_op = true;
    while ((rc = next_member(f, &off, &e, &skip)) == 1) {
        if (skip) continue;
        if (n++ == index) { s32_long_op = false; *out = e; return true; }
    }
    s32_long_op = false;
    return false;
}
