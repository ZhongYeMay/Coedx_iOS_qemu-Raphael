/* SM8150 / A640 bring-up primitives, shared by XNU and the host fault-injection tests.
 * Register facts: Linux v6.12 gpucc-sm8150.c, gcc-sm8150.c, a6xx_gmu.xml.
 * No GPU virtual address in a firmware file is a CPU physical address.
 */
#ifndef RAPHAEL_A640_H
#define RAPHAEL_A640_H
#include <stdint.h>
#include <stddef.h>

#define R640_GPU_PA 0x02c00000u
#define R640_GMU_PA 0x02c6a000u
#define R640_GPUCC_PA 0x02c90000u
#define R640_SMMU_PA 0x02ca0000u
#define R640_GCC_PA 0x00100000u
#define R640_CHIP_ID 0x06040001u /* DT identity, NOT a register read */
#define R640_TCM_SIZE 0x4000u
#define R640_DTCM_VA 0x00040000u /* A640; A650-family's 0x10004000 is WRONG here */
#define R640_ITCM_REG 0x3000u
#define R640_DTCM_REG 0x7000u
#define R640_CX_GDSCR 0x106cu
#define R640_GDSC_ON (1u << 31)
#define R640_SW_COLLAPSE 1u
#define R640_GMU_RESET 0x14000u
#define R640_GMU_BOOT_RESULT 0x14070u
#define R640_COUNTER_LO 0x14220u
#define R640_COUNTER_HI 0x14224u
#define R640_MAX_SEGMENTS 64u
#define R640_JOURNAL_SIZE 12u

enum r640_error {
 R640_OK=0, R640_BAD_FW=-1, R640_FW_RANGE=-2, R640_FW_OVERLAP=-3,
 R640_FW_LIMIT=-4, R640_POWER_OFF=-5, R640_CLOCK_SOURCE=-6,
 R640_TIMEOUT=-7, R640_STATE=-8, R640_VERIFY=-9
};
enum r640_region { R640_ITCM, R640_DTCM, R640_ICACHE, R640_DCACHE, R640_DUMMY };
struct r640_segment { uint32_t address,size,region; size_t offset; };
struct r640_fw_plan {
 struct r640_segment segment[R640_MAX_SEGMENTS];
 uint32_t count, tcm_bytes, external_bytes, metadata;
};
static inline uint32_t r640_le32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static inline int r640_contains(uint32_t base,uint32_t length,uint32_t a,uint32_t n)
{ return a>=base && n<=length && a-base<=length-n; }
static inline int r640_region_for(uint32_t a,uint32_t n)
{
 if(r640_contains(0,0x4000,a,n)) return R640_ITCM;
 if(r640_contains(0x40000,0x4000,a,n)) return R640_DTCM;
 if(r640_contains(0x4000,0x3c000,a,n)) return R640_ICACHE;
 if(r640_contains(0x44000,0x3c000,a,n)) return R640_DCACHE;
 if(r640_contains(0x60000000,0x1000,a,n)) return R640_DUMMY;
 return -1;
}
/* Parse everything before a caller may copy anything. A failed plan must not be used.
 * Zero-sized preallocation records are not understood by this A640 implementation:
 * reject them rather than silently treating newer A660 firmware as A640 firmware.
 */
static inline int r640_fw_parse(const uint8_t *data,size_t length,struct r640_fw_plan *p)
{
 size_t at=0; p->count=p->tcm_bytes=p->external_bytes=p->metadata=0;
 if(!data || !length || length>0x400000) return R640_BAD_FW;
 while(at<length) {
  uint32_t a,n,type; int region;
  if(length-at<16) return R640_BAD_FW;
  a=r640_le32(data+at); n=r640_le32(data+at+4);type=r640_le32(data+at+8);at+=16;
  if(!n || (a&3) || (n&3) || n>length-at) return R640_BAD_FW;
  /* Public A640 blocks use this marker in both otherwise-unused header words. */
  if(type!=0xefbeaddeu || r640_le32(data+at-4)!=0xefbeaddeu) return R640_BAD_FW;
  region=r640_region_for(a,n); if(region<0) return R640_FW_RANGE;
  if(p->count==R640_MAX_SEGMENTS) return R640_FW_LIMIT;
  for(uint32_t i=0;i<p->count;i++) {
   const struct r640_segment *s=&p->segment[i];
   if((uint64_t)a<(uint64_t)s->address+s->size && (uint64_t)s->address<(uint64_t)a+n)
    return R640_FW_OVERLAP;
  }
  p->segment[p->count++]=(struct r640_segment){a,n,(uint32_t)region,at};
  if(region==R640_ITCM || region==R640_DTCM) p->tcm_bytes+=n; else p->external_bytes+=n;
  at+=n;
 }
 return p->tcm_bytes ? R640_OK : R640_BAD_FW;
}
/* Writes to the three address spaces use different callbacks. There is deliberately no
 * implementation that writes an IOVA such as 0x57000 through ml_io_map(). */
struct r640_fw_sink {
 void *ctx;
 int (*write_tcm)(void *,uint32_t reg,const uint8_t *,uint32_t);
 int (*write_iova)(void *,uint32_t iova,const uint8_t *,uint32_t);
};
static inline int r640_fw_copy(const uint8_t *data,size_t length,struct r640_fw_sink *sink)
{
 struct r640_fw_plan p; int e=r640_fw_parse(data,length,&p);
 if(e) return e;
 if(!sink || !sink->write_tcm || (p.external_bytes && !sink->write_iova)) return R640_STATE;
 for(uint32_t i=0;i<p.count;i++) {
  const struct r640_segment *s=&p.segment[i];
  if(s->region==R640_ITCM || s->region==R640_DTCM) {
   uint32_t reg=s->region==R640_ITCM ? R640_ITCM_REG+s->address : R640_DTCM_REG+s->address-R640_DTCM_VA;
   e=sink->write_tcm(sink->ctx,reg,data+s->offset,s->size);
  } else e=sink->write_iova(sink->ctx,s->address,data+s->offset,s->size);
  if(e) return e;
 }
 return R640_OK;
}

enum r640_bank { R640_GPUCC, R640_GCC, R640_GMU, R640_SMMU };
struct r640_io {
 void *ctx;
 uint32_t (*read)(void *,unsigned bank,uint32_t reg);
 void (*write)(void *,unsigned bank,uint32_t reg,uint32_t value);
 void (*wait_us)(void *,uint32_t us);
};
struct r640_saved { uint32_t reg,mask,value; unsigned bank; };
struct r640_clocks { struct r640_saved saved[R640_JOURNAL_SIZE]; unsigned count; };
static inline int r640_poll(struct r640_io *io,unsigned bank,uint32_t reg,uint32_t mask,uint32_t want)
{
 for(unsigned i=0;i<1000;i++) {
  if((io->read(io->ctx,bank,reg)&mask)==want) return R640_OK;
  io->wait_us(io->ctx,10);
 }
 return R640_TIMEOUT;
}
/* Only restore bits owned by this transaction. Do not overwrite HWCG/status bits. */
static inline int r640_clocks_restore(struct r640_io *io,struct r640_clocks *c)
{
 int result=0;
 while(c->count) {
  const struct r640_saved *s=&c->saved[--c->count];
  uint32_t now=io->read(io->ctx,s->bank,s->reg);
  io->write(io->ctx,s->bank,s->reg,(now&~s->mask)|s->value);
  if((io->read(io->ctx,s->bank,s->reg)&s->mask)!=s->value) result=R640_VERIFY;
 }
 return result;
}
static inline int r640_vote(struct r640_io *io,struct r640_clocks *c,unsigned bank,uint32_t reg,uint32_t mask,int poll)
{
 uint32_t old=io->read(io->ctx,bank,reg);
 if((old&mask)!=mask) {
  if(c->count==R640_JOURNAL_SIZE) return R640_STATE;
  c->saved[c->count++]=(struct r640_saved){reg,mask,old&mask,bank};
  io->write(io->ctx,bank,reg,old|mask);
  if((io->read(io->ctx,bank,reg)&mask)!=mask) return R640_VERIFY;
 }
 /* Voted and AHB branches use a delay as in clk-branch.c; a status bit may reflect
  * other voters. Ordinary branches must report running (CBCR bit31 clear). */
 if(!poll) {io->wait_us(io->ctx,10);return R640_OK;}
 return r640_poll(io,bank,reg,1u<<31,0);
}
/* Bring-up while firmware has left CX powered. Cold RPMh/rail control is intentionally
 * not guessed. Do not reset a running GMU or reprogram a PLL shared with other blocks.
 * The GMU source must already select GPLL0_DIV / 1.5 (200MHz at the usual 600MHz GPLL0).
 * This verifies the mux/divider, NOT a measured frequency or a voltage vote.
 */
static inline int r640_clocks_on(struct r640_io *io,struct r640_clocks *c)
{
 uint32_t power,cfg; int e;
 if(c->count) return R640_STATE;
 power=io->read(io->ctx,R640_GPUCC,R640_CX_GDSCR);
 if(!(power&R640_GDSC_ON) || (power&R640_SW_COLLAPSE)) return R640_POWER_OFF;
 if(io->read(io->ctx,R640_GPUCC,0x1120)&1) return R640_CLOCK_SOURCE;
 cfg=io->read(io->ctx,R640_GPUCC,0x1124);
 if((cfg&0x71f)!=0x602) return R640_CLOCK_SOURCE;
 /* SM8150 GCC votes, then GPUCC AHB/CXO/GMU. No GX register is accessed. */
 static const struct {unsigned b;uint32_t r,m;int poll;} v[]={
  {R640_GCC,0x52004,1u<<16,0},
  {R640_GCC,0x71154,1,0},{R640_GCC,0x7100c,1,0},{R640_GCC,0x71018,1,1},
  {R640_GPUCC,0x1078,1,0},{R640_GPUCC,0x109c,1,1},{R640_GPUCC,0x1098,1,1}
 };
 for(unsigned i=0;i<sizeof(v)/sizeof(v[0]);i++) {
  e=r640_vote(io,c,v[i].b,v[i].r,v[i].m,v[i].poll);
  if(e) {int undo=r640_clocks_restore(io,c);return undo?undo:e;}
 }
 return R640_OK;
}
#endif
