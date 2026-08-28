// Walking an uncompressed tar (ABI 3.4). A .s32 archive is an ordinary tar,
// so the console reads one with a few hundred bytes of code and no
// decompressor: members are contiguous, 512-aligned, and preceded by their
// own header, which is exactly what the loader and the disk API need.
#ifndef S32TAR_H
#define S32TAR_H
#include <stdint.h>
#include <stdbool.h>
#include "ff.h"

#define S32TAR_NAME_MAX 100

typedef struct {
    char     name[S32TAR_NAME_MAX + 1];  // leading "./" already stripped
    uint32_t data_off;                   // absolute file offset of the data
    uint32_t size;
} s32tar_entry_t;

// Is this file a tar? Cheap: "ustar" at offset 257 of the first block.
bool s32tar_is_tar(FIL *f);

// Find one member by name. Returns true and fills `out` on a hit.
// Names compare with any leading "./" ignored on both sides.
bool s32tar_find(FIL *f, const char *name, s32tar_entry_t *out);

// Enumerate regular-file members. `index` counts only members the console
// exposes: directories, PaxHeader/ and other metadata are skipped, as are
// the console's own files. Returns false once index is past the end.
bool s32tar_list(FIL *f, int index, s32tar_entry_t *out);

#endif
