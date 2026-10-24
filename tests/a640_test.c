#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raphael_a640.h"

static void word(unsigned char *p,unsigned v)
{for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static size_t block(unsigned char *b,unsigned a,unsigned n)
{word(b,a);word(b+4,n);word(b+8,0xefbeadde);word(b+12,0xefbeadde);memset(b+16,0x55,n);return 16+n;}
struct sink_model { unsigned tcm,ext,bytes;int fail; };
static int tcm(void *ctx,uint32_t reg,const uint8_t *p,uint32_t n)
{
 struct sink_model *m=ctx;assert(p && n && !(n&3));
 assert(r640_contains(0x3000,0x4000,reg,n)||r640_contains(0x7000,0x4000,reg,n));
 m->tcm++;m->bytes+=n;return m->fail;
}
static int external(void *ctx,uint32_t va,const uint8_t *p,uint32_t n)
{struct sink_model *m=ctx;assert(p && r640_region_for(va,n)>=R640_ICACHE);m->ext++;m->bytes+=n;return m->fail;}
static void parser_tests(void)
{
 unsigned char b[4096]={0};struct r640_fw_plan p;size_t n;
 assert(r640_fw_parse(NULL,0,&p)==R640_BAD_FW);
 n=block(b,0,4);assert(r640_fw_parse(b,n,&p)==0 && p.count==1 && p.tcm_bytes==4);
 assert(r640_fw_parse(b,n-1,&p)==R640_BAD_FW);
 assert(r640_fw_parse(b,n+1,&p)==R640_BAD_FW);
 word(b+4,0xfffffffc);assert(r640_fw_parse(b,n,&p)==R640_BAD_FW);
 n=block(b,2,4);assert(r640_fw_parse(b,n,&p)==R640_BAD_FW);
 n=block(b,0x3ffc,8);assert(r640_fw_parse(b,n,&p)==R640_FW_RANGE);
 n=block(b,0x10004000,4);assert(r640_fw_parse(b,n,&p)==R640_FW_RANGE);
 n=block(b,0xfffffffc,8);assert(r640_fw_parse(b,n,&p)==R640_FW_RANGE);
 n=block(b,0,4);n+=block(b+n,0,4);assert(r640_fw_parse(b,n,&p)==R640_FW_OVERLAP);
 n=block(b,0,8);n+=block(b+n,4,4);assert(r640_fw_parse(b,n,&p)==R640_FW_OVERLAP);
 n=block(b,0,4);n+=block(b+n,4,4);assert(r640_fw_parse(b,n,&p)==0);
 n=0;for(unsigned i=0;i<65;i++)n+=block(b+n,i*4,4);
 assert(r640_fw_parse(b,n,&p)==R640_FW_LIMIT);
 n=block(b,0,4);word(b+8,1);assert(r640_fw_parse(b,n,&p)==R640_BAD_FW);
 n=block(b,0,0);assert(r640_fw_parse(b,n,&p)==R640_BAD_FW);
 n=block(b,0,4);n+=block(b+n,0x40000,4);n+=block(b+n,0x57000,4);
 assert(r640_fw_parse(b,n,&p)==0 && p.tcm_bytes==8 && p.external_bytes==4);
 struct sink_model m={0};struct r640_fw_sink s={&m,tcm,NULL};
 assert(r640_fw_copy(b,n,&s)==R640_STATE && m.tcm==0); /* no partial upload without an IOVA sink */
 s.write_iova=external;assert(r640_fw_copy(b,n,&s)==0 && m.tcm==2 && m.ext==1 && m.bytes==12);
 m=(struct sink_model){.fail=-91};assert(r640_fw_copy(b,n,&s)==-91 && m.tcm==1 && m.ext==0);
 m=(struct sink_model){0};word(b+n-4,0);word(b+n-16,0xfffffffc);
 assert(r640_fw_copy(b,n,&s)!=0 && m.tcm==0); /* validate ALL segments before the first write */
 puts("PASS: firmware truncation, overflow, regions, overlap, record limits, validation before upload, sink failures");
}
struct cell {unsigned bank;uint32_t reg,v;};
struct model {struct cell cells[64];unsigned n,writes,reads,waits;int hang,deny;};
static struct cell *cell(struct model *m,unsigned b,uint32_t r)
{
 for(unsigned i=0;i<m->n;i++)if(m->cells[i].bank==b && m->cells[i].reg==r)return &m->cells[i];
 assert(m->n<64);struct cell *c=&m->cells[m->n++];*c=(struct cell){b,r,0};return c;
}
static uint32_t readreg(void *ctx,unsigned b,uint32_t r)
{struct model *m=ctx;m->reads++;return cell(m,b,r)->v;}
static void writereg(void *ctx,unsigned b,uint32_t r,uint32_t v)
{
 struct model *m=ctx;m->writes++;
 if(m->deny && b==R640_GPUCC && r==0x1098 && (v&1))return;
 if(m->hang && b==R640_GPUCC && r==0x1098 && (v&1))v|=1u<<31;
 cell(m,b,r)->v=v;
}
static void waitreg(void *ctx,uint32_t us)
{struct model *m=ctx;assert(us==10);m->waits++;}
static void initialize(struct model *m)
{
 memset(m,0,sizeof(*m));cell(m,R640_GPUCC,R640_CX_GDSCR)->v=R640_GDSC_ON;
 cell(m,R640_GPUCC,0x1124)->v=0x602;cell(m,R640_GCC,0x52004)->v=0x55;
}
static void clocks_tests(void)
{
 struct model m;initialize(&m);struct r640_io io={&m,readreg,writereg,waitreg};struct r640_clocks c={0};
 cell(&m,R640_GPUCC,R640_CX_GDSCR)->v=0;
 assert(r640_clocks_on(&io,&c)==R640_POWER_OFF && m.writes==0);
 cell(&m,R640_GPUCC,R640_CX_GDSCR)->v=R640_GDSC_ON|R640_SW_COLLAPSE;
 assert(r640_clocks_on(&io,&c)==R640_POWER_OFF && m.writes==0);
 initialize(&m);cell(&m,R640_GPUCC,0x1124)->v=0x302;
 assert(r640_clocks_on(&io,&c)==R640_CLOCK_SOURCE && m.writes==0);
 initialize(&m);cell(&m,R640_GPUCC,0x1120)->v=1;
 assert(r640_clocks_on(&io,&c)==R640_CLOCK_SOURCE && m.writes==0);
 initialize(&m);assert(r640_clocks_on(&io,&c)==0 && c.count==7);
 assert(r640_clocks_on(&io,&c)==R640_STATE);
 /* A concurrent owner's unrelated vote survives cleanup. */
 cell(&m,R640_GCC,0x52004)->v|=1u<<15;
 assert(r640_clocks_restore(&io,&c)==0 && c.count==0);
 assert(cell(&m,R640_GCC,0x52004)->v==(0x55|(1u<<15)));
 assert(cell(&m,R640_GPUCC,0x1098)->v==0);
 initialize(&m);m.hang=1;
 assert(r640_clocks_on(&io,&c)==R640_TIMEOUT && c.count==0 && m.waits>=1000 && m.waits<1100);
 assert((cell(&m,R640_GCC,0x52004)->v&(1u<<16))==0);
 assert((cell(&m,R640_GPUCC,0x1098)->v&1)==0);
 initialize(&m);m.deny=1;assert(r640_clocks_on(&io,&c)==R640_VERIFY && c.count==0);
 /* Inherited votes must not be revoked at cleanup. */
 initialize(&m);cell(&m,R640_GPUCC,0x1098)->v=1;
 assert(r640_clocks_on(&io,&c)==0 && c.count==6);
 assert(r640_clocks_restore(&io,&c)==0 && cell(&m,R640_GPUCC,0x1098)->v==1);
 assert(r640_clocks_restore(&io,&c)==0);
 puts("PASS: power/source guards, bounded clock timeout, failed writes, reverse cleanup, unrelated/inherited votes");
}
static void real_firmware(const char *path)
{
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);
 assert(len>0 && len<0x400000);unsigned char *b=malloc((size_t)len);assert(b);
 assert(fread(b,1,(size_t)len,f)==(size_t)len);fclose(f);
 struct r640_fw_plan p;assert(r640_fw_parse(b,(size_t)len,&p)==0);
 struct sink_model m={0};struct r640_fw_sink s={&m,tcm,external};
 assert(r640_fw_copy(b,(size_t)len,&s)==0 && m.bytes==p.tcm_bytes+p.external_bytes);
 printf("PASS: real A640 firmware: %ld bytes, %u segments, TCM=%u external=%u, copied=%u bytes (mock sinks)\n",len,p.count,p.tcm_bytes,p.external_bytes,m.bytes);
 /* Every single truncation must be either rejected or a complete prefix with valid blocks.
  * Random byte mutations exercise all bounds; successful parses may still contain changed code,
  * since this parser is intentionally not an authenticity or instruction validator. */
 for(size_t i=0;i<(size_t)len;i++) (void)r640_fw_parse(b,i,&p);
 uint32_t rnd=0x640;
 for(unsigned i=0;i<10000;i++) {
  rnd=rnd*1664525u+1013904223u;size_t at=rnd%(size_t)len;uint8_t old=b[at];b[at]^=(uint8_t)(1u<<(rnd>>29));
  (void)r640_fw_parse(b,(size_t)len,&p);b[at]=old;
 }
 free(b);puts("PASS: exhaustive firmware truncations and 10000 deterministic byte mutations under sanitizers");
}
int main(int argc,char **argv)
{parser_tests();clocks_tests();if(argc>1)real_firmware(argv[1]);return 0;}
