// HDMI data-island scheduling: splits the back porch of a scanline into
// [preamble+guard][32-symbol packet][remaining porch] so audio rides the
// existing DVI link. Only used when audio is armed; otherwise the porch is
// emitted as one control run exactly as before (bit-identical to DVI).
//
// Layout inside the 48-clock back porch of 640x480p60:
//   8 clocks  data-island preamble (CTL0=1,CTL1=0,CTL2=1,CTL3=0 on ch1/ch2)
//   2 clocks  leading guard band
//   32 clocks TERC4 packet
//   2 clocks  trailing guard band
//   4 clocks  remaining control period
#include <string.h>
#include "dvi.h"
#include "hdmi_island.h"

#define ISLAND_PREAMBLE 0   /* preamble lives in the preceding control run */
#define ISLAND_GUARD    2
#define ISLAND_PACKET   32
#define ISLAND_TOTAL    (ISLAND_PREAMBLE + ISLAND_GUARD + ISLAND_PACKET + ISLAND_GUARD)

// Guard-band symbols (HDMI 1.4b 5.2.3.3): fixed codes, ch0 carries sync.
// ch1/ch2 use 0x4cc; ch0 uses the normal control symbol for its sync state.
#define GUARD_SYM_12 0x133   /* HDMI 1.4b 5.2.3.3: 0b0100110011 */

// Per-scanline island storage: 3 lanes x (preamble+guards+packet) symbol
// pairs. Double buffered so the DMA can read one while we fill the other.
// DMA reads 20-bit words holding TWO consecutive 10-bit symbols
// (low symbol first), the same packing dvi_ctrl_syms uses.
#define ISLAND_WORDS (ISLAND_TOTAL / 2)
typedef struct {
    uint32_t word[3][ISLAND_WORDS];
} island_buf_t;

static island_buf_t island_bufs[2];
static volatile int island_fill;        // buffer being filled
static volatile bool island_armed;      // audio running?

// packed control symbol for a given sync state, single (not doubled)
extern const uint32_t dvi_ctrl_syms[4];
static inline uint32_t ctrl_single(bool vsync, bool hsync) {
    return dvi_ctrl_syms[(!!vsync << 1) | !!hsync] & 0x3ff;
}

void hdmi_island_arm(bool on) { island_armed = on; }
bool hdmi_island_is_armed(void) { return island_armed; }

// A built island waits here until a scanline consumes it. The audio pump
// builds at most one per line; the scanline builder takes whatever is ready.
static volatile int island_ready = -1;
static bool island_inited;
bool hdmi_island_pending(void) { return island_armed && island_ready >= 0; }
int hdmi_island_ready(void) {
    if (!island_inited) {          // both buffers start as NULL packets so
        island_inited = true;      // the always-present block is always legal
        hdmi_packet_t null_pkt;
        memset(&null_pkt, 0, sizeof null_pkt);
        hdmi_island_build(&null_pkt, false, false);
        hdmi_island_build(&null_pkt, false, false);
        island_ready = -1;
    }
    return island_ready < 0 ? island_fill : island_ready;
}
void hdmi_island_consume(void) { island_ready = -1; }

// Fill the next island buffer with one packet. Returns the buffer index.
int hdmi_island_build(const hdmi_packet_t *pkt, bool hsync_active, bool vsync) {
    int b = island_fill ^ 1;
    island_buf_t *ib = &island_bufs[b];
    uint32_t pkt_syms[3][32];
    hdmi_encode_island(pkt, hsync_active, vsync, true, pkt_syms);

    uint32_t guard0 = hdmi_terc4[(hsync_active ? 1 : 0) | ((vsync ? 1 : 0) << 1) | 0xc];
    for (int ch = 0; ch < 3; ch++) {
        uint32_t lane[ISLAND_TOTAL];
        int i = 0;
        // Data-island preamble (HDMI 1.4b 5.2.1.1): CTL0=1 CTL1=0 CTL2=1
        // CTL3=0, i.e. BOTH ch1 and ch2 carry the 0b01 control symbol.
        // (Sending 0b00 on ch2 is the video-period preamble, and a sink
        // never recognises the island at all.)
        for (int k = 0; k < ISLAND_PREAMBLE; k++)
            lane[i++] = (ch == 0) ? ctrl_single(vsync, hsync_active)
                                  : ctrl_single(false, true);
        for (int k = 0; k < ISLAND_GUARD; k++)
            lane[i++] = (ch == 0) ? guard0 : GUARD_SYM_12;
        for (int k = 0; k < ISLAND_PACKET; k++)
            lane[i++] = pkt_syms[ch][k];
        for (int k = 0; k < ISLAND_GUARD; k++)
            lane[i++] = (ch == 0) ? guard0 : GUARD_SYM_12;
        for (int w = 0; w < ISLAND_WORDS; w++)
            ib->word[ch][w] = lane[w * 2] | (lane[w * 2 + 1] << 10);
    }
    island_fill = b;
    island_ready = b;
    return b;
}

const uint32_t *hdmi_island_words(int buf, int lane) {
    return island_bufs[buf].word[lane];
}
int hdmi_island_words_len(void) { return ISLAND_WORDS; }
