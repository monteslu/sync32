// HSTX island packing check.
//
// libdvi hands islands to PIO as three PER-LANE arrays, two 10-bit symbols to
// a word. HSTX has one shared shift register that every lane indexes into, so
// video_hstx.c transposes the same island into one word per SYMBOL with the
// lanes 10 bits apart. This checks that transposition round-trips.
//
// It matters because getting it wrong is NOT a crash and NOT a broken
// picture: the video looks perfect and the sink silently rejects the audio.
// That is the failure mode the PIO backend spent a long time in, and it is
// invisible without either a real sink or a test like this one.
//
// Build and run:
//   cc -O1 -I ../libdvi -o t test_hstx_island.c ../libdvi/hdmi_audio.c && ./t
//
// Verified to FAIL when lane 1 is rotated by one bit: 36 of 36 symbols
// mismatch, so it can see a real defect.
//
// Verify the HSTX transposition WITHOUT dragging in libdvi's DMA headers:
// replicate hdmi_island.c's per-lane packing exactly, then check that the
// transposition recovers the original symbols. This tests the packing math,
// which is the part I wrote and the part most likely to be wrong.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "hdmi_audio.h"

#define ISLAND_GUARD 2
#define ISLAND_PACKET 32
#define ISLAND_TOTAL (ISLAND_GUARD + ISLAND_PACKET + ISLAND_GUARD)
#define ISLAND_WORDS (ISLAND_TOTAL / 2)
#define GUARD_SYM_12 0x133
static const uint32_t ctrl[4] = {0x354, 0x0ab, 0x154, 0x2ab};

int main(void) {
    hdmi_packet_t p;
    int16_t pcm[8] = {1000,-1000,2000,-2000,3000,-3000,4000,-4000};
    hdmi_pkt_audio(&p, pcm, 4, true, 0);

    uint32_t sym[3][32];
    hdmi_encode_island(&p, true, true, true, sym);

    // --- exactly what hdmi_island.c does: build per-lane, 2 syms per word ---
    uint32_t word[3][ISLAND_WORDS];
    uint32_t guard0 = 0x2c3;   /* terc4[1|2<<1|0xc] stand-in, value irrelevant */
    for (int ch = 0; ch < 3; ch++) {
        uint32_t lane[ISLAND_TOTAL]; int i = 0;
        for (int k = 0; k < ISLAND_GUARD; k++)
            lane[i++] = (ch == 0) ? guard0 : GUARD_SYM_12;
        for (int k = 0; k < ISLAND_PACKET; k++) lane[i++] = sym[ch][k];
        for (int k = 0; k < ISLAND_GUARD; k++)
            lane[i++] = (ch == 0) ? guard0 : GUARD_SYM_12;
        for (int w = 0; w < ISLAND_WORDS; w++)
            word[ch][w] = lane[w*2] | (lane[w*2+1] << 10);
    }

    // --- my HSTX transposition ---
    uint32_t hstx[ISLAND_TOTAL];
    for (int w = 0, s = 0; w < ISLAND_WORDS; w++)
        for (int half = 0; half < 2 && s < ISLAND_TOTAL; half++, s++) {
            uint32_t a = (word[0][w] >> (half*10)) & 0x3ff;
            uint32_t b = (word[1][w] >> (half*10)) & 0x3ff;
            uint32_t c = (word[2][w] >> (half*10)) & 0x3ff;
            hstx[s] = a | (b<<10) | (c<<20);
        }

    int bad = 0;
    for (int i = 0; i < ISLAND_PACKET; i++) {
        uint32_t got = hstx[ISLAND_GUARD + i];
        uint32_t want = (sym[0][i]&0x3ff) | ((sym[1][i]&0x3ff)<<10) | ((sym[2][i]&0x3ff)<<20);
        if (got != want) { if (bad<4) printf("  sym %2d got %08x want %08x\n", i, got, want); bad++; }
    }
    // guards must survive too
    for (int g = 0; g < ISLAND_GUARD; g++) {
        uint32_t want = guard0 | (GUARD_SYM_12<<10) | ((uint32_t)GUARD_SYM_12<<20);
        if (hstx[g] != want) { printf("  head guard %d wrong\n", g); bad++; }
        if (hstx[ISLAND_GUARD+ISLAND_PACKET+g] != want) { printf("  tail guard %d wrong\n", g); bad++; }
    }
    printf("%s: %d mismatches of %d symbols\n", bad?"FAIL":"PASS", bad, ISLAND_TOTAL);
    return bad != 0;
}
