#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "raphael_gt9886.h"

static void seal(uint8_t *d, unsigned n)
{
    uint8_t sum = 0;
    d[n-1] = 0;
    for (unsigned i=0; i<n-1; ++i) sum = (uint8_t)(sum+d[i]);
    d[n-1] = (uint8_t)(0-sum);
}
int main(void)
{
    uint8_t d[RAPHAEL_REPORT_MAX] = {0x80,1, 0,0x37,4,0x23,9,9,0,0,0,0};
    struct raphael_frame f = {0}, keep;
    seal(d,12);
    assert(raphael_decode(d,12,&f)==1);
    assert(f.mask==1 && f.contact[0].x==1079 && f.contact[0].y==2339);
    assert(raphael_axis(f.contact[0].x,1080)==65535);
    assert(raphael_axis(65535,2340)==65535 && raphael_axis(0,2340)==0);
    keep=f; d[5]^=1;
    assert(raphael_decode(d,12,&f)==-1 && !memcmp(&f,&keep,sizeof(f)));
    d[5]^=1;
    assert(raphael_decode(d,11,&f)==-1);
    d[0]=0x40;
    assert(raphael_decode(d,12,&f)==0 && f.mask==keep.mask);
    d[0]=0x80; d[1]=11;
    assert(raphael_decode(d,sizeof(d),&f)==-1);
    d[1]=1; d[2]=10; seal(d,12);
    assert(raphael_decode(d,12,&f)==-1);
    memset(d,0,sizeof(d)); d[0]=0x80; d[1]=10;
    for(unsigned i=0;i<10;++i) d[2+i*8]=(uint8_t)i;
    seal(d,sizeof(d));
    assert(raphael_decode(d,sizeof(d),&f)==1 && f.mask==0x3ff);
    d[2+8]=0; seal(d,sizeof(d));
    assert(raphael_decode(d,sizeof(d),&f)==-1 && f.mask==0x3ff);
    /* Empty release packets follow the reference driver's no-checksum path. */
    d[0]=0x80; d[1]=0;
    assert(raphael_decode(d,4,&f)==1 && f.mask==0);
    assert(raphael_decode(NULL,0,&f)==-1);
    puts("PASS: GT9886 checksum, malformed frames, IDs, 10 contacts, release and scaling");
    return 0;
}
