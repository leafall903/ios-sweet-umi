/* 20F66 EAGL/CoreAnimation bridge based on MakrSas/ios-topaz topaz_inject.c
 * and pinned HakiOS eagl.c. Explicit target guards; no generic address patching.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include "probe_log.h"
#include "target_guard.h"
#include "slot_transaction.h"
extern const void *_dyld_get_shared_cache_range(size_t *length);
#define GL_LIBRARY "/usr/local/lib/umi-ios165-v1/libumi-ios165-v1-gl.dylib"
static uintptr_t slide;
static int (*original_supported)(void *);
static void (*original_uniform)(void *,unsigned,size_t,size_t,const float *);
static void (*function_refused)(unsigned);
static void (*function_values)(void *,unsigned,unsigned,size_t,size_t,const float *);
static unsigned active_function(void *context) { return *(*(uint8_t **)((char *)context+0x10)+0x10); }
static int supported(void *context) { int yes=original_supported(context); if (!yes && function_refused) function_refused(active_function(context)); return yes; }
static void uniform(void *context,unsigned index,size_t components,size_t count,const float *values) {
    function_values(context,active_function(context),index,components,count,values);
    original_uniform(context,index,components,count,values);
}
struct slot_addresses { uintptr_t values[2]; };
static int slot_read(void *ctx,unsigned i,uintptr_t *value) {
    struct slot_addresses *addresses=ctx;*value=*(uintptr_t*)addresses->values[i];return 1;
}
static int slot_protect(void *ctx,unsigned i,int writable) {
    struct slot_addresses *addresses=ctx;
    vm_address_t page=(vm_address_t)addresses->values[i] & ~(vm_address_t)(vm_page_size-1);
    vm_prot_t protection=writable?(VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY):VM_PROT_READ;
    return vm_protect(mach_task_self(),page,vm_page_size,FALSE,protection)==KERN_SUCCESS;
}
static int slot_write(void *ctx,unsigned i,uintptr_t value) {
    struct slot_addresses *addresses=ctx;*(uintptr_t*)addresses->values[i]=value;return 1;
}
/* Load Apple's class before any CA vtable is changed. Merely loading our
 * manually-installed bridge does not cause EAGLContext to be registered. */
static int original_eagl_ready(void) {
    static const struct { const char *name,*types; uintptr_t imp; unsigned char prefix[16]; } methods[] = {
        {"initWithAPI:properties:","@32@0:8Q16@24",0x1d3159270ULL,{0xff,0x43,0x01,0xd1,0xf6,0x57,0x02,0xa9,0xf4,0x4f,0x03,0xa9,0xfd,0x7b,0x04,0xa9}},
        {"API","Q16@0:8",0x1d3159880ULL,{0x08,0x04,0x40,0xf9,0x00,0x09,0x40,0xf9,0xc0,0x03,0x5f,0xd6,0x08,0x04,0x40,0xf9}},
        {"sharegroup","@16@0:8",0x1d315988cULL,{0x08,0x04,0x40,0xf9,0x00,0x05,0x40,0xf9,0xc0,0x03,0x5f,0xd6,0xff,0x83,0x00,0xd1}},
        {"setParameter:to:","Q28@0:8I16^i20",0x1d315a064ULL,{0xfd,0x7b,0xbf,0xa9,0xfd,0x03,0x00,0x91,0x08,0x04,0x40,0xf9,0xa9,0xf5,0x05,0xb0}},
        {"getMacroContextPrivate","^{EAGLMacroContext=}16@0:8",0x1d315a11cULL,{0x08,0x04,0x40,0xf9,0x00,0x81,0x00,0x91,0xc0,0x03,0x5f,0xd6,0x36,0x0b,0x00,0x14}},
        {"texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:","B72@0:8^{__IOSurface=}16Q24Q32I40I44Q48Q56I64B68",0x1d3159efcULL,{0xff,0x03,0x02,0xd1,0xfa,0x67,0x03,0xa9,0xf8,0x5f,0x04,0xa9,0xf6,0x57,0x05,0xa9}},
        {"attachImage:toCoreSurface:invertedRender:","B36@0:8Q16^{__IOSurface=}24B32",0x1d3159c60ULL,{0xff,0x83,0x01,0xd1,0xfa,0x67,0x01,0xa9,0xf8,0x5f,0x02,0xa9,0xf6,0x57,0x03,0xa9}},
        {"dealloc","v16@0:8",0x1d3159714ULL,{0xff,0xc3,0x00,0xd1,0xf4,0x4f,0x01,0xa9,0xfd,0x7b,0x02,0xa9,0xfd,0x83,0x00,0x91}},
    };
    _Static_assert(sizeof methods / sizeof methods[0] == 8, "EAGL preflight method count");
    void *original = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY | RTLD_LOCAL);
    if (!original) { umi_ios165_log("gl_original_gles_failed", dlerror()); return 0; }
    /* Keep the handle resident: after success these classes are the bridge's host. */
    Class cls = objc_getClass("EAGLContext");
    if (!cls) { umi_ios165_log("gl_eagl_preflight_failed", "original OpenGLES has no EAGLContext"); return 0; }
    static const unsigned char gles_uuid[16]={0x46,0x2f,0xbe,0x47,0x4b,0x9e,0x3c,0x2e,0x8e,0xf6,0x9d,0xb6,0xa5,0xc5,0x8a,0xa6};
    size_t length=0; const unsigned char *cache=_dyld_get_shared_cache_range(&length);
    if(!umi165_cache_matches(cache,length)) return 0;
    for (unsigned i=0; i<sizeof methods / sizeof methods[0]; i++) {
        Method method=class_getInstanceMethod(cls, sel_registerName(methods[i].name));
        IMP imp=method?method_getImplementation(method):NULL;
        const char *types=method?method_getTypeEncoding(method):NULL;
        uintptr_t delta=methods[i].imp-UMI165_CACHE_BASE;
        if (!imp || !types || strcmp(types,methods[i].types) || length<16 || delta>length-16 ||
            (uintptr_t)cache>UINTPTR_MAX-delta || (uintptr_t)imp!=(uintptr_t)cache+delta ||
            !umi165_image_matches((const void*)imp,gles_uuid) || memcmp((const void*)imp,methods[i].prefix,16)) {
            umi_ios165_log("gl_eagl_preflight_failed",methods[i].name);return 0;
        }
    }
    umi_ios165_log("gl_eagl_preflight_pass", "original OpenGLES loaded; eight EAGL exact IMP/type/prefix+OpenGLES image UUID; no hooks yet");
    return 1;
}
__attribute__((constructor)) static void umi_v20_backboard(void) {
    const char *name=getprogname();
    if (!name || strcmp(name,"backboardd")) return;
    umi_ios165_log("gl_constructor_enter",name);
    char args[1024]={0}; size_t sz=sizeof args-1;
    if (sysctlbyname("kern.bootargs",args,&sz,NULL,0)!=0 || !getenv("UMI_GL_ENABLE") || strcmp(getenv("UMI_GL_ENABLE"),"1") || !strstr(args,"umi_gl=1")) { umi_ios165_log("gl_skipped","explicit umi_gl=1 absent");return; }
    size_t length=0; const unsigned char *cache=_dyld_get_shared_cache_range(&length);
    if (!umi165_cache_matches(cache,length)) { umi_ios165_log("gl_guard_failed","cache UUID differs from Apple20F66");return; }
    slide=(uintptr_t)cache-0x180000000ULL;
    const uintptr_t slot_s=0x1d5b3b8c8ULL,slot_u=0x1d5b3b8d8ULL,fn_s=0x1884a9308ULL,fn_u=0x1884a9350ULL;
    const unsigned char prefix_s[16]={0x08,0x08,0x40,0xf9,0x09,0x41,0x40,0x39,0x3f,0x65,0x01,0x71,0xa8,0x01,0x00,0x54};
    const unsigned char prefix_u[16]={0xf8,0x5f,0xbc,0xa9,0xf6,0x57,0x01,0xa9,0xf4,0x4f,0x02,0xa9,0xfd,0x7b,0x03,0xa9};
    if (slot_u-0x180000000ULL+8>length || fn_u-0x180000000ULL+16>length || *(uintptr_t *)(slot_s+slide)!=fn_s+slide || *(uintptr_t *)(slot_u+slide)!=fn_u+slide || memcmp((const void *)(fn_s+slide),prefix_s,16) || memcmp((const void *)(fn_u+slide),prefix_u,16)) { umi_ios165_log("gl_guard_failed","vtable pointers or original instructions differ");return; }
    const unsigned char quartz_uuid[16]={0xdc,0x2d,0xd4,0xc1,0x78,0x66,0x34,0x43,0x80,0x50,0xfc,0x91,0x5a,0x02,0x04,0xc1};
    if(!umi165_image_matches((const void*)(fn_s+slide),quartz_uuid) || !umi165_image_matches((const void*)(fn_u+slide),quartz_uuid)) { umi_ios165_log("gl_guard_failed","QuartzCore image UUID mismatch");return; }
    io_service_t service=0;
    umi_ios165_log("gl_gpu_wait","waiting up to 40 seconds for SweetGPU");
    for (int i=0;i<400;i++) {
        service=IOServiceGetMatchingService(0,IOServiceMatching("SweetGPU"));
        if (service) break;
        usleep(100000);
    }
    if (!service) { umi_ios165_log("gl_skipped","SweetGPU service absent");return; } IOObjectRelease(service);
    void *lib=dlopen(GL_LIBRARY,RTLD_NOW);
    if (!lib) { umi_ios165_log("gl_dlopen_failed",dlerror());return; }
    int (*probe)(char *,size_t)=dlsym(lib,"umi_v20_probe_gpu");
    unsigned (*install)(void)=dlsym(lib,"umi_v20_install_eagl");
    function_values=dlsym(lib,"hakios_gl_function_values");function_refused=dlsym(lib,"hakios_gl_function_refused");
    if (!probe || !install || !function_values || !function_refused) { umi_ios165_log("gl_symbols_failed","bridge exports incomplete");return; }
    char description[240];
    if (!probe(description,sizeof description)) { umi_ios165_log("gl_gpu_probe_failed",description);return; }
    umi_ios165_log("gl_gpu_probe_pass",description);
    if (!original_eagl_ready()) return;
    original_supported=(void *)(fn_s+slide);original_uniform=(void *)(fn_u+slide);
    struct slot_addresses addresses={{slot_s+slide,slot_u+slide}};
    struct umi165_slot_transaction transaction={
        .ops={&addresses,slot_read,slot_protect,slot_write},
        .original={fn_s+slide,fn_u+slide},
        .replacement={(uintptr_t)supported,(uintptr_t)uniform},.touched=0
    };
    int patched=umi165_slots_install(&transaction);
    if(patched!=1) {
        umi_ios165_log("gl_hooks_failed",patched<0?"slot rollback/protection not proven; exiting":"two-slot transaction rejected; originals restored");
        if(patched<0)_exit(126);
        return;
    }
    unsigned replaced=install();
    if (replaced!=8) {
        int restored=umi165_slots_restore(&transaction);
        umi_ios165_log("gl_install_failed",restored<0?"slot rollback unproven; exiting":"unexpected EAGL install count; exiting to discard partial class state");
        _exit(126);
    }
    umi_ios165_log("gl_enabled","20F66 cache+instructions+vtables; FD650 clear/readback; eight EAGL methods");
}
