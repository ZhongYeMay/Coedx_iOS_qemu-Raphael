/* QEMU virtio-mmio GPU display driver for XNU, with an opt-in virgl smoke test.
 * Boot arg virtio_gpu=1, QEMU -device virtio-gpu-device,id=vgpu.
 * This is not a Metal/OpenGL implementation. virtio_gpu_virgl=1 negotiates
 * virgl v1; virtio_gpu_3d_test=1 runs an isolated 64x64 context/clear/readback
 * test. Neither flag connects CoreAnimation to the 3D command path.
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
 uint32_t width,height;uintptr_t pixels;bool ready,attempted,virgl;
 vm_offset_t test_pixels;
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
static bool command_ctx(uint32_t op,uint32_t size,uint32_t expected,uint32_t response_size,uint32_t context)
{
 uint64_t deadline;uint64_t fence=++gpu.fence;
 struct rwvg_hdr *req=(struct rwvg_hdr *)gpu.request;
 if(size<sizeof(*req)||size>PAGE_SIZE||response_size>PAGE_SIZE)return false;
 *req=rwvg_header(op,fence);req->ctx=context;bzero((void *)gpu.response,PAGE_SIZE);
 gpu.desc[0]=(struct descriptor){kvtophys(gpu.request),size,1,1};
 gpu.desc[1]=(struct descriptor){kvtophys(gpu.response),response_size,2,0};
 gpu.avail->ring[gpu.avail->idx%RWVG_QSIZE]=0;
 barrier();gpu.avail->idx++;barrier();wr(R_NOTIFY,0);
 clock_interval_to_deadline(gpu.virgl?2500:500,NSEC_PER_MSEC,&deadline);
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
static bool command(uint32_t op,uint32_t size,uint32_t expected,uint32_t response_size)
{return command_ctx(op,size,expected,response_size,0);}
static bool virgl_capabilities(void)
{
 uint32_t count=rr(0x10c); /* device configuration: num_capsets */
 if(count>16)count=16;
 for(uint32_t i=0;i<count;i++){
  struct rwvg_cap_info_req *q=(void *)gpu.request;bzero(q,sizeof(*q));q->index=i;
  if(!command(RWVG_GET_CAPSET_INFO,sizeof(*q),RWVG_OK_CAPSET_INFO,sizeof(struct rwvg_cap_info)))return false;
  struct rwvg_cap_info info=*(struct rwvg_cap_info *)gpu.response;
  printf("virtio-gpu: capset %u: id=%u version=%u bytes=%u\n",i,info.id,info.version,info.size);
  if(info.id!=1)continue; /* Explicitly virgl v1, not virgl2/Venus. */
  if(info.version<1||info.size<72||info.size>PAGE_SIZE-sizeof(struct rwvg_hdr))return false;
  struct rwvg_cap_req *c=(void *)gpu.request;bzero(c,sizeof(*c));c->id=1;c->version=1;
  if(!command(RWVG_GET_CAPSET,sizeof(*c),RWVG_OK_CAPSET,sizeof(struct rwvg_hdr)+info.size))return false;
  const uint32_t *caps=(const void *)(gpu.response+sizeof(struct rwvg_hdr));
  /* virgl_caps_v1: version, 16 sampler mask words, then render mask. */
  if(!(caps[17]&(1u<<1))){printf("virtio-gpu: BGRA8 render-target format unavailable\n");return false;}
  printf("virtio-gpu: virgl v1 capabilities accepted; BGRA8 render target supported\n");return true;
 }
 printf("virtio-gpu: virgl v1 capset unavailable\n");return false;
}
/* This submits host-rendered clears and checks a DMA readback. The guest fills
 * backing memory with a different sentinel before each pass, never expected color.
 * It exercises resource, context and fence plumbing; it is not a shader test.
 */
static bool virgl_selftest(void)
{
 const uint32_t context=1,resource=2;
 bool matched=true;
 if(PAGE_SIZE<RWVG_TEST_BYTES||!alloc_page(&gpu.test_pixels))return false;
 struct rwvg_ctx_create *c=(void *)gpu.request;bzero(c,sizeof(*c));
 const char name[]="xnu-virgl-smoke";c->length=sizeof(name)-1;memcpy(c->name,name,sizeof(name));
 if(!command_ctx(RWVG_CTX_CREATE,sizeof(*c),RWVG_OK_NODATA,sizeof(struct rwvg_hdr),context))return false;
 struct rwvg_create3d *r=(void *)gpu.request;bzero(r,sizeof(*r));
 r->resource=resource;r->target=2;r->format=1;r->bind=2;r->width=RWVG_TEST_WIDTH;r->height=RWVG_TEST_HEIGHT;
 r->depth=1;r->array_size=1;r->flags=1;
 if(!command(RWVG_CREATE_3D,sizeof(*r),RWVG_OK_NODATA,sizeof(struct rwvg_hdr)))return false;
 struct rwvg_attach *a=(void *)gpu.request;bzero(a,sizeof(*a)+sizeof(struct rwvg_mem));a->resource=resource;a->entries=1;
 struct rwvg_mem *mem=(void *)(a+1);mem->addr=kvtophys(gpu.test_pixels);mem->length=RWVG_TEST_BYTES;
 if(!command(RWVG_ATTACH_BACKING,sizeof(*a)+sizeof(*mem),RWVG_OK_NODATA,sizeof(struct rwvg_hdr)))return false;
 struct rwvg_ctx_resource *cr=(void *)gpu.request;bzero(cr,sizeof(*cr));cr->resource=resource;
 if(!command_ctx(RWVG_CTX_ATTACH,sizeof(*cr),RWVG_OK_NODATA,sizeof(struct rwvg_hdr),context))return false;
 for(unsigned pass=0;pass<2;pass++){
  memset((void *)gpu.test_pixels,pass?0x5a:0xa5,RWVG_TEST_BYTES);barrier();
  struct rwvg_submit *sub=(void *)gpu.request;bzero(sub,PAGE_SIZE);
  const uint32_t colors[2][4]={{0x3f800000,0,0,0x3f800000},{0,0x3f800000,0,0x3f800000}};
  unsigned words=rwvg_virgl_clear((void *)(sub+1),(PAGE_SIZE-sizeof(*sub))/4,resource,colors[pass],pass==0);
  if(!words)return false;sub->bytes=words*4;
  if(!command_ctx(RWVG_SUBMIT_3D,sizeof(*sub)+sub->bytes,RWVG_OK_NODATA,sizeof(struct rwvg_hdr),context))return false;
  struct rwvg_transfer3d *t=(void *)gpu.request;bzero(t,sizeof(*t));
  t->box=(struct rwvg_box){0,0,0,RWVG_TEST_WIDTH,RWVG_TEST_HEIGHT,1};t->resource=resource;
  t->stride=RWVG_TEST_WIDTH*4;t->layer_stride=RWVG_TEST_BYTES;
  if(!command_ctx(RWVG_FROM_HOST_3D,sizeof(*t),RWVG_OK_NODATA,sizeof(struct rwvg_hdr),context))return false;
  barrier();const volatile uint32_t *pixels=(const void *)gpu.test_pixels;
  uint32_t expected=pass?0xff00ff00u:0xffff0000u,bad=0;
  for(unsigned i=0;i<RWVG_TEST_BYTES/4;i++)if(pixels[i]!=expected)bad++;
  printf("virtio-gpu: VIRGL_CLEAR pass %u: expected=%#x first=%#x mismatches=%u/%u fence=%llu\n",
   pass+1,expected,pixels[0],bad,RWVG_TEST_BYTES/4,gpu.fence);
  if(bad)matched=false;
 }
 cr=(void *)gpu.request;bzero(cr,sizeof(*cr));cr->resource=resource;
 if(!command_ctx(RWVG_CTX_DETACH,sizeof(*cr),RWVG_OK_NODATA,sizeof(struct rwvg_hdr),context))return false;
 if(!command_ctx(RWVG_CTX_DESTROY,sizeof(struct rwvg_hdr),RWVG_OK_NODATA,sizeof(struct rwvg_hdr),context))return false;
 cr=(void *)gpu.request;bzero(cr,sizeof(*cr));cr->resource=resource;
 if(!command(RWVG_UNREF,sizeof(*cr),RWVG_OK_NODATA,sizeof(struct rwvg_hdr)))return false;
 printf("virtio-gpu: VIRGL_3D_SELFTEST %s: context, resource3D, submit, readback, cleanup; no CoreAnimation acceleration\n",matched?"PASS":"FAIL");
 return matched;
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
 uint32_t enabled=0,want_virgl=0,test3d=0;uint64_t bytes=0;
 PE_parse_boot_argn("virtio_gpu",&enabled,sizeof(enabled));
 if(!enabled)return false;
 PE_parse_boot_argn("virtio_gpu_virgl",&want_virgl,sizeof(want_virgl));
 PE_parse_boot_argn("virtio_gpu_3d_test",&test3d,sizeof(test3d));
 if(test3d)want_virgl=1;
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
 gpu.virgl=want_virgl && (low&1);
 printf("virtio-gpu: host features %#x:%08x; VERSION_1 with %s\n",high,low,gpu.virgl?"VIRGL (opt-in)":"2D only");
 if(want_virgl&&!gpu.virgl)printf("virtio-gpu: VIRGL unavailable; retaining 2D fallback\n");
 if(!(high&1)){fail("VERSION_1 missing");goto out;}
 wr(R_DRV_FEATURES_SEL,0);wr(R_DRV_FEATURES,gpu.virgl?1:0);
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
 if(gpu.virgl){
  if(!virgl_capabilities()){fail("virgl capability validation failed");goto out;}
  if(test3d&&!virgl_selftest()){fail("virgl 3D self-test failed");goto out;}
 }
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
 printf("virtio-gpu: READY %ux%u, resource1, %s; CoreAnimation renderer remains software\n",w,h,gpu.virgl?"virgl-backed scanout":"2D scanout");
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
