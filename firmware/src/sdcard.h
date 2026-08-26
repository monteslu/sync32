#ifndef SYNC32_SDCARD_H
#define SYNC32_SDCARD_H
#include <stdint.h>
typedef struct { char name[48]; char title[17]; uint8_t xip; uint8_t game_id[8]; uint32_t size; } rom_entry_t;
int  sd_mount(void);                    // 0 ok
int  sd_list_roms(rom_entry_t *out, int max);
int  sd_read_rom(const char *name, uint8_t *buf, uint32_t max, uint32_t *size);
void sd_set_game_id(const uint8_t *id8);
#endif
