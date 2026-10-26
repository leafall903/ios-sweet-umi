/* Minimal prewarm adaptation from HakiOS inject_media.c (MIT; LICENSE.upstream).
 * Only skips portrait CoreImage prewarm to keep original media decode service alive.
 * No camera vendor, torch, audio, encoder, stabilization or decoder registration hooks. */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "media_logic.h"
static const char *image_path="/System/Library/PrivateFrameworks/CMCapture.framework/CMCapture";
static const unsigned char expected_uuid[16]={0xdb,0x6c,0xb6,0xb3,0xa2,0x6b,0x3a,0x86,0x9e,0x55,0x29,0x9c,0xf1,0xc4,0x85,0xbe};
static const unsigned char expected_prefix[32]={0xf4,0x4f,0xbe,0xa9,0xfd,0x7b,0x01,0xa9,0xfd,0x43,0x00,0x91,0xf3,0x03,0x02,0xaa,0xe3,0x00,0x00,0x34,0x68,0xb0,0x1d,0xb0,0x00,0xe5,0x46,0xf9,0xa8,0x66,0x1c,0xd0};
static _Atomic int installation_state;
static _Atomic unsigned call_count;
static void log_event(const char *event,const char *detail) {
    int saved=errno;char path[128],line[384];
    int n=snprintf(path,sizeof path,"/private/var/tmp/umi-ios165-media.%ld.log",(long)getpid());
    if(n<=0 || (size_t)n>=sizeof path){errno=saved;return;}
    int fd=open(path,O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0644);struct stat st;
    if(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==getuid() && st.st_nlink==1 && st.st_size<65536){
        n=snprintf(line,sizeof line,"UMI-IOS165-MEDIA pid=%ld uid=%ld event=%s %s\n",(long)getpid(),(long)getuid(),event,detail?detail:"");
        if(n>0 && (size_t)n<sizeof line)(void)write(fd,line,(size_t)n);
    }
    if(fd>=0)close(fd);errno=saved;
}
static int original_os(void) {
    int fd=open("/System/Library/CoreServices/SystemVersion.plist",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st;
    if(fd<0)return 0;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>16384){close(fd);return 0;}
    unsigned char bytes[16384];size_t have=0,need=(size_t)st.st_size;
    while(have<need){ssize_t r=read(fd,bytes+have,need-have);if(r<0&&errno==EINTR)continue;if(r<=0){close(fd);return 0;}have+=(size_t)r;}
    close(fd);CFDataRef data=CFDataCreate(kCFAllocatorDefault,bytes,(CFIndex)have);if(!data)return 0;
    CFPropertyListRef p=CFPropertyListCreateWithData(kCFAllocatorDefault,data,kCFPropertyListImmutable,NULL,NULL);CFRelease(data);
    if(!p)return 0;int ok=0;
    if(CFGetTypeID(p)==CFDictionaryGetTypeID()){
        CFTypeRef v=CFDictionaryGetValue(p,CFSTR("ProductVersion")),b=CFDictionaryGetValue(p,CFSTR("ProductBuildVersion"));
        ok=v&&b&&CFGetTypeID(v)==CFStringGetTypeID()&&CFGetTypeID(b)==CFStringGetTypeID()&&CFEqual(v,CFSTR("16.5"))&&CFEqual(b,CFSTR("20F66"));
    }
    CFRelease(p);return ok;
}
static const unsigned char *image_uuid(const void *base) {
    const struct mach_header_64 *h=base;
    if(!h||h->magic!=MH_MAGIC_64||h->ncmds>1024||h->sizeofcmds>1048576||h->sizeofcmds<h->ncmds*8u)return NULL;
    const unsigned char *p=(const void *)(h+1);size_t left=h->sizeofcmds;
    for(uint32_t i=0;i<h->ncmds;i++){
        if(left<sizeof(struct load_command))return NULL;
        const struct load_command *c=(const void *)p;
        if(c->cmdsize<sizeof(*c)||c->cmdsize>left)return NULL;
        if(c->cmd==LC_UUID)return c->cmdsize==sizeof(struct uuid_command)?((const struct uuid_command *)c)->uuid:NULL;
        p+=c->cmdsize;left-=c->cmdsize;
    }
    return NULL;
}
/* This exact void(unsigned32,BOOL) entry is only a prewarming optimization.
 * Avoiding it leaves VideoToolbox and its native software codec untouched. */
static void replacement(id self,SEL selector,unsigned version,BOOL styles) {
    (void)self;(void)selector;(void)version;(void)styles;
    unsigned calls=atomic_fetch_add_explicit(&call_count,1,memory_order_relaxed);
    if(calls<4)log_event("prewarm_skipped","portrait CoreImage prewarm only; native media decode unchanged");
}
static uintptr_t method_get(void *ctx){return (uintptr_t)method_getImplementation((Method)ctx);}
static uintptr_t method_set(void *ctx,uintptr_t imp){return (uintptr_t)method_setImplementation((Method)ctx,(IMP)imp);}
static void attempt_install(const struct mach_header *header,intptr_t image_slide) {
    (void)header;(void)image_slide;
    if(atomic_load_explicit(&installation_state,memory_order_relaxed)!=0)return;
    Class cls=objc_getClass("BWCoreImageFilterRenderer");if(!cls)return;
    int expected=0;
    if(!atomic_compare_exchange_strong_explicit(&installation_state,&expected,1,memory_order_acq_rel,memory_order_relaxed))return;
    Method method=class_getClassMethod(cls,sel_registerName("prewarmPortraitFilterVersion:semanticStyleFilters:"));
    IMP original=method?method_getImplementation(method):NULL;Dl_info info={0};
    if(!original||!dladdr((void *)original,&info)||!info.dli_fname||strcmp(info.dli_fname,image_path)||!info.dli_fbase||
        !umi165_media_guard((uintptr_t)info.dli_fbase,0x1a1e96000ULL,0x1a2115c0cULL,(uintptr_t)original,
            method_getTypeEncoding(method),image_uuid(info.dli_fbase),expected_uuid,(const unsigned char *)original,expected_prefix)){
        log_event("guard_failed","actual CMCapture UUID/IMP/type/32original bytes differ; zero hooks");
        atomic_store_explicit(&installation_state,2,memory_order_release);return;
    }
    struct umi165_media_swap_ops ops={method,method_get,method_set};
    if(!umi165_media_swap(&ops,(uintptr_t)original,(uintptr_t)replacement)){
        log_event("swap_failed","method readback/race; restoration attempted");
        atomic_store_explicit(&installation_state,2,memory_order_release);return;
    }
    atomic_store_explicit(&installation_state,3,memory_order_release);
    log_event("enabled","20F66 exact CMCapture class-method v24@0:8I16B20; only portrait prewarm skipped");
}
__attribute__((constructor)) static void initialize(void) {
    const char *program=getprogname();if(!program||strcmp(program,"mediaserverd"))return;
    log_event("constructor_enter","single portrait prewarm hook for native media decode service");
    const char *enabled=getenv("UMI_MEDIA_ENABLE");
    if(getuid()!=501||geteuid()!=501||!enabled||strcmp(enabled,"1")){log_event("skipped","mobile identity/explicit enable absent");return;}
    if(!original_os()){log_event("guard_failed","OS must be16.5/20F66");return;}
    /* Like the qualified upstream implementation, observe existing and later
     * loaded classes. Do not force-load the capture stack from this constructor. */
    _dyld_register_func_for_add_image(attempt_install);
    if(atomic_load_explicit(&installation_state,memory_order_acquire)==0)
        log_event("waiting_for_cm_capture","dyld callback registered; no method changed yet");
}
