#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "hdmi_audio.h"
static uint8_t bitat(const uint8_t*b,int i){return (b[i>>3]>>(i&7))&1;}
/* the ORIGINAL per-pixel algorithm, kept here as the oracle */
static void old_encode(const hdmi_packet_t*p,int hs,int vs,int fp,uint32_t o[3][32]){
    uint8_t hdr[4],sub[4][8];
    memcpy(hdr,p->header,3); hdr[3]=hdmi_ecc_header(p->header);
    for(int i=0;i<4;i++){memcpy(sub[i],p->sub[i],7);sub[i][7]=hdmi_ecc_sub(p->sub[i]);}
    for(int px=0;px<32;px++){
        uint8_t st=(px==0&&fp)?0:1;
        uint8_t c0=(hs?1:0)|((vs?1:0)<<1)|(st<<2)|(bitat(hdr,px)<<3);
        o[0][px]=hdmi_terc4[c0];
        uint8_t n1=0,n2=0;
        for(int s=0;s<4;s++){n1|=bitat(sub[s],px*2)<<s;n2|=bitat(sub[s],px*2+1)<<s;}
        o[1][px]=hdmi_terc4[n1]; o[2][px]=hdmi_terc4[n2];
    }
}
int main(void){
    srand(1234);
    int bad=0;
    for(int t=0;t<20000;t++){
        hdmi_packet_t p; 
        for(int i=0;i<3;i++) p.header[i]=rand();
        for(int i=0;i<4;i++) for(int k=0;k<7;k++) p.sub[i][k]=rand();
        int hs=rand()&1, vs=rand()&1, fp=rand()&1;
        uint32_t a[3][32],b[3][32];
        old_encode(&p,hs,vs,fp,a);
        hdmi_encode_island(&p,hs,vs,fp,b);
        if(memcmp(a,b,sizeof a)){ bad++; if(bad==1) printf("first mismatch at trial %d\n",t); }
    }
    printf("random packets compared: 20000, mismatches: %d\n", bad);
    printf("EQUIVALENCE: %s\n", bad?"FAIL":"PASS");
    return bad?1:0;
}
