#ifndef SYNC32_SDCARD_H
#define SYNC32_SDCARD_H
#include <stdint.h>
#include <stdbool.h>

// How a game is stored on the card (ABI 3.2). The loader needs to know;
// nothing above the loader does.
typedef enum {
    S32_FORM_RAW = 0,   // NAME.s32, header + code image (the original form)
    S32_FORM_TAR,       // NAME.s32 that is a tar; main.s32e is a member
    S32_FORM_DIR,       // a directory containing main.s32e
} s32_form_t;

typedef struct {
    char       name[48];      // "chromium.s32" or "nes"
    char       title[17];
    uint8_t    xip;
    uint8_t    game_id[8];
    uint32_t   size;
    s32_form_t form;
    uint32_t   code_at;       // file offset of the 64-byte header
    bool       has_icon;      // an icon.bmp the launcher could load
} rom_entry_t;

int  sd_mount(void);                    // 0 ok
int  sd_list_roms(rom_entry_t *out, int max);
int  sd_read_rom(const char *name, uint8_t *buf, uint32_t max, uint32_t *size);
int  sd_read_rom_at(const char *name, uint32_t off, uint8_t *buf, uint32_t max,
                    uint32_t *size);
void sd_set_game_id(const uint8_t *id8);

// Load a game's icon.bmp into a 16x16 RGB565 buffer. False if there is no
// usable icon: an icon is decoration, so every failure is silent (ABI 3.3).
bool sd_load_icon_bmp(const rom_entry_t *r, uint16_t out[256]);

// Resolve a game to the file holding its code and to its namespace backings.
void s32_game_paths(const rom_entry_t *r, char *file, size_t cap,
                    const char **dir, const char **tar);
#endif
