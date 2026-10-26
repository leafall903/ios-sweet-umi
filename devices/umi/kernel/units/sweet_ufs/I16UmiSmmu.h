/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * C adaptation of reAAAq/UniXNU-umi package/UmiSmmuTakeover.h.
 * Same dual-stream policy and register order; callbacks add no I/O by themselves.
 */
#ifndef I16_UMI_SMMU_H
#define I16_UMI_SMMU_H
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t (*I16Read)(void *,unsigned);
typedef void (*I16Write)(void *,unsigned,uint32_t);
typedef struct {
 unsigned bank,pages,count,slot[2]; bool reuse[2]; unsigned error;
} I16SmmuPlan;
static const uint16_t i16_streams[2] = {0xe0,0x4e0};
static I16SmmuPlan i16_smmu_plan(uint64_t dma, uint64_t bytes, I16Read read, void *ctx)
{
  I16SmmuPlan p = {0}; p.slot[0] = p.slot[1] = ~0U;
  uint32_t id0=read(ctx,0x20), id1=read(ctx,0x24), id2=read(ctx,0x28);
  p.count=id0&255;
  p.pages=1U<<(((id1>>28)&7)+1);
  unsigned banks=id1&255;
  if (!(id0&(1U<<30)) || !(id0&(1U<<27)) || !(id2&(1U<<12)) ||
      !p.count || p.count>128 || (id1>>31) || p.pages>128 ||
      banks<2 || banks>128 || (p.pages+banks)*4096>0xd0000) {
    p.error=1; return p;
  }
  const unsigned bits[]={32,36,40,42,44,48};
  for(unsigned shift=0;shift<=8;shift+=4) {
    unsigned code=(id2>>shift)&15;
    if(code>=6 || !bytes || dma>=(1ULL<<bits[code]) ||
       bytes>(1ULL<<bits[code])-dma) { p.error=2; return p; }
  }
  p.bank=banks-1;
  unsigned freeSlots[128], freeCount=0;
  for(unsigned i=0;i<p.count;++i) {
    uint32_t smr=read(ctx,0x800+4*i);
    if(!(smr&0x80000000U)) { freeSlots[freeCount++]=i; continue; }
    uint32_t s2cr=read(ctx,0xc00+4*i), mask=(smr>>16)&0x7fff;
    bool ownBank=((s2cr>>16)&3)==0 && (s2cr&255)==p.bank;
    bool owned=false;
    for(unsigned s=0;s<2;++s) {
      if(((smr^i16_streams[s])&0xffff&~mask)!=0) continue;
      if(p.reuse[s] || mask || !ownBank) { p.error=3; return p; }
      p.reuse[s]=true; p.slot[s]=i; owned=true;
    }
    if(ownBank && !owned) { p.error=4; return p; }
  }
  uint32_t sctlr=read(ctx,(p.pages+p.bank)*4096);
  if((sctlr&1) ||
      ((p.reuse[0]||p.reuse[1]) &&
       (sctlr || read(ctx,0x1000+4*p.bank)!=0x10000 ||
        (read(ctx,0x1800+4*p.bank)&~1U)))) { p.error=5; return p; }
  for(unsigned s=0;s<2;++s) if(!p.reuse[s]) {
    if(!freeCount) { p.error=6; return p; }
    p.slot[s]=freeSlots[--freeCount];
  }
  return p;
}

// Queue must be stopped before calling. Every write must be ordered by the
// caller. Refused writes stop immediately; no DMA may start on any failure.
static unsigned i16_smmu_apply(const I16SmmuPlan *p,I16Read read,I16Write write,void *ctx)
{
  if(p->error) return p->error;
  unsigned cb=(p->pages+p->bank)*4096;
  // Recheck the complete plan before the first write, including other streams.
  // DMA range validation was performed by the planner on the actual allocation.
  for(unsigned s=0;s<2;++s) {
    uint32_t smr=read(ctx,0x800+4*p->slot[s]);
    if(p->reuse[s] ? (smr!=(0x80000000U|i16_streams[s]) ||
         read(ctx,0xc00+4*p->slot[s])!=p->bank) : (smr&0x80000000U)) return 7;
  }
  if(read(ctx,cb)&1) return 7;
  if(!p->reuse[0] && !p->reuse[1]) write(ctx,cb,0);
  if(read(ctx,cb)!=0) return 8;
  if(read(ctx,0x1800+4*p->bank)!=1) write(ctx,0x1800+4*p->bank,1);
  if(!p->reuse[0] && !p->reuse[1]) write(ctx,0x1000+4*p->bank,0x10000);
  if(read(ctx,0x1800+4*p->bank)!=1 || read(ctx,0x1000+4*p->bank)!=0x10000 ||
     read(ctx,cb)!=0) return 9;
  // Configure both routes before making either new stream valid.
  for(unsigned s=0;s<2;++s) if(!p->reuse[s]) {
    write(ctx,0xc00+4*p->slot[s],p->bank);
    if(read(ctx,0xc00+4*p->slot[s])!=p->bank) return 10;
  }
  for(unsigned s=0;s<2;++s) if(!p->reuse[s]) {
    write(ctx,0x800+4*p->slot[s],0x80000000U|i16_streams[s]);
    if(read(ctx,0x800+4*p->slot[s])!=(0x80000000U|i16_streams[s]) ||
       read(ctx,0xc00+4*p->slot[s])!=p->bank) return 11;
  }
  return 0;
}

#endif
