#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
bool egl_init(const char *,int,void **);
static int marker,errors;
void error_setg_internal(void **errp,const char *file,int line,const char *func,const char *fmt,...)
{
 assert(file&&line&&func&&fmt);va_list ap;va_start(ap,fmt);
 assert(!strcmp(fmt,"%s"));const char *msg=va_arg(ap,const char *);assert(msg&&*msg);
 va_end(ap);*errp=&marker;errors++;
}
int main(void)
{
 void *err=NULL;
 assert(!egl_init(NULL,0,&err)&&err==&marker&&errors==1);
 err=NULL;assert(!egl_init(NULL,3,&err)&&err==&marker&&errors==2);
 /* No QEMU symbols are exported by this binary: ABI failure must set Error. */
 err=NULL;assert(!egl_init(NULL,1,&err)&&err==&marker&&errors==3);
 assert(!egl_init(NULL,0,NULL)&&errors==3);
 puts("PASS: surfaceless adapter rejects unsupported modes/ABI and reports Error without a render node");
 return 0;
}
