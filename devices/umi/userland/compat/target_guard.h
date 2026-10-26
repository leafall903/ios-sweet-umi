/* Actual20F66 identities, read from verified Apple SystemOS; fail closed. */
#ifndef UMI165_TARGET_GUARD_H
#define UMI165_TARGET_GUARD_H
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <string.h>
#define UMI165_CACHE_BASE 0x180000000ULL
static const unsigned char umi165_cache_uuid[16]={0xe5,0x42,0x0d,0xb3,0x30,0xd0,0x33,0xaa,0x8b,0x7e,0x05,0xb5,0x03,0x8b,0xb4,0x6d};
static int umi165_cache_matches(const void *cache,size_t length) {
 return cache && length>=104 && !memcmp(cache,"dyld_v1   arm64",15) && !memcmp((const unsigned char*)cache+88,umi165_cache_uuid,16);
}
static int umi165_image_matches(const void *address,const unsigned char uuid[16]) {
 Dl_info info;
 if(!dladdr(address,&info) || !info.dli_fbase) return 0;
 const struct mach_header_64 *h=info.dli_fbase;
 if(h->magic!=MH_MAGIC_64 || h->ncmds>1024 || h->sizeofcmds>(1u<<20) || h->sizeofcmds<h->ncmds*8u) return 0;
 const unsigned char *p=(const unsigned char*)(h+1);size_t left=h->sizeofcmds;
 for(uint32_t i=0;i<h->ncmds;i++) {
  if(left<sizeof(struct load_command)) return 0;
  const struct load_command *c=(const void*)p;
  if(c->cmdsize<sizeof(*c) || c->cmdsize>left) return 0;
  if(c->cmd==LC_UUID) return c->cmdsize==sizeof(struct uuid_command) && !memcmp(((const struct uuid_command*)c)->uuid,uuid,16);
  left-=c->cmdsize;p+=c->cmdsize;
 }
 return 0;
}
#endif
