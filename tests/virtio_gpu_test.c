#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "redwood_virtio_gpu.h"
struct map {uintptr_t base;unsigned mode,calls;};
static uint64_t pa(void *ctx,uintptr_t va)
{
 struct map *m=ctx;m->calls++;uintptr_t off=va-m->base;
 if(m->mode==2 && off>=16384)return 0;
 if(m->mode==1 && off>=16384)return 0x200000+off;
 return 0x100000+off;
}
int main(void)
{
 uint64_t bytes=0;
 assert(rwvg_geometry(1125,2048,4500,&bytes)&&bytes==9216000);
 assert(!rwvg_geometry(0,2048,0,&bytes));assert(!rwvg_geometry(1125,2048,4608,&bytes));
 assert(!rwvg_geometry(0xffffffffu,0xffffffffu,0xfffffffcu,&bytes));
 struct rwvg_mem e[8]={0};struct map m={.base=0x800000};
 assert(rwvg_backing(m.base,32768,16384,e,8,pa,&m)==1);
 assert(e[0].addr==0x100000&&e[0].length==32768&&m.calls==2);
 m.mode=1;m.calls=0;
 assert(rwvg_backing(m.base,32768,16384,e,8,pa,&m)==2);
 assert(e[0].length==16384&&e[1].addr==0x204000&&e[1].length==16384);
 assert(rwvg_backing(m.base,32768,16384,e,1,pa,&m)==-1);
 m.mode=2;assert(rwvg_backing(m.base,32768,16384,e,8,pa,&m)==-1);
 m.mode=0;assert(rwvg_backing(m.base+3,32768,16384,e,8,pa,&m)==1);
 assert(e[0].addr==0x100003&&e[0].length==32768);
 assert(rwvg_backing(m.base,0,16384,e,8,pa,&m)==-1);
 assert(rwvg_backing(m.base,32768,123,e,8,pa,&m)==-1);
 assert(rwvg_backing(UINTPTR_MAX-4,8,16384,e,8,pa,&m)==-1);
 struct rwvg_hdr h=rwvg_header(RWVG_OK_NODATA,99);
 assert(rwvg_completion(&h,24,0,24,RWVG_OK_NODATA,99));
 assert(!rwvg_completion(&h,23,0,24,RWVG_OK_NODATA,99));
 assert(!rwvg_completion(&h,25,0,24,RWVG_OK_NODATA,99));
 assert(!rwvg_completion(&h,24,1,24,RWVG_OK_NODATA,99));
 assert(!rwvg_completion(&h,24,0,24,RWVG_OK_NODATA,100));
 assert(!rwvg_completion(&h,24,0,24,0x1200,99));
 h.flags=0;assert(!rwvg_completion(&h,24,0,24,RWVG_OK_NODATA,99));
 uint32_t stream[32]={0};const uint32_t red[4]={0x3f800000,0,0,0x3f800000};
 unsigned n=rwvg_virgl_clear(stream,32,2,red,1);
 assert(n==19 && stream[0]==0x00050801 && stream[1]==1 && stream[2]==2 && stream[3]==1);
 assert(stream[6]==0x00030005 && stream[7]==1 && stream[8]==0 && stream[9]==1);
 assert(stream[10]==0x00080007 && stream[11]==4);
 assert(stream[12]==red[0] && stream[13]==0 && stream[14]==0 && stream[15]==red[3]);
 assert(stream[16]==0 && stream[17]==0 && stream[18]==0);
 unsigned at=0,packets=0;while(at<n){at+=1+(stream[at]>>16);packets++;}assert(at==n && packets==3);
 assert(rwvg_virgl_clear(stream,32,2,red,0)==13 && stream[0]==0x00030005 && stream[4]==0x00080007);
 stream[0]=0xabcdef;
 assert(!rwvg_virgl_clear(stream,18,2,red,1) && stream[0]==0xabcdef);
 assert(!rwvg_virgl_clear(stream,32,0,red,1));
 assert(!rwvg_virgl_clear(NULL,32,2,red,1));assert(!rwvg_virgl_clear(stream,32,2,NULL,1));
 h=rwvg_header(RWVG_OK_CAPSET_INFO,7);
 assert(!rwvg_completion(&h,24,0,sizeof(struct rwvg_cap_info),RWVG_OK_CAPSET_INFO,7));
 assert(rwvg_completion(&h,sizeof(struct rwvg_cap_info),0,sizeof(struct rwvg_cap_info),RWVG_OK_CAPSET_INFO,7));
 puts("PASS: virgl v1 surface/framebuffer/clear packet encoding, reuse, capacity guards and complete capset responses");
 puts("PASS: packed mode bounds, scatter/gather, extent coalescing, unaligned pages, unmapped backing, limits, response/fence validation");
 return 0;
}
