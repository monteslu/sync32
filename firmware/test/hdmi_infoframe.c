#include <stdio.h>
#include <string.h>
#include "hdmi_audio.h"
/* Validate InfoFrame contents against CEA-861: checksum must make the sum of
   header+data zero, and the fields must say what we intend. */
static int check(const char *name, hdmi_packet_t *p, int datalen) {
    unsigned sum = p->header[0] + p->header[1] + p->header[2];
    /* data bytes live in sub[0] then sub[1] */
    unsigned char d[32]; int n = 0;
    for (int s = 0; s < 4 && n < datalen; s++)
        for (int i = 0; i < 7 && n < datalen; i++) d[n++] = p->sub[s][i];
    for (int i = 0; i < datalen; i++) sum += d[i];
    int ok = ((sum & 0xff) == 0);
    printf("%s: hdr=%02x %02x %02x  checksum byte=%02x  sum&0xff=%02x  %s\n",
           name, p->header[0], p->header[1], p->header[2], d[0], sum & 0xff,
           ok ? "VALID" : "BAD CHECKSUM");
    return ok;
}
int main(void) {
    hdmi_packet_t avi, ai, acr;
    hdmi_pkt_avi_infoframe(&avi);
    hdmi_pkt_audio_infoframe(&ai);
    hdmi_pkt_acr(&acr, 6144, 25200);
    int ok = 1;
    ok &= check("AVI  ", &avi, 14);
    ok &= check("Audio", &ai, 6);
    /* AVI specifics */
    printf("  AVI: version=%d length=%d VIC=%d (1=640x480p60)\n",
           avi.header[1], avi.header[2], avi.sub[0][4]);
    /* Audio InfoFrame specifics */
    printf("  AudioInfo: version=%d length=%d CC=%d CT=%d\n",
           ai.header[1], ai.header[2], ai.sub[0][1] & 7, (ai.sub[0][1] >> 4) & 0xf);
    /* ACR: N and CTS must come back out intact */
    unsigned cts = ((acr.sub[0][1] & 0x0f) << 16) | (acr.sub[0][2] << 8) | acr.sub[0][3];
    unsigned N   = ((acr.sub[0][4] & 0x0f) << 16) | (acr.sub[0][5] << 8) | acr.sub[0][6];
    printf("  ACR: N=%u CTS=%u  (want N=6144 CTS=25200)  %s\n",
           N, cts, (N == 6144 && cts == 25200) ? "OK" : "WRONG");
    if (N != 6144 || cts != 25200) ok = 0;
    /* the 128*fs/1000 rule: N=6144 for 48kHz is the spec's exact value */
    printf("\nINFOFRAMES: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
