// HDMI audio packet + TERC4 encoding (see hdmi_audio.h).
#include <string.h>
#include "hdmi_audio.h"

// HDMI 1.4b table 5-4: TERC4 encoding, 4 bits -> 10-bit symbol
const uint16_t hdmi_terc4[16] = {
    0x29c, 0x263, 0x2e4, 0x2e2, 0x171, 0x11e, 0x18e, 0x13c,
    0x39c, 0x2cc, 0x31c, 0x234, 0x2c4, 0x1cc, 0x1c4, 0x1a4,
};

// BCH(255,247) over GF(2) with generator x^8 + x^7 + x^6 + x^4 + 1 (0xd1),
// processed LSB-first (HDMI 1.4b 5.2.3.4).
static uint8_t bch(const uint8_t *data, int nbytes) {
    uint8_t crc = 0;
    for (int i = 0; i < nbytes; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint8_t)((crc >> 1) ^ 0x83) : (uint8_t)(crc >> 1);
    }
    return crc;
}

uint8_t hdmi_ecc_header(const uint8_t hdr[3]) { return bch(hdr, 3); }
uint8_t hdmi_ecc_sub(const uint8_t sub[7]) { return bch(sub, 7); }

// IEC 60958 subframe: 24-bit sample field (we place 16-bit PCM in the MSBs)
// plus V/U/C/P status bits. HDMI packs L and R into one 7-byte subpacket.
static void pack_subframe(uint8_t out[7], int16_t l, int16_t r,
                          bool channel_status_bit_l, bool channel_status_bit_r) {
    uint32_t lw = ((uint32_t)(uint16_t)l) << 8;   // 16-bit PCM -> upper bits
    uint32_t rw = ((uint32_t)(uint16_t)r) << 8;
    out[0] = (uint8_t)(lw & 0xff);
    out[1] = (uint8_t)((lw >> 8) & 0xff);
    out[2] = (uint8_t)((lw >> 16) & 0xff);
    out[3] = (uint8_t)(rw & 0xff);
    out[4] = (uint8_t)((rw >> 8) & 0xff);
    out[5] = (uint8_t)((rw >> 16) & 0xff);
    // byte 6: bits for both channels - V=0 (valid), U=0, C=status, P=parity
    uint8_t bits = 0;
    if (channel_status_bit_l) bits |= 0x04;    // C for left
    if (channel_status_bit_r) bits |= 0x40;    // C for right
    // parity: even parity over the 26 bits of each subframe
    uint32_t pl = lw ^ (bits & 0x07), pr = rw ^ ((bits >> 4) & 0x07);
    int cl = 0, cr = 0;
    for (int i = 0; i < 32; i++) { cl += (pl >> i) & 1; cr += (pr >> i) & 1; }
    if (cl & 1) bits |= 0x08;                  // P for left
    if (cr & 1) bits |= 0x80;                  // P for right
    out[6] = bits;
}

void hdmi_pkt_audio(hdmi_packet_t *p, const int16_t *lr, int frames,
                    bool frame0_is_block_start, uint8_t frame_counter) {
    memset(p, 0, sizeof *p);
    if (frames < 1) frames = 1;
    if (frames > 4) frames = 4;
    uint8_t layout = 0;                        // layout 0 = 2 channels
    uint8_t sample_present = (uint8_t)((1u << frames) - 1);
    uint8_t sample_flat = 0;                   // no muted samples
    uint8_t b = frame0_is_block_start ? 0x01 : 0x00;
    p->header[0] = HDMI_PKT_AUDIO;
    p->header[1] = (uint8_t)(sample_present | (layout << 4));
    p->header[2] = (uint8_t)((sample_flat << 4) | b);
    (void)frame_counter;
    for (int i = 0; i < frames; i++) {
        bool cs = frame0_is_block_start && i == 0;
        pack_subframe(p->sub[i], lr[i * 2], lr[i * 2 + 1], cs, cs);
    }
}

void hdmi_pkt_acr(hdmi_packet_t *p, uint32_t n, uint32_t cts) {
    memset(p, 0, sizeof *p);
    p->header[0] = HDMI_PKT_ACR;
    for (int i = 0; i < 4; i++) {              // all four subpackets identical
        p->sub[i][0] = 0;
        p->sub[i][1] = (uint8_t)((cts >> 16) & 0x0f);
        p->sub[i][2] = (uint8_t)((cts >> 8) & 0xff);
        p->sub[i][3] = (uint8_t)(cts & 0xff);
        p->sub[i][4] = (uint8_t)((n >> 16) & 0x0f);
        p->sub[i][5] = (uint8_t)((n >> 8) & 0xff);
        p->sub[i][6] = (uint8_t)(n & 0xff);
    }
}

void hdmi_pkt_audio_infoframe(hdmi_packet_t *p) {
    memset(p, 0, sizeof *p);
    p->header[0] = HDMI_PKT_AUDIO_INFO;
    p->header[1] = 0x01;                       // version
    p->header[2] = 0x0a;                       // length
    uint8_t d[6] = {0};
    d[1] = 0x01;                               // CC=2 channels, CT=refer to stream
    d[2] = 0x00;                               // SF/SS refer to stream
    d[3] = 0x00;
    d[4] = 0x00;                               // CA = FR,FL
    // checksum over header + data must make the sum zero
    uint8_t sum = p->header[0] + p->header[1] + p->header[2];
    for (int i = 1; i < 6; i++) sum = (uint8_t)(sum + d[i]);
    d[0] = (uint8_t)(0x100 - sum);
    memcpy(p->sub[0], d, 6);
}

// ---- island encoding -----------------------------------------------------
// A data island carries 32 TERC4 "pixels" per packet: each pixel encodes
// 1 bit from the header and 2 bits from each of the four subpackets.
void hdmi_encode_island(const hdmi_packet_t *p, bool hsync, bool vsync,
                        bool first_packet, uint32_t out[3][32]) {
    uint8_t hdr[4], sub[4][8];
    memcpy(hdr, p->header, 3);
    hdr[3] = hdmi_ecc_header(p->header);
    for (int i = 0; i < 4; i++) {
        memcpy(sub[i], p->sub[i], 7);
        sub[i][7] = hdmi_ecc_sub(p->sub[i]);
    }
    for (int px = 0; px < 32; px++) {
        // channel 0: HSYNC, VSYNC, header bit, and the island preamble flag
        uint8_t hbit = (uint8_t)((hdr[px >> 3] >> (px & 7)) & 1);
        uint8_t c0 = (uint8_t)((hsync ? 1 : 0) | ((vsync ? 1 : 0) << 1) |
                               (hbit << 2) |
                               (((px == 0 && first_packet) ? 0 : 1) << 3));
        out[0][px] = hdmi_terc4[c0];
        // channels 1 and 2: two subpacket bits each
        for (int ch = 1; ch <= 2; ch++) {
            int s0 = (ch - 1) * 2, s1 = s0 + 1;
            uint8_t b0 = (uint8_t)((sub[s0][px >> 3] >> (px & 7)) & 1);
            uint8_t b1 = (uint8_t)((sub[s1][px >> 3] >> (px & 7)) & 1);
            uint8_t nibble = (uint8_t)(b0 | (b1 << 1));
            out[ch][px] = hdmi_terc4[nibble];
        }
    }
}

// AVI InfoFrame: without this a sink treats the stream as DVI and ignores
// every data island, including audio. RGB, no overscan, 640x480p (VIC 1).
void hdmi_pkt_avi_infoframe(hdmi_packet_t *p) {
    memset(p, 0, sizeof *p);
    p->header[0] = HDMI_PKT_AVI_INFO;
    p->header[1] = 0x02;                       // version 2
    p->header[2] = 0x0d;                       // length 13
    uint8_t d[14] = {0};
    d[1] = 0x00;                               // RGB, no active-format info
    d[2] = 0x08;                               // no aspect, same as picture
    d[3] = 0x00;                               // no colorimetry
    d[4] = 0x01;                               // VIC 1 = 640x480p60
    d[5] = 0x00;                               // no pixel repetition
    uint8_t sum = p->header[0] + p->header[1] + p->header[2];
    for (int i = 1; i < 14; i++) sum = (uint8_t)(sum + d[i]);
    d[0] = (uint8_t)(0x100 - sum);
    memcpy(p->sub[0], d, 7);
    memcpy(p->sub[1], d + 7, 7);
}
