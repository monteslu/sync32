// HDMI audio over the existing DVI link: data islands in the horizontal
// blanking interval. Pure packet/encoding layer here so it can be unit
// tested on the host; the scheduler that hands islands to the DMA
// blocklist lives in hdmi_audio.c alongside the scanline builder.
//
// References: HDMI 1.4b sections 5.2 (TERC4), 5.3.3 (data islands),
// 7.2 (audio sample packet), 7.2.3 (audio clock regeneration).
#ifndef HDMI_AUDIO_H
#define HDMI_AUDIO_H
#include <stdint.h>
#include <stdbool.h>

// TERC4: 4 bits -> one 10-bit TMDS symbol (HDMI 1.4b table 5-4)
extern const uint16_t hdmi_terc4[16];

// Packet types we emit
#define HDMI_PKT_NULL        0x00
#define HDMI_PKT_ACR         0x01   // audio clock regeneration (N/CTS)
#define HDMI_PKT_AUDIO       0x02   // audio sample packet
#define HDMI_PKT_AVI_INFO    0x82   // AVI InfoFrame
#define HDMI_PKT_AUDIO_INFO  0x84   // audio InfoFrame

// A packet is a 3-byte header (+1 ECC) and 4 subpackets of 7 bytes (+1 ECC).
typedef struct {
    uint8_t header[3];
    uint8_t sub[4][7];
} hdmi_packet_t;

// BCH ECC used for both the header and each subpacket (HDMI 1.4b 5.2.3.4)
uint8_t hdmi_ecc_header(const uint8_t hdr[3]);
uint8_t hdmi_ecc_sub(const uint8_t sub[7]);

// Audio Sample Packet: up to 4 stereo frames of 16-bit PCM.
// `frames` is 1..4; `present` is the sample_present bitmap.
void hdmi_pkt_audio(hdmi_packet_t *p, const int16_t *lr, int frames,
                    bool frame0_is_block_start, uint8_t frame_counter);

// Audio Clock Regeneration: tells the sink how to derive the audio clock
// from the TMDS clock. For 48kHz at a 25.2MHz pixel clock, N=6144 and
// CTS=25200 (HDMI 1.4b table 7-1..7-3).
void hdmi_pkt_acr(hdmi_packet_t *p, uint32_t n, uint32_t cts);

// Audio InfoFrame: 2ch, PCM, refer-to-stream sample rate/size.
void hdmi_pkt_audio_infoframe(hdmi_packet_t *p);
void hdmi_pkt_avi_infoframe(hdmi_packet_t *p);

// Encode one packet into 32 TERC4 symbol-pairs per channel.
// out[ch][i] holds two 10-bit symbols packed like dvi_ctrl_syms.
// hsync/vsync drive channel 0's sync bits during the island.
void hdmi_encode_island(const hdmi_packet_t *p, bool hsync, bool vsync,
                        bool first_packet, uint32_t out[3][32]);

#endif
