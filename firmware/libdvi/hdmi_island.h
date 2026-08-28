#ifndef HDMI_ISLAND_H
#define HDMI_ISLAND_H
#include <stdint.h>
#include <stdbool.h>
#include "hdmi_audio.h"
void hdmi_island_init(void);
void hdmi_island_arm(bool on);
bool hdmi_island_is_armed(void);
bool hdmi_island_pending(void);
int  hdmi_island_ready(void);
int  hdmi_island_build(const hdmi_packet_t *pkt, bool hsync_active, bool vsync);
const uint32_t *hdmi_island_words(int buf, int lane);
int  hdmi_island_words_len(void);
void hdmi_island_consume(void);
#endif
