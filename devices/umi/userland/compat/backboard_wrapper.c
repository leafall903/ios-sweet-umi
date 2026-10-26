/* backboardd wrapper, same PID/Mach service after exec, no image installation here. */
#include <stdlib.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include "probe_log.h"
int main(int argc,char **argv) {
 (void)argc;
 const char *lib="/usr/local/lib/umi-ios165-v1/libumi-ios165-v1-backboard.dylib";
 const char *real="/usr/libexec/real/backboardd";
 umi_ios165_log("gl_wrapper_enter","backboardd");
 char args[1024]={0};size_t count=sizeof(args)-1;int gl_enabled=0;
 if(sysctlbyname("kern.bootargs",args,&count,NULL,0)==0) {
  const char *a=args;while((a=strstr(a,"umi_gl=1"))!=NULL) {
   if((a==args || a[-1]==' ') && (a[8]==0 || a[8]==' ')) {setenv("UMI_GL_ENABLE","1",1);gl_enabled=1;break;}a+=8;
  }
 }
 /* Mesa reads FD_MESA_DEBUG once, at its first context (the GPU probe).
  * Select the upstream no-tiling path before exec, not after that probe. */
 if(gl_enabled) {
  const char *a=args;const char *token="umi-gl-sysmem=1";size_t n=strlen(token);
  while((a=strstr(a,token))!=NULL) {
   if((a==args || a[-1]==' ') && (a[n]==0 || a[n]==' ')) {
    if(setenv("FD_MESA_DEBUG","sysmem",1)==0)
     umi_ios165_log("gl_sysmem_requested","FD_MESA_DEBUG=sysmem before exec and first GPU probe");
    break;
   }
   a+=n;
  }
 }
 if(access(lib,R_OK)==0 && setenv("DYLD_INSERT_LIBRARIES",lib,1)==0) umi_ios165_log("gl_library_requested",lib);
 else umi_ios165_log("gl_library_unavailable",lib);
 argv[0]=(char *)real;execv(real,argv);umi_ios165_log("gl_exec_failed",strerror(errno));return 127;
}
