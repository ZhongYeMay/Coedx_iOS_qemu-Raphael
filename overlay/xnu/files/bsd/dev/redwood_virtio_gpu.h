/* VirtIO GPU 1.2, little-endian 2D control protocol. Shared wire code and tests. */
#ifndef REDWOOD_VIRTIO_GPU_H
#define REDWOOD_VIRTIO_GPU_H
#include <stdint.h>
#include <stddef.h>
#define RWVG_QSIZE 16
#define RWVG_FENCE 1u
#define RWVG_GET_DISPLAY_INFO 0x0100u
#define RWVG_CREATE_2D 0x0101u
#define RWVG_UNREF 0x0102u
#define RWVG_SET_SCANOUT 0x0103u
#define RWVG_FLUSH 0x0104u
#define RWVG_TRANSFER 0x0105u
#define RWVG_ATTACH_BACKING 0x0106u
#define RWVG_OK_NODATA 0x1100u
#define RWVG_OK_DISPLAY_INFO 0x1101u
#define RWVG_B8G8R8X8_UNORM 2u
struct rwvg_hdr { uint32_t type,flags; uint64_t fence; uint32_t ctx;uint8_t ring,pad[3]; };
struct rwvg_rect { uint32_t x,y,width,height; };
struct rwvg_create { struct rwvg_hdr hdr;uint32_t resource,format,width,height; };
struct rwvg_attach { struct rwvg_hdr hdr;uint32_t resource,entries; };
struct rwvg_mem { uint64_t addr;uint32_t length,pad; };
struct rwvg_scanout {struct rwvg_hdr hdr;struct rwvg_rect rect;uint32_t scanout,resource;};
struct rwvg_transfer {struct rwvg_hdr hdr;struct rwvg_rect rect;uint64_t offset;uint32_t resource,pad;};
struct rwvg_flush {struct rwvg_hdr hdr;struct rwvg_rect rect;uint32_t resource,pad;};
struct rwvg_display {struct rwvg_rect rect;uint32_t enabled,flags;};
struct rwvg_display_info {struct rwvg_hdr hdr;struct rwvg_display mode[16];};
_Static_assert(sizeof(struct rwvg_hdr)==24,"virtio GPU header");
_Static_assert(sizeof(struct rwvg_create)==40,"create2d");
_Static_assert(sizeof(struct rwvg_scanout)==48,"scanout");
_Static_assert(sizeof(struct rwvg_transfer)==56,"transfer2d");
_Static_assert(sizeof(struct rwvg_flush)==48,"flush");
_Static_assert(sizeof(struct rwvg_attach)==32,"attach");
_Static_assert(sizeof(struct rwvg_mem)==16,"backing entry");
static inline int rwvg_geometry(uint32_t w,uint32_t h,uint32_t stride,uint64_t *bytes)
{
 /* Boot framebuffer must be packed: resource2D has no independent pitch. */
 if(!w||!h||w>4096||h>4096||stride!=(uint64_t)w*4) return 0;
 *bytes=(uint64_t)stride*h;return 1;
}
/* Build scatter/gather entries across arbitrary page boundaries, including an
 * unaligned first byte. Physical addresses come from the caller, never a cast. */
static inline int rwvg_backing(uintptr_t base,uint64_t bytes,uint32_t page_size,
 struct rwvg_mem *entries,uint32_t limit,uint64_t (*physical)(void *,uintptr_t),void *ctx)
{
 uint64_t at=0;uint32_t n=0;
 if(!bytes||!page_size||(page_size&(page_size-1))||bytes>UINTPTR_MAX-base) return -1;
 while(at<bytes) {
  uintptr_t va=base+(uintptr_t)at;uint64_t pa=physical(ctx,va);
  uint32_t len=page_size-(uint32_t)(va&(page_size-1));
  if(len>bytes-at)len=(uint32_t)(bytes-at);
  if(!pa||pa>UINT64_MAX-len) return -1;
  if(n && entries[n-1].addr+entries[n-1].length==pa && entries[n-1].length<=UINT32_MAX-len)
   entries[n-1].length+=len;
  else {
   if(n==limit)return -1;
   entries[n++]=(struct rwvg_mem){pa,len,0};
  }
  at+=len;
 }
 return (int)n;
}
static inline struct rwvg_hdr rwvg_header(uint32_t op,uint64_t fence)
{
 struct rwvg_hdr h={0};h.type=op;h.flags=RWVG_FENCE;h.fence=fence;return h;
}
static inline int rwvg_completion(const struct rwvg_hdr *h,uint32_t length,uint32_t id,
 uint32_t capacity,uint32_t expected,uint64_t fence)
{
 return id==0 && length>=sizeof(*h) && length==capacity && h->type==expected &&
     (h->flags&RWVG_FENCE) && h->fence==fence;
}
/* Optional virgl v1 transport smoke test; these are not OpenGL/Metal APIs. */
#define RWVG_GET_CAPSET_INFO 0x0108u
#define RWVG_GET_CAPSET 0x0109u
#define RWVG_OK_CAPSET_INFO 0x1102u
#define RWVG_OK_CAPSET 0x1103u
#define RWVG_CTX_CREATE 0x0200u
#define RWVG_CTX_DESTROY 0x0201u
#define RWVG_CTX_ATTACH 0x0202u
#define RWVG_CTX_DETACH 0x0203u
#define RWVG_CREATE_3D 0x0204u
#define RWVG_FROM_HOST_3D 0x0206u
#define RWVG_SUBMIT_3D 0x0207u
#define RWVG_TEST_WIDTH 64u
#define RWVG_TEST_HEIGHT 64u
#define RWVG_TEST_BYTES (RWVG_TEST_WIDTH*RWVG_TEST_HEIGHT*4u)
struct rwvg_cap_info_req {struct rwvg_hdr hdr;uint32_t index,pad;};
struct rwvg_cap_info {struct rwvg_hdr hdr;uint32_t id,version,size,pad;};
struct rwvg_cap_req {struct rwvg_hdr hdr;uint32_t id,version;};
struct rwvg_ctx_create {struct rwvg_hdr hdr;uint32_t length,init;char name[64];};
struct rwvg_ctx_resource {struct rwvg_hdr hdr;uint32_t resource,pad;};
struct rwvg_create3d {struct rwvg_hdr hdr;uint32_t resource,target,format,bind,width,height,depth,array_size,last_level,samples,flags,pad;};
struct rwvg_box {uint32_t x,y,z,w,h,d;};
struct rwvg_transfer3d {struct rwvg_hdr hdr;struct rwvg_box box;uint64_t offset;uint32_t resource,level,stride,layer_stride;};
struct rwvg_submit {struct rwvg_hdr hdr;uint32_t bytes,pad;};
_Static_assert(sizeof(struct rwvg_cap_info)==40,"capset info");
_Static_assert(sizeof(struct rwvg_ctx_create)==96,"context create");
_Static_assert(sizeof(struct rwvg_create3d)==72,"resource create3d");
_Static_assert(sizeof(struct rwvg_transfer3d)==72,"transfer3d");
_Static_assert(sizeof(struct rwvg_submit)==32,"submit3d prefix");
/* virgl v1: CREATE_OBJECT(surface), SET_FRAMEBUFFER_STATE, CLEAR color0.
 * Payload lengths count DWORDs after the packet header. No shader/rasterizer
 * pipeline is claimed by a clear test. The second pass reuses its surface.
 */
static inline unsigned rwvg_virgl_clear(uint32_t *out,unsigned capacity,uint32_t resource,
 const uint32_t rgba[4],int create_surface)
{
 unsigned n=create_surface?19u:13u,i=0;
 if(!out||!rgba||!resource||capacity<n)return 0;
 if(create_surface){
  out[i++]=1u|(8u<<8)|(5u<<16);out[i++]=1;out[i++]=resource;
  out[i++]=1;out[i++]=0;out[i++]=0; /* surface1, BGRA8, mip0/layer0 */
 }
 out[i++]=5u|(3u<<16);out[i++]=1;out[i++]=0;out[i++]=1;
 out[i++]=7u|(8u<<16);out[i++]=4; /* PIPE_CLEAR_COLOR0 */
 for(unsigned c=0;c<4;c++)out[i++]=rgba[c];
 out[i++]=0;out[i++]=0;out[i++]=0; /* unused depth/stencil */
 return i;
}
#endif
