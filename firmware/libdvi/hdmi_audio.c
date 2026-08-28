// HDMI audio packet + TERC4 encoding (see hdmi_audio.h).
#include <string.h>
#include "hdmi_audio.h"

// HDMI 1.4b table 5-4: TERC4 encoding, 4 bits -> 10-bit symbol
const uint16_t hdmi_terc4[16] = {
    0x29c, 0x263, 0x2e4, 0x2e2,
    0x171, 0x11e, 0x18e, 0x13c,
    0x2cc, 0x139, 0x19c, 0x2c6,
    0x28e, 0x271, 0x163, 0x2c3,
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

// IEC 60958 subframe pair (HDMI 1.4b 7.2): 16-bit PCM sits in bytes 1-2 of
// each channel's 3-byte sample field, byte 0 stays zero (it is the low 8 bits
// of a 24-bit sample we do not have). Byte 6 carries the V/U/C/P bits for
// both channels: V (bit0/bit4) marks the sample VALID and must be set, and P
// (bit3/bit7) is even parity over the two PCM bytes plus those status bits.
// Leaving V clear, or computing the parity over a different bit set, makes a
// sink reject every sample even though the packet's BCH ECC is perfect.
static int byte_parity(uint8_t x) {
    int p = 0;
    while (x) { p ^= x & 1; x >>= 1; }
    return p;
}

static void pack_subframe(uint8_t out[7], int16_t l, int16_t r) {
    const uint8_t vuc = 1;                     // valid, no user data, no status
    out[0] = 0;
    out[1] = (uint8_t)l;
    out[2] = (uint8_t)((uint16_t)l >> 8);
    out[3] = 0;
    out[4] = (uint8_t)r;
    out[5] = (uint8_t)((uint16_t)r >> 8);
    int pl = byte_parity(out[1]) ^ byte_parity(out[2]) ^ byte_parity(vuc);
    int pr = byte_parity(out[4]) ^ byte_parity(out[5]) ^ byte_parity(vuc);
    out[6] = (uint8_t)((vuc << 0) | (pl << 3) | (vuc << 4) | (pr << 7));
}

void hdmi_pkt_audio(hdmi_packet_t *p, const int16_t *lr, int frames,
                    bool frame0_is_block_start, uint8_t frame_counter) {
    memset(p, 0, sizeof *p);
    if (frames < 1) frames = 1;
    if (frames > 4) frames = 4;
    uint8_t layout = 0;                        // layout 0 = 2 channels
    uint8_t sample_present = (uint8_t)((1u << frames) - 1);
    uint8_t sample_flat = 0;                   // no muted samples
    // header[2] carries B in the UPPER nibble: it flags which frame in the
    // packet starts a 192-frame IEC channel-status block.
    uint8_t b = frame0_is_block_start ? 0x01 : 0x00;
    p->header[0] = HDMI_PKT_AUDIO;
    p->header[1] = (uint8_t)(sample_present | (layout << 4));
    p->header[2] = (uint8_t)((b << 4) | sample_flat);
    (void)frame_counter;
    for (int i = 0; i < frames; i++)
        pack_subframe(p->sub[i], lr[i * 2], lr[i * 2 + 1]);
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
// A data island is 32 TERC4 "pixels". Each pixel carries ONE header bit and
// TWO consecutive bits from EACH of the four subpackets (HDMI 1.4b 5.2.3.4:
// "BCH packets 0 to 3 are transferred two bits at a time"). So a subpacket's
// bit index is px*2 and px*2+1 -- 32 pixels x 2 bits = the full 64-bit
// subpacket, while the 32-bit header sends one bit per pixel.
//
// Channel assignment (HDMI 1.4b, data_island_data nibble per channel):
//   ch0 = {header_bit, island_started, vsync, hsync}
//   ch1 = the FIRST bit of each of sub0..sub3
//   ch2 = the SECOND bit of each of sub0..sub3
// Splitting sub0/sub1 onto ch1 and sub2/sub3 onto ch2 is NOT the layout: it
// scrambles all four subpackets and drops half of every one of them.
static inline uint8_t bit_at(const uint8_t *bytes, int idx) {
    return (uint8_t)((bytes[idx >> 3] >> (idx & 7)) & 1);
}

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
        // channel 0: hsync bit0, vsync bit1, island-started bit2, header bit3
        uint8_t started = (uint8_t)((px == 0 && first_packet) ? 0 : 1);
        uint8_t c0 = (uint8_t)((hsync ? 1 : 0) | ((vsync ? 1 : 0) << 1) |
                               (started << 2) | (bit_at(hdr, px) << 3));
        out[0][px] = hdmi_terc4[c0];
        // channels 1 and 2: bit px*2 and px*2+1 of every subpacket
        uint8_t n1 = 0, n2 = 0;
        for (int s = 0; s < 4; s++) {
            n1 = (uint8_t)(n1 | (bit_at(sub[s], px * 2) << s));
            n2 = (uint8_t)(n2 | (bit_at(sub[s], px * 2 + 1) << s));
        }
        out[1][px] = hdmi_terc4[n1];
        out[2][px] = hdmi_terc4[n2];
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
