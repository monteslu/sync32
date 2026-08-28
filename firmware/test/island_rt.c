#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "hdmi_audio.h"

/* Decoder written from the REFERENCE spec description (hdl-util/hdmi
   packet_assembler line 35 + hdmi.sv lines 335-338), independently of the
   encoder, so agreement is real evidence and not a tautology. */
static int rev_terc4(uint32_t sym) {
    for (int i = 0; i < 16; i++) if (hdmi_terc4[i] == sym) return i;
    return -1;
}
int main(void) {
    hdmi_packet_t p;
    int16_t pcm[8] = { 11173, -4001, 12287, 22222, -13797, 500, 13711, -32000 };
    hdmi_pkt_audio(&p, pcm, 4, true, 0);

    uint32_t out[3][32];
    hdmi_encode_island(&p, true, false, true, out);

    uint8_t hdr[4] = {0}, sub[4][8]; memset(sub, 0, sizeof sub);
    bool bad_sym = false, hsync_ok = true, vsync_ok = true;
    for (int px = 0; px < 32; px++) {
        int n0 = rev_terc4(out[0][px]);
        int n1 = rev_terc4(out[1][px]);
        int n2 = rev_terc4(out[2][px]);
        if (n0 < 0 || n1 < 0 || n2 < 0) { bad_sym = true; break; }
        if ((n0 & 1) != 1) hsync_ok = false;          /* hsync=true */
        if (((n0 >> 1) & 1) != 0) vsync_ok = false;   /* vsync=false */
        hdr[px >> 3] |= (uint8_t)(((n0 >> 3) & 1) << (px & 7));
        for (int s = 0; s < 4; s++) {
            int b1 = (n1 >> s) & 1, b2 = (n2 >> s) & 1;
            int i1 = px * 2, i2 = px * 2 + 1;
            sub[s][i1 >> 3] |= (uint8_t)(b1 << (i1 & 7));
            sub[s][i2 >> 3] |= (uint8_t)(b2 << (i2 & 7));
        }
    }
    printf("all symbols legal TERC4 : %s\n", bad_sym ? "NO" : "yes");
    printf("hsync/vsync carried     : %s / %s\n", hsync_ok?"yes":"NO", vsync_ok?"yes":"NO");
    printf("header  %02x %02x %02x ecc=%02x  (sent %02x %02x %02x ecc=%02x) %s\n",
           hdr[0],hdr[1],hdr[2],hdr[3],
           p.header[0],p.header[1],p.header[2], hdmi_ecc_header(p.header),
           (memcmp(hdr,p.header,3)==0 && hdr[3]==hdmi_ecc_header(p.header)) ? "MATCH":"MISMATCH");
    printf("header ECC verifies     : %s\n",
           hdmi_ecc_header(hdr)==hdr[3] ? "yes":"NO");

    int fails = 0;
    for (int s = 0; s < 4; s++) {
        int datamatch = memcmp(sub[s], p.sub[s], 7) == 0;
        int eccok = hdmi_ecc_sub(sub[s]) == sub[s][7];
        if (!datamatch || !eccok) fails++;
        printf("sub%d data=%s ecc=%s\n", s, datamatch?"match":"MISMATCH", eccok?"ok":"BAD");
    }
    /* recover PCM the way a sink would */
    printf("\nrecovered PCM:\n");
    int pcm_ok = 1;
    for (int i = 0; i < 4; i++) {
        uint32_t lw = sub[i][0] | (sub[i][1]<<8) | (sub[i][2]<<16);
        uint32_t rw = sub[i][3] | (sub[i][4]<<8) | (sub[i][5]<<16);
        int16_t L = (int16_t)((lw >> 8) & 0xffff), R = (int16_t)((rw >> 8) & 0xffff);
        int ok = (L == pcm[i*2] && R == pcm[i*2+1]);
        if (!ok) pcm_ok = 0;
        printf("  frame%d L=%6d R=%6d  (sent %6d %6d) %s\n",
               i, L, R, pcm[i*2], pcm[i*2+1], ok?"ok":"MISMATCH");
    }
    int pass = !bad_sym && hsync_ok && vsync_ok && fails==0 && pcm_ok &&
               memcmp(hdr,p.header,3)==0;
    printf("\nROUND TRIP: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
