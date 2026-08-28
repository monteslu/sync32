#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "hdmi_audio.h"

/* Reference construction, transcribed from the behaviour described in
   rh1tech/frank-hdmi-audio set_audio_sample() (GPL -- reimplemented here as a
   test oracle, not copied into the firmware). */
static int par8(uint8_t x){ int p=0; while(x){p^=x&1;x>>=1;} return p; }

static void ref_audio(uint8_t hdr[3], uint8_t sub[4][7],
                      const int16_t *lr, int n, int frameCt) {
    int layout = 0, samplePresent = (1<<n)-1;
    int B = (frameCt < n) ? (1 << frameCt) : 0;
    hdr[0] = 2;
    hdr[1] = (uint8_t)((layout<<4) | samplePresent);
    hdr[2] = (uint8_t)(B << 4);
    memset(sub, 0, 4*7);
    for (int i = 0; i < n; i++) {
        int16_t l = lr[i*2], r = lr[i*2+1];
        uint8_t vuc = 1;
        uint8_t *d = sub[i];
        d[0]=0; d[1]=(uint8_t)l; d[2]=(uint8_t)(l>>8);
        d[3]=0; d[4]=(uint8_t)r; d[5]=(uint8_t)(r>>8);
        int pl = par8(d[1]) ^ par8(d[2]) ^ par8(vuc);
        int pr = par8(d[4]) ^ par8(d[5]) ^ par8(vuc);
        d[6] = (uint8_t)((vuc<<0) | (pl<<3) | (vuc<<4) | (pr<<7));
    }
}

int main(void) {
    int16_t pcm[8] = { 11173, -4001, 12287, 22222, -13797, 500, 13711, -32000 };
    for (int n = 1; n <= 4; n++) {
        hdmi_packet_t mine;
        hdmi_pkt_audio(&mine, pcm, n, false, 0);
        uint8_t rh[3], rs[4][7];
        ref_audio(rh, rs, pcm, n, 100 /* not a block start */);

        int hdiff = memcmp(mine.header, rh, 3);
        printf("n=%d header mine=%02x %02x %02x  ref=%02x %02x %02x  %s\n", n,
               mine.header[0],mine.header[1],mine.header[2],
               rh[0],rh[1],rh[2], hdiff? "DIFFER":"same");
        for (int i = 0; i < n; i++) {
            if (memcmp(mine.sub[i], rs[i], 7)) {
                printf("   sub%d mine:", i);
                for (int k=0;k<7;k++) printf(" %02x", mine.sub[i][k]);
                printf("\n   sub%d ref :", i);
                for (int k=0;k<7;k++) printf(" %02x", rs[i][k]);
                printf("\n");
            }
        }
    }
    return 0;
}
