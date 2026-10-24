#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "raphael_i2c.h"
struct model {
    unsigned reads, total, waits, clocks, highs;
    int sda, scl, stuck;
    unsigned char bits[64], driven[128];
};
static void sda(void *p,int release) { ((struct model *)p)->sda=release; }
static void scl(void *p,int release) {
    struct model *m=p; m->scl=release;
    if(release) { assert(m->highs<128); m->driven[m->highs++]=(unsigned char)m->sda; }
    ++m->clocks;
}
static int rsda(void *p) {
    struct model *m=p; assert(m->reads<m->total); return m->bits[m->reads++];
}
static int rscl(void *p) { return !((struct model *)p)->stuck; }
static void wait_half(void *p) { ++((struct model *)p)->waits; }
static struct raphael_i2c bus(struct model *m) {
    struct raphael_i2c b={m,sda,scl,rsda,rscl,wait_half}; return b;
}
static unsigned sent(struct model *m,unsigned first) {
    unsigned v=0; for(unsigned i=0;i<8;++i)v=v*2+m->driven[first+i]; return v;
}
int main(void)
{
    /* idle, address ACK, reg-high ACK, reg-low ACK, repeated-start idle,
       read-address ACK, eight data bits (a5), final STOP idle. */
    struct model m={.total=15,.bits={1,0,0,0,1,0,1,0,1,0,0,1,0,1,1}};
    struct raphael_i2c b=bus(&m); uint8_t v=0;
    assert(raphael_i2c_reg(&b,0x4535,&v,1,1));
    assert(v==0xa5 && m.reads==15 && m.sda && m.scl);
    assert(sent(&m,1)==0xba && sent(&m,10)==0x45 && sent(&m,19)==0x35);
    assert(sent(&m,29)==0xbb);
    assert(m.driven[46]==1); /* final received byte must be NACKed */
    m=(struct model){.total=3,.bits={1,1,1}}; b=bus(&m);
    assert(!raphael_i2c_reg(&b,0x4100,&v,1,1)); /* address NACK */
    assert(m.reads==3 && m.sda && m.scl);
    m=(struct model){.stuck=1}; b=bus(&m);
    assert(!raphael_i2c_reg(&b,0x4100,&v,1,1));
    assert(m.waits<=410 && m.sda && m.scl); /* bounded clock-stretch failure */
    assert(!raphael_i2c_reg(&b,0,&v,0,1));
    assert(!raphael_i2c_reg(&b,0,&v,257,1));
    puts("PASS: I2C address/register ordering, repeated START, byte read, NACK, STOP and timeout");
    return 0;
}
