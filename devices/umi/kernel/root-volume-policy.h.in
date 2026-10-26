#ifndef UMI_ROOT_VOLUME_H
#define UMI_ROOT_VOLUME_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define UMI_ROOT_BLOCKS 4194304ULL
static bool umi_root_mode_valid(uint32_t opt,uint32_t cpus,uint32_t rw,uint32_t rootrw,uint32_t log,uint32_t trace) {
 return opt==1 && (cpus==1||cpus==4||cpus==8) && rw<=1 && rootrw==rw && log==0 && trace==0;
}
static uint32_t umi_root_be32(const uint8_t *p) {return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static bool umi_root_header_valid(const uint8_t *h,size_t n) {
 static const uint8_t id[8]={0,0,0,0,0,0,0,0};
 if(!h||n<512||h[0]!=0x48||h[1]!=0x58||h[2]!=0||h[3]!=5||umi_root_be32(h+40)!=4096||umi_root_be32(h+44)!=UMI_ROOT_BLOCKS) return false;
 for(unsigned i=0;i<8;i++)if(h[104+i]!=id[i])return false;
 return true;
}
#endif
