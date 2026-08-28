#include <stdio.h>
#include <string.h>
#include "hdmi_audio.h"
/* The island lives in the back porch, between control runs that carry
   get_ctrl_sym(!v_sync_polarity, !h_sync_polarity) on an active scanline.
   640x480p60 is negative-sync on both, so those runs carry hsync=1, vsync=1.
   The island's ch0 nibble must carry the SAME levels, or sync inverts for the
   44 clocks of the island on every scanline. */
static const uint32_t dvi_ctrl_syms[4] = {0xd5354, 0x2acab, 0x55154, 0xaaeab};
int main(void) {
    const int h_sync_polarity = 0, v_sync_polarity = 0;   /* 640x480p60 */
    int want_h = !h_sync_polarity, want_v = !v_sync_polarity;

    /* what the blanking control run actually emits (low 10 bits of the pair) */
    uint32_t blank = dvi_ctrl_syms[(!!want_v << 1) | !!want_h] & 0x3ff;

    hdmi_packet_t p; int16_t pcm[8] = {0};
    hdmi_pkt_audio(&p, pcm, 4, true, 0);
    uint32_t out[3][32];
    hdmi_encode_island(&p, want_h, want_v, true, out);

    int bad = 0;
    for (int px = 0; px < 32; px++) {
        int n = -1;
        for (int i = 0; i < 16; i++) if (hdmi_terc4[i] == out[0][px]) n = i;
        if (n < 0) { printf("px%d: not a TERC4 symbol\n", px); bad = 1; break; }
        int h = n & 1, v = (n >> 1) & 1;
        if (h != want_h || v != want_v) {
            printf("px%2d: island hsync=%d vsync=%d, blanking wants %d/%d MISMATCH\n",
                   px, h, v, want_h, want_v);
            bad = 1; break;
        }
    }
    printf("blanking control symbol : 0x%03x (vsync=%d hsync=%d)\n",
           blank, want_v, want_h);
    printf("island ch0 sync bits    : %s\n", bad ? "MISMATCH" : "match on all 32 pixels");
    printf("\nSYNC CONTINUITY: %s\n", bad ? "FAIL" : "PASS");
    return bad;
}
