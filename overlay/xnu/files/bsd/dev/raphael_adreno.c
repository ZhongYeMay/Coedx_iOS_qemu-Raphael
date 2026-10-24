/* Redmi K20 Pro / SM8150 / Adreno 640 staged bring-up.
 *
 * raphael=1 raphael_gpu=N, default N=0 (no access):
 *  1 validate A640 firmware and construct its load plan, CPU only;
 *  2 snapshot SM8150 GPUCC, read only;
 *  3 temporarily vote clocks, ONLY if CX already powered and GMU mux inherited;
 *  4 read GMU and SMMU IDs, sample the always-on counter;
 *  5 test one ITCM word and restore it, ONLY if CM3 is already held in reset.
 * Clock votes are restored at the end and on failure. No GPU/GMU reset is
 * asserted, no GX access, no SMMU mappings, no secure calls, no DMA submission.
 * This is NOT hardware acceleration; do not publish RedwoodGPU for this device.
 * Hardware reads themselves may fault on an unvalidated boot chain: stages >=2
 * remain phone-only diagnostics, not something to enable in a daily boot image.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/redwood_input.h>
#include "raphael_a640.h"

extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern const uint8_t *redwood_image_file(const char *,const char *,uint64_t *,int *);
extern void loader_log_putc(char);

static vm_offset_t maps[4];
static const uint32_t bases[]={R640_GPUCC_PA,R640_GCC_PA,R640_GMU_PA,R640_SMMU_PA};
static const uint32_t spans[]={0x9000,0x1f0000,0x30000,0x10000};
static struct r640_clocks votes;

static void __printflike(1,2)
rlog(const char *fmt,...)
{
 char b[320]; va_list ap;va_start(ap,fmt);vsnprintf(b,sizeof(b),fmt,ap);va_end(ap);
 for(const char *p=b;*p;p++) loader_log_putc(*p);
 printf("%s",b);
}
static uint32_t __attribute__((noinline))
read32(void *ctx,unsigned bank,uint32_t reg)
{
 uint32_t value;(void)ctx;
 /* Single-register instructions also work with Qualcomm's trapped MMIO. */
 __asm__ volatile("ldr %w0, [%1]":"=r"(value):"r"(maps[bank]+reg):"memory");
 return value;
}
static void __attribute__((noinline))
write32(void *ctx,unsigned bank,uint32_t reg,uint32_t value)
{
 (void)ctx;
 __asm__ volatile("str %w0, [%1]"::"r"(value),"r"(maps[bank]+reg):"memory");
 __asm__ volatile("dsb sy":::"memory");
}
static void wait_us(void *ctx,uint32_t us)
{ (void)ctx;delay_for_interval(us,NSEC_PER_USEC); }
static struct r640_io io={NULL,read32,write32,wait_us};
static int map_bank(unsigned bank)
{
 if(!maps[bank]) maps[bank]=ml_io_map(bases[bank],spans[bank]);
 return maps[bank]!=0;
}
static int
firmware_plan(void)
{
 uint64_t size=0;int disk=-1;
 const uint8_t *fw=redwood_image_file("fw","a640_gmu.bin",&size,&disk);
 struct r640_fw_plan plan;
 if(!fw || size>0x400000) {
  rlog("raphael-gpu: fw/a640_gmu.bin missing or oversized; hardware untouched\n");return 0;
 }
 int e=r640_fw_parse(fw,(size_t)size,&plan);
 if(e) {rlog("raphael-gpu: GMU firmware rejected (%d); hardware untouched\n",e);return 0;}
 rlog("raphael-gpu: A640 GMU plan: %u segments, %u TCM bytes, %u external bytes (md%d)\n",
     plan.count,plan.tcm_bytes,plan.external_bytes,disk);
 for(uint32_t i=0;i<plan.count;i++) {
  const struct r640_segment *s=&plan.segment[i];
  rlog("raphael-gpu: segment %u: IOVA %#x, bytes %#x, region %u, file offset %#lx\n",
      i,s->address,s->size,s->region,(unsigned long)s->offset);
 }
 size=0;fw=redwood_image_file("fw","a630_sqe.fw",&size,&disk);
 /* A640 shares this SQE image with A630. First dword is the container header,
  * excluded from microcode uploads by a6xx_ucode_init(). Not a signature check. */
 if(!fw || size<8 || size>0x100000 || (size&3) || r640_le32(fw)!=0) {
  rlog("raphael-gpu: fw/a630_sqe.fw missing or incompatible container; hardware untouched\n");return 0;
 }
 rlog("raphael-gpu: SQE container: %llu bytes, payload %llu bytes; not authenticated or executed\n",size,size-4);
 rlog("raphael-gpu: DT target %#x, DTCM IOVA %#x; external memory requires SMMU mappings before firmware upload\n",
     R640_CHIP_ID,R640_DTCM_VA);
 return 1;
}
static void
snapshot_cc(void)
{
 static const uint32_t regs[]={0x106c,0x100c,0x1078,0x1098,0x109c,0x1120,0x1124};
 for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++)
  rlog("raphael-gpu: GPUCC+%#x = %#x\n",regs[i],read32(NULL,R640_GPUCC,regs[i]));
}
static uint64_t counter(void)
{
 uint32_t hi1=0,hi2=0,lo=0;
 for(unsigned tries=0;tries<8;tries++) {
  hi1=read32(NULL,R640_GMU,R640_COUNTER_HI);lo=read32(NULL,R640_GMU,R640_COUNTER_LO);
  hi2=read32(NULL,R640_GMU,R640_COUNTER_HI);if(hi1==hi2) return (uint64_t)hi1<<32|lo;
 }
 return 0;
}
static int
snapshot_gmu(void)
{
 if(!map_bank(R640_GMU) || !map_bank(R640_SMMU)) return 0;
 uint32_t reset=read32(NULL,R640_GMU,R640_GMU_RESET);
 uint32_t boot=read32(NULL,R640_GMU,R640_GMU_BOOT_RESULT);
 uint64_t first=counter();wait_us(NULL,1000);uint64_t second=counter();
 rlog("raphael-gpu: GMU reset=%#x boot_result=%#x; AO counter %llu -> %llu\n",reset,boot,first,second);
 /* No SMMU context/stream register is written or assumed to be unowned. */
 uint32_t id0=read32(NULL,R640_SMMU,0x20),id1=read32(NULL,R640_SMMU,0x24),id2=read32(NULL,R640_SMMU,0x28);
 rlog("raphael-gpu: SMMU IDR0=%#x IDR1=%#x IDR2=%#x; stream entries=%u context banks=%u\n",
      id0,id1,id2,id0&255,id1&255);
 if(!id0 || id0==0xffffffffu || !id1 || id1==0xffffffffu || !(id1&255) || second<=first) {
  rlog("raphael-gpu: GMU counter/SMMU identity check failed; no later stage\n");return 0;
 }
 return 1;
}
static int
itcm_test(void)
{
 /* Stage5 is a bus/memory diagnostic, not a firmware boot. Preserve original data.
  * Merely seeing boot_result!=BABEFACE is insufficient: require reset asserted. */
 uint32_t reset=read32(NULL,R640_GMU,R640_GMU_RESET);
 if(!(reset&1)) {rlog("raphael-gpu: CM3 not held in reset; ITCM left untouched\n");return 0;}
 uint32_t old=read32(NULL,R640_GMU,R640_ITCM_REG),test=old^0xa5c35a3cu;
 write32(NULL,R640_GMU,R640_ITCM_REG,test);
 uint32_t got=read32(NULL,R640_GMU,R640_ITCM_REG);
 write32(NULL,R640_GMU,R640_ITCM_REG,old);
 uint32_t restored=read32(NULL,R640_GMU,R640_ITCM_REG);
 rlog("raphael-gpu: ITCM test=%s restore=%s; CM3 remains in reset\n",
      got==test?"pass":"FAIL",restored==old?"pass":"FAIL");
 return got==test && restored==old;
}
void
raphael_adreno_init(void)
{
 uint32_t board=0,stage=0;int e;
 PE_parse_boot_argn("raphael",&board,sizeof(board));
 PE_parse_boot_argn("raphael_gpu",&stage,sizeof(stage));
 if(!stage || board!=1) return;
 if(stage>5) {rlog("raphael-gpu: unsupported stage %u; no access\n",stage);return;}
 rlog("raphael-gpu: experimental A640 bring-up stage %u; acceleration unavailable\n",stage);
 if(!firmware_plan()) return;
 rlog("raphael-gpu: stage 1 passed (firmware load plan only)\n");
 if(stage==1) return;
 /* Generic QEMU loader uses a different SoC peripheral base. A boot-arg alone must
  * not cause physical SM8150 accesses in a virtual machine. This guard is not a
  * hardware SoC-ID detector; a phone run still requires the raphael loader. */
 if(pe_arm_get_soc_base_phys()!=0x00400000ul) {
  rlog("raphael-gpu: phone MMIO refused: loader SoC base %#lx\n",(unsigned long)pe_arm_get_soc_base_phys());return;
 }
 if(!map_bank(R640_GPUCC)) {rlog("raphael-gpu: cannot map GPUCC\n");return;}
 snapshot_cc();rlog("raphael-gpu: stage 2 passed (read-only snapshot)\n");
 if(stage==2) return;
 if(!map_bank(R640_GCC)) {rlog("raphael-gpu: cannot map GCC\n");return;}
 e=r640_clocks_on(&io,&votes);
 if(e) {rlog("raphael-gpu: stage 3 stopped (%d): CX must be powered and GMU source inherited; cold RPMh start not implemented\n",e);return;}
 rlog("raphael-gpu: stage 3 passed (temporary clock votes; frequency not measured)\n");
 if(stage>=4) {
  if(!snapshot_gmu()) goto out;
  rlog("raphael-gpu: stage 4 passed (GMU/SMMU snapshot)\n");
 }
 if(stage>=5 && itcm_test()) rlog("raphael-gpu: stage 5 passed (ITCM round trip)\n");
out:
 e=r640_clocks_restore(&io,&votes);
 rlog("raphael-gpu: clock vote restore %s; no command processor or accelerator service started\n",e?"FAILED":"complete");
}
