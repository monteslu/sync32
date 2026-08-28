#include <stdio.h>
/* Replicates the scheduler in video.c exactly and counts the packet mix over
   one second of islands, so the spec minimums are checked without needing to
   catch a rare packet on the wire. */
int main(void) {
    const int ISLANDS_PER_SEC = 14400;      /* active-line ceiling */
    unsigned ctl_ctr = 0;
    int acr = 0, avi = 0, ainfo = 0, audio = 0;
    for (int i = 0; i < ISLANDS_PER_SEC; i++) {
        if ((ctl_ctr & 0x1f) == 0) {
            unsigned slot = (ctl_ctr >> 5) & 15;
            if (slot == 0 || slot == 8) avi++;
            else if (slot == 4)         ainfo++;
            else                        acr++;
            ctl_ctr++;
        } else {
            audio++;
            ctl_ctr++;
        }
    }
    printf("over %d islands (1 second):\n", ISLANDS_PER_SEC);
    printf("  ACR             %4d/s   need >=300  %s\n", acr,   acr   >= 300 ? "OK" : "FAIL");
    printf("  AVI InfoFrame   %4d/s   need >= 30  %s\n", avi,   avi   >=  30 ? "OK" : "FAIL");
    printf("  Audio InfoFrame %4d/s\n", ainfo);
    printf("  audio packets   %4d/s -> %d Hz capacity  need 48000  %s\n",
           audio, audio * 4, audio * 4 >= 48000 ? "OK" : "FAIL");
    int pass = acr >= 300 && avi >= 30 && audio * 4 >= 48000;
    printf("\nCADENCE: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
