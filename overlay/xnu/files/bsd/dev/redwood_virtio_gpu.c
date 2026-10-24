/* QEMU virtio-mmio GPU 2D driver for XNU.
 * Boot arg virtio_gpu=1, QEMU -device virtio-gpu-device,id=vgpu.
 * This is a scanout/transfer driver, NOT a Metal, OpenGL or virgl renderer.
 * It registers the boot framebuffer as scatter/gather backing without another
 * guest-side pixel copy. CoreAnimation still composes in software.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include "redwood_virtio_gpu.h"
extern vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern vm_offset_t pe_arm_get_soc_base_phys(void);

#define R_MAGIC 0x000
#define R_VERSION 0x004
#define R_ID 0x008
#define R_FEATURES 0x010
#define R_FEATURES_SEL 0x014
#define R_DRV_FEATURES 0x020
#define R_DRV_FEATURES_SEL 0x024
#define R_QSEL 0x030
#define R_QMAX 0x034
#define R_QNUM 0x038
#define R_QREADY 0x044
#define R_NOTIFY 0x050
#define R_INTR 0x060
#define R_ACK 0x064
#define R_STATUS 0x070
#define R_QDESC 0x080
#define R_QAVAIL 0x090
#define R_QUSED 0x0a0
#define USED_OFFSET 0x800
#define AVAIL_OFFSET 0x400
#define RESOURCE 1u
struct descriptor {uint64_t addr;uint32_t len;uint16_t flags,next;};
struct available {uint16_t flags,idx,ring[RWVG_QSIZE];};
struct used {uint16_t flags,idx;struct {uint32_t id,len;} ring[RWVG_QSIZE];};
static struct {
 vm_offset_t regs,queue,request,response;
 struct descriptor *desc;struct available *avail;volatile struct used *used;
 uint16_t seen;uint64_t fence,frames;
 uint32_t width,height;uintptr_t pixels;bool ready,attempted;
} gpu;
static LCK_GRP_DECLARE(vgpu_group,"virtio-gpu");
static LCK_MTX_DECLARE(vgpu_lock,&vgpu_group);
static uint32_t rr(uint32_t r){return *(volatile uint32_t *)(gpu.regs+r);}
static void wr(uint32_t r,uint32_t v){*(volatile uint32_t *)(gpu.regs+r)=v;}
static void wr64(uint32_t r,uint64_t v){wr(r,(uint32_t)v);wr(r+4,(uint32_t)(v>>32));}
static void barrier(void){__asm__ volatile("dsb sy":::"memory");}
static int alloc_page(vm_offset_t *p)
{return kmem_alloc(kernel_map,p,PAGE_SIZE,KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)==KERN_SUCCESS;}
static uint64_t physical(void *unused,uintptr_t va){(void)unused;return kvtophys((vm_offset_t)va);}
static bool reset_device(void)
{
 wr(R_STATUS,0);barrier();
 for(unsigned i=0;i<1000;i++){if(rr(R_STATUS)==0)return true;delay(10);}
 printf("virtio-gpu: reset timeout; DMA pages remain pinned\n");return false;
}
static void fail(const char *what)
{
 printf("virtio-gpu: %s; virtual scanout disabled, boot framebuffer retained\n",what);
 gpu.ready=false;
 /* No freed DMA buffer is reused after a timeout. Reset stops outstanding access;
  * allocations and the boot framebuffer remain valid for the life of the kernel. */
 if(gpu.regs)reset_device();
}
/* One outstanding request, protected by vgpu_lock. Both descriptors stay wired.
 * A timed-out request poisons the device; never reuse its buffers without reset. */
static bool command(uint32_t op,uint32_t size,uint32_t expected,uint32_t response_size)
{
 uint64_t deadline;uint64_t fence=++gpu.fence;
 struct rwvg_hdr *req=(struct rwvg_hdr *)gpu.request;
 if(size<sizeof(*req)||size>PAGE_SIZE||response_size>PAGE_SIZE)return false;
 *req=rwvg_header(op,fence);bzero((void *)gpu.response,PAGE_SIZE);
 gpu.desc[0]=(struct descriptor){kvtophys(gpu.request),size,1,1};
 gpu.desc[1]=(struct descriptor){kvtophys(gpu.response),response_size,2,0};
 gpu.avail->ring[gpu.avail->idx%RWVG_QSIZE]=0;
 barrier();gpu.avail->idx++;barrier();wr(R_NOTIFY,0);
 clock_interval_to_deadline(500,NSEC_PER_MSEC,&deadline);
 for(;;){
  barrier();if(gpu.used->idx!=gpu.seen)break;
  if(mach_absolute_time()>deadline){printf("virtio-gpu: op %#x fence %llu timeout\n",op,fence);fail("control queue timeout");return false;}
  delay(10);
 }
 uint16_t next=gpu.used->idx;
 uint32_t id=gpu.used->ring[gpu.seen%RWVG_QSIZE].id,len=gpu.used->ring[gpu.seen%RWVG_QSIZE].len;
 barrier();gpu.seen++;
 struct rwvg_hdr *res=(struct rwvg_hdr *)gpu.response;
 if(next!=gpu.seen||!rwvg_completion(res,len,id,response_size,expected,fence)){
  printf("virtio-gpu: op %#x response %#x len %u id %u fence %llu/%llu\n",op,res->type,len,id,res->fence,fence);
  fail("invalid control completion");return false;
 }
 wr(R_ACK,rr(R_INTR));return true;
}
static bool transfer(void)
{
 struct rwvg_transfer *t=(void *)gpu.request;bzero(t,sizeof(*t));
 t->rect=(struct rwvg_rect){0,0,gpu.width,gpu.height};t->resource=RESOURCE;
 if(!command(RWVG_TRANSFER,sizeof(*t),RWVG_OK_NODATA,sizeof(struct rwvg_hdr)))return false;
 struct rwvg_flush *f=(void *)gpu.request;bzero(f,sizeof(*f));
 f->rect=(struct rwvg_rect){0,0,gpu.width,gpu.height};f->resource=RESOURCE;
 return command(RWVG_FLUSH,sizeof(*f),RWVG_OK_NODATA,sizeof(struct rwvg_hdr));
}
/* Called after the framebuffer's geometry and pointer are stable, before its
 * presenter draws the first frame. No phone device is ever probed. */
bool redwood_virtio_gpu_start(const void *pixels,uint32_t w,uint32_t h,uint32_t stride)
{
 uint32_t enabled=0;uint64_t bytes=0;
 PE_parse_boot_argn("virtio_gpu",&enabled,sizeof(enabled));
 if(!enabled)return false;
 lck_mtx_lock(&vgpu_lock);
 if(gpu.attempted){bool ok=gpu.ready;lck_mtx_unlock(&vgpu_lock);return ok;}
 gpu.attempted=true;
 if(pe_arm_get_soc_base_phys()!=0x08000000ul || !pixels || !rwvg_geometry(w,h,stride,&bytes)){
  printf("virtio-gpu: non-QEMU platform or unsupported packed framebuffer; no MMIO\n");goto out;
 }
 vm_offset_t bus=ml_io_map(0x0a000000,32*0x200);
 if(!bus)goto out;
 for(unsigned i=0;i<32;i++){
  gpu.regs=bus+i*0x200;
  if(rr(R_MAGIC)==0x74726976 && rr(R_ID)==16)break;
  gpu.regs=0;
 }
 if(!gpu.regs){printf("virtio-gpu: no virtual GPU device\n");goto out;}
 if(rr(R_VERSION)!=2){fail("legacy transport unsupported");goto out;}
 if(!reset_device())goto out;
 wr(R_STATUS,1);wr(R_STATUS,3);
 wr(R_FEATURES_SEL,0);uint32_t low=rr(R_FEATURES);
 wr(R_FEATURES_SEL,1);uint32_t high=rr(R_FEATURES);
 printf("virtio-gpu: host features %#x:%08x; negotiating VERSION_1 only (no VIRGL/3D)\n",high,low);
 if(!(high&1)){fail("VERSION_1 missing");goto out;}
 wr(R_DRV_FEATURES_SEL,0);wr(R_DRV_FEATURES,0);
 wr(R_DRV_FEATURES_SEL,1);wr(R_DRV_FEATURES,1);wr(R_STATUS,11);
 if(!(rr(R_STATUS)&8)){fail("features refused");goto out;}
 wr(R_QSEL,0);
 if(rr(R_QMAX)<RWVG_QSIZE||rr(R_QREADY)){fail("control queue unavailable");goto out;}
 if(!alloc_page(&gpu.queue)||!alloc_page(&gpu.request)||!alloc_page(&gpu.response)){fail("DMA allocation failed");goto out;}
 gpu.desc=(void *)gpu.queue;gpu.avail=(void *)(gpu.queue+AVAIL_OFFSET);gpu.used=(void *)(gpu.queue+USED_OFFSET);
 gpu.avail->flags=1;wr(R_QNUM,RWVG_QSIZE);
 wr64(R_QDESC,kvtophys(gpu.queue));wr64(R_QAVAIL,kvtophys(gpu.queue+AVAIL_OFFSET));wr64(R_QUSED,kvtophys(gpu.queue+USED_OFFSET));
 barrier();wr(R_QREADY,1);wr(R_STATUS,15);
 if(!command(RWVG_GET_DISPLAY_INFO,sizeof(struct rwvg_hdr),RWVG_OK_DISPLAY_INFO,sizeof(struct rwvg_display_info)))goto out;
 struct rwvg_display_info *info=(void *)gpu.response;
 printf("virtio-gpu: scanout0 enabled=%u preferred=%ux%u\n",info->mode[0].enabled,info->mode[0].rect.width,info->mode[0].rect.height);
 if(!info->mode[0].enabled){fail("scanout0 unavailable");goto out;}
 gpu.width=w;gpu.height=h;gpu.pixels=(uintptr_t)pixels;
 struct rwvg_create *c=(void *)gpu.request;bzero(c,sizeof(*c));c->resource=RESOURCE;c->format=RWVG_B8G8R8X8_UNORM;c->width=w;c->height=h;
 if(!command(RWVG_CREATE_2D,sizeof(*c),RWVG_OK_NODATA,sizeof(struct rwvg_hdr)))goto out;
 struct rwvg_attach *a=(void *)gpu.request;bzero(a,PAGE_SIZE);
 struct rwvg_mem *mem=(void *)(a+1);
 int n=rwvg_backing((uintptr_t)pixels,bytes,PAGE_SIZE,mem,(PAGE_SIZE-sizeof(*a))/sizeof(*mem),physical,NULL);
 if(n<=0){fail("framebuffer backing translation failed");goto out;}
 a->resource=RESOURCE;a->entries=(uint32_t)n;
 printf("virtio-gpu: backing %llu bytes in %d physical extents, first=%#llx; no guest pixel copy\n",bytes,n,mem[0].addr);
 if(!command(RWVG_ATTACH_BACKING,sizeof(*a)+(uint32_t)n*sizeof(*mem),RWVG_OK_NODATA,sizeof(struct rwvg_hdr)))goto out;
 /* Populate host resource before scanout, then publish it. */
 if(!transfer())goto out;
 struct rwvg_scanout *s=(void *)gpu.request;bzero(s,sizeof(*s));s->rect=(struct rwvg_rect){0,0,w,h};s->resource=RESOURCE;
 if(!command(RWVG_SET_SCANOUT,sizeof(*s),RWVG_OK_NODATA,sizeof(struct rwvg_hdr)))goto out;
 if(!transfer())goto out;
 gpu.ready=true;
 printf("virtio-gpu: READY %ux%u, resource1, 2D scanout; CoreAnimation renderer remains software\n",w,h);
out:
 {bool ready=gpu.ready;lck_mtx_unlock(&vgpu_lock);return ready;}
}
void redwood_virtio_gpu_present(void)
{
 if(!gpu.ready)return;
 lck_mtx_lock(&vgpu_lock);
 if(gpu.ready && transfer()){
  gpu.frames++;
  if(gpu.frames<=3 || gpu.frames%120==0)
   printf("virtio-gpu: presented frame %llu, completed fence %llu (TRANSFER+FLUSH)\n",gpu.frames,gpu.fence);
 }
 lck_mtx_unlock(&vgpu_lock);
}
