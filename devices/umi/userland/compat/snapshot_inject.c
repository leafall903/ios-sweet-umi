/* Isolated20F66 SpringBoard snapshot format adaptation. No setup, activation,
 * battery, CT, wallpaper-editor or registration hooks. */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <objc/runtime.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "snapshot_logic.h"
static const char *image_path="/System/Library/PrivateFrameworks/SplashBoard.framework/SplashBoard";
static const unsigned char expected_uuid[16]={0x64,0x71,0x64,0x58,0x95,0xf7,0x33,0x44,0xa2,0x3a,0x0a,0x2b,0xac,0xff,0x46,0xe5};
static const unsigned char expected_prefix[32]={0xf6,0x57,0xbd,0xa9,0xf4,0x4f,0x01,0xa9,0xfd,0x7b,0x02,0xa9,0xfd,0x83,0x00,0x91,0x07,0xc8,0x7e,0x94,0xf3,0x03,0x00,0xaa,0x08,0x03,0x11,0xf0,0x08,0x35,0x45,0xf9};
static _Atomic unsigned call_count;
static void log_event(const char *event,const char *detail) {
    int saved=errno;char path[128],line[384];
    int n=snprintf(path,sizeof path,"/private/var/tmp/umi-ios165-snapshot.%ld.log",(long)getpid());
    if(n<=0 || (size_t)n>=sizeof path){errno=saved;return;}
    int fd=open(path,O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0644);struct stat st;
    if(fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==getuid() && st.st_nlink==1 && st.st_size<65536){
        n=snprintf(line,sizeof line,"UMI-IOS165-SNAPSHOT pid=%ld uid=%ld event=%s %s\n",(long)getpid(),(long)getuid(),event,detail?detail:"");
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
/* Validate the runtime method ABI before calling an object's implementation.
 * Calling the fetched IMP avoids a second selector lookup changing its type. */
static int opaque_value(id snapshot,int *valid) {
    *valid=0;if(!snapshot)return 0;
    SEL selector=sel_registerName("isImageOpaque");Method m=class_getInstanceMethod(object_getClass(snapshot),selector);
    if(!m || method_getNumberOfArguments(m)!=2)return 0;
    char result[8]={0},self_type[8]={0},cmd_type[8]={0};
    method_getReturnType(m,result,sizeof result);method_getArgumentType(m,0,self_type,sizeof self_type);method_getArgumentType(m,1,cmd_type,sizeof cmd_type);
    if(strcmp(result,"B")||strcmp(self_type,"@")||strcmp(cmd_type,":"))return 0;
    IMP imp=method_getImplementation(m);if(!imp)return 0;
    *valid=1;return ((BOOL(*)(id,SEL))imp)(snapshot,selector)!=NO;
}
static long replacement(id self,SEL selector,id snapshot) {
    (void)self;(void)selector;int valid=0,opaque=opaque_value(snapshot,&valid);
    long result=umi165_snapshot_format(snapshot!=NULL,valid,opaque);
    unsigned calls=atomic_fetch_add_explicit(&call_count,1,memory_order_relaxed);
    if(calls<8){char line[128];snprintf(line,sizeof line,"call=%u format=%ld snapshot=%d bool_abi=%d opaque=%d",calls+1,result,snapshot!=NULL,valid,opaque);log_event("format_selected",line);}
    return result;
}
static uintptr_t method_get(void *ctx){return (uintptr_t)method_getImplementation((Method)ctx);}
static uintptr_t method_set(void *ctx,uintptr_t imp){return (uintptr_t)method_setImplementation((Method)ctx,(IMP)imp);}
__attribute__((constructor)) static void initialize(void) {
    const char *program=getprogname();if(!program||strcmp(program,"SpringBoard"))return;
    log_event("constructor_enter","single snapshot format hook");
    const char *enabled=getenv("UMI_SNAPSHOT_ENABLE");
    if(getuid()!=501||geteuid()!=501||!enabled||strcmp(enabled,"1")){log_event("skipped","mobile identity/explicit enable absent");return;}
    if(!original_os()){log_event("guard_failed","OS must be16.5/20F66");return;}
    void *image=dlopen(image_path,RTLD_LAZY|RTLD_LOCAL);if(!image){log_event("guard_failed","SplashBoard load failed");return;}
    /* Keep the handle resident: the hooked class must not be unloaded. */
    Class cls=objc_getClass("XBApplicationSnapshotManifestImpl");
    Method method=cls?class_getClassMethod(cls,sel_registerName("_outputFormatForSnapshot:")):NULL;
    IMP original=method?method_getImplementation(method):NULL;Dl_info info={0};
    if(!original||!dladdr((void *)original,&info)||!info.dli_fname||strcmp(info.dli_fname,image_path)||!info.dli_fbase||
        !umi165_snapshot_guard((uintptr_t)info.dli_fbase,0x1f1c82000ULL,0x1f1c91480ULL,(uintptr_t)original,
            method_getTypeEncoding(method),image_uuid(info.dli_fbase),expected_uuid,(const unsigned char *)original,expected_prefix)){
        log_event("guard_failed","actual image UUID/IMP/type/32original bytes differ; zero hooks");return;
    }
    struct umi165_snapshot_swap_ops ops={method,method_get,method_set};
    if(!umi165_snapshot_swap(&ops,(uintptr_t)original,(uintptr_t)replacement)){log_event("swap_failed","method readback/race; restoration attempted");return;}
    log_event("enabled","20F66 actual SplashBoard UUID/IMP/type/prefix matched; PNG0/JPEG3");
}
