/* Opt-in QEMU 10.0.x test adapter: EGL surfaceless instead of a DRM render node.
 * The cloud has no /dev/dri. This intentionally uses host Mesa llvmpipe, not a GPU.
 * It interposes the exported egl_init ABI of QEMU's ui-opengl module only for this
 * test process. No QEMU binary, system library, network policy or device is changed.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>

typedef void *EDisplay;typedef void *EConfig;typedef void *EContext;
typedef unsigned EBool;typedef int EInt;
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 1
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 8
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_OPENGL_API 0x30a2
#define EGL_PLATFORM_SURFACELESS_MESA 0x31dd
#define GL_RENDERER 0x1f01

static bool failure(void **errp,const char *message)
{
 void (*seterr)(void **,const char *,int,const char *,const char *,...)=
  dlsym(RTLD_DEFAULT,"error_setg_internal");
 fprintf(stderr,"surfaceless-test: %s\n",message);
 if(errp&&seterr)seterr(errp,__FILE__,__LINE__,__func__,"%s",message);
 return false;
}
bool egl_init(const char *rendernode,int mode,void **errp)
{
 (void)rendernode;
 if(mode!=1&&mode!=2)return failure(errp,"test adapter supports OpenGL core mode only");
 /* Make the software-only contract explicit even outside the launcher script. */
 if(setenv("LIBGL_ALWAYS_SOFTWARE","1",1)!=0)return failure(errp,"cannot select software renderer");
 void *egl=dlopen("libEGL.so.1",RTLD_NOW|RTLD_GLOBAL);
 if(!egl)return failure(errp,"libEGL.so.1 unavailable");
 EDisplay (*get)(unsigned,void *,const intptr_t *)=dlsym(egl,"eglGetPlatformDisplay");
 EBool (*initialize)(EDisplay,EInt *,EInt *)=dlsym(egl,"eglInitialize");
 EBool (*bind)(unsigned)=dlsym(egl,"eglBindAPI");
 EBool (*choose)(EDisplay,const EInt *,EConfig *,EInt,EInt *)=dlsym(egl,"eglChooseConfig");
 EContext (*init_ctx)(void)=dlsym(RTLD_DEFAULT,"qemu_egl_init_ctx");
 EDisplay *dpy=dlsym(RTLD_DEFAULT,"qemu_egl_display");
 EConfig *config=dlsym(RTLD_DEFAULT,"qemu_egl_config");
 EContext *ctx=dlsym(RTLD_DEFAULT,"qemu_egl_rn_ctx");
 int *qmode=dlsym(RTLD_DEFAULT,"qemu_egl_mode");
 int *opengl=dlsym(RTLD_DEFAULT,"display_opengl");
 if(!get||!initialize||!bind||!choose||!init_ctx||!dpy||!config||!ctx||!qmode||!opengl){
  return failure(errp,"incompatible QEMU/EGL ABI");
 }
 EInt maj=0,min=0,n=0;
 EInt attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,
  EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
 *dpy=get(EGL_PLATFORM_SURFACELESS_MESA,NULL,NULL);
 if(!*dpy||!initialize(*dpy,&maj,&min)||!bind(EGL_OPENGL_API)||!choose(*dpy,attrs,config,1,&n)||n!=1){
  return failure(errp,"EGL context setup failed");
 }
 *qmode=2; /* QEMU 10 DisplayGLMode: off=0, on=1, core=2, es=3. */
 *ctx=init_ctx();if(!*ctx)return failure(errp,"OpenGL context creation failed");
 void *gl=dlopen("libGL.so.1",RTLD_NOW|RTLD_GLOBAL);
 const unsigned char *(*string)(unsigned)=gl?dlsym(gl,"glGetString"):NULL;
 const char *renderer=string?(const char *)string(GL_RENDERER):NULL;
 if(!renderer||(!strstr(renderer,"llvmpipe")&&!strstr(renderer,"softpipe")))
  return failure(errp,"renderer is not a recognized software renderer");
 *opengl=1;
 fprintf(stderr,"surfaceless-test: EGL %d.%d, GL_RENDERER=%s; SOFTWARE HOST TEST ONLY\n",maj,min,renderer?renderer:"unknown");
 return true;
}
