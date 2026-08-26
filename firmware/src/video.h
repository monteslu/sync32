#ifndef SYNC32_VIDEO_H
#define SYNC32_VIDEO_H
#include <stdint.h>
#include "sync32.h"

void video_init(void);                 // sets up DVI + launches core1 scanout
void video_palette(const uint16_t *rgb565_256);
void video_clear(uint16_t rgb565);     // records clear color for next render
int  video_sheet_load(const void *px, int w, int h);
void video_sheet_reset(void);          // free all sheets (between games)
void video_sprite(int sheet, int sx, int sy, int w, int h, int x, int y, uint8_t flags);
void video_rect(int x, int y, int w, int h, uint16_t rgb565);
uint8_t *video_canvas(void);
void video_canvas_mark(int y0, int y1);
void video_present(void);              // render list into fb, wait next vblank
uint32_t video_frame_count(void);
extern uint8_t s32_framebuf[320 * 240];
#endif
