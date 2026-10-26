/* Resource bounds checks for the fixed umi firmware inputs; no MMIO. */
#ifndef UMI_GPU_FIRMWARE_H
#define UMI_GPU_FIRMWARE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
static inline uint32_t umi_gpu_le32(const uint8_t *p) {
 return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static inline uint16_t umi_gpu_le16(const uint8_t *p) { return (uint16_t)(p[0]|((uint16_t)p[1]<<8)); }
static inline bool umi_gpu_inputs(const uint8_t *gmu,size_t gn,const uint8_t *sqe,size_t sn,
 const uint8_t *mdt,size_t mn,const uint8_t *seg,size_t zn) {
 if(!gmu||!sqe||!mdt||!seg||gn!=41548||sn!=31632||mn!=6860||zn!=1676) return false;
 for(size_t at=0;at<gn;) {
  if(gn-at<16) return false;
  uint32_t va=umi_gpu_le32(gmu+at),n=umi_gpu_le32(gmu+at+4),kind=umi_gpu_le32(gmu+at+8),val=umi_gpu_le32(gmu+at+12);
  at+=16;
  if(n>gn-at || n>UINT32_MAX-va) return false;
  if(n==0&&(kind==1||kind==7)&&(!val||val>0x1000000u||val>UINT32_MAX-va)) return false;
  at+=n;
 }
 if((umi_gpu_le32(sqe+4)&0xfffu)!=0x93u || (sn&3u)) return false;
 if(mdt[0]!=0x7f||mdt[1]!='E'||mdt[2]!='L'||mdt[3]!='F'||mdt[4]!=1||mdt[5]!=1) return false;
 uint32_t off=umi_gpu_le32(mdt+28);uint32_t count=umi_gpu_le16(mdt+44);
 if(umi_gpu_le16(mdt+42)!=32||count!=3||off>mn||count>(mn-off)/32) return false;
 const uint8_t *load=mdt+off+64;
 return umi_gpu_le32(load)==1&&umi_gpu_le32(load+16)==zn&&umi_gpu_le32(load+20)>=zn&&umi_gpu_le32(load+20)<=0x1000;
}
#endif
