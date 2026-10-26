// SPDX-License-Identifier: BSD-2-Clause-Patent
// Port of UniXNU-umi UmiButtonsDxe observer reads. No PMIC writes/IRQ ACK.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <pexpert/pexpert.h>
#include "UmiSmpMode.h"
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include "UmiKeyState.h"
extern "C" bool umi_input_is_umi(void);
extern "C" void sweet_hid_key(int,int);
static uintptr_t core,config,observer;
struct KeyChannel { unsigned index; uint32_t mapping,owner; };
static uint32_t read32(uintptr_t a) { uint32_t v=*(volatile uint32_t *)a; __asm__ volatile("dsb sy":::"memory"); return v; }
static bool mapRegion(uint64_t pa,unsigned bytes,uintptr_t &va) {
    IODeviceMemory *m=IODeviceMemory::withRange(pa,bytes); if(!m)return false;
    IOMemoryMap *r=m->map(kIOMapAnywhere|kIOMapInhibitCache); m->release();
    if(!r)return false; va=r->getVirtualAddress(); return true;
}
static bool findChannel(unsigned ppid,KeyChannel &out) {
    bool found=false;
    for(unsigned i=0;i<512;++i) {
        uint32_t m=read32(core+0x900+4*i),o=read32(config+0x700+4*i)&7;
        if(m && ((m>>8)&0xfff)==ppid && (!found || !o)) { out={i,m,o}; found=true; }
    }
    return found;
}
static bool statusByte(const KeyChannel &c,unsigned &v) {
    if(read32(core+0x900+4*c.index)!=c.mapping || (read32(config+0x700+4*c.index)&7)!=c.owner)return false;
    uintptr_t a=observer+0x80*c.index;
    // Observer EXT_READL offset 0x10 length 1; no peripheral writes.
    __asm__ volatile("dsb sy":::"memory");
    *(volatile uint32_t *)a=(1U<<27)|(0x10U<<4);
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned n=0;n<1000;++n) {
        uint32_t s=read32(a+8);
        if(s&1) { if(s&14)return false; v=read32(a+0x18)&255; return true; }
        IODelay(1);
    }
    return false;
}
static void keysWorker(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"umi_keys16");
    if(!umi_input_is_umi() || !mapRegion(0xc440000,0x2000,core) ||
       !mapRegion(0xc40a000,0x1000,config) || !mapRegion(0xe600000,0x10000,observer)) {
        IOLog("UMI keys: board/map refused\n");return;
    }
    KeyChannel pon={},gpio={};
    if(!findChannel(0x008,pon) || !findChannel(0x0c5,gpio) || pon.index==gpio.index) {
        IOLog("UMI keys: observer routes unavailable\n");return;
    }
    IOLog("UMI keys: observer-only PON=%u GPIO6=%u\n",pon.index,gpio.index);
    UmiKeyState state;unsigned errors=0;
    auto emit=[](int k,int down){sweet_hid_key(k,down);};
    for(;;) {
        unsigned p=0,g=0;
        if(!statusByte(pon,p) || !statusByte(gpio,g)) {
            state.reset(emit);
            if(++errors>=8){IOLog("UMI keys: read/ownership failure; stopped with releases\n");return;}
        } else {
            errors=0; state.update(((g&1)?0:1)|(p&2)|((p&1)<<2),emit);
        }
        IOSleep(20);
    }
}
static void keysEntry(void *p,wait_result_t w) { keysWorker(p,w); thread_terminate(current_thread()); }
extern "C" void umi_keys_start(void) {
    unsigned enabled=0,cpus=0;
    if(!PE_parse_boot_argn("umi-keys",&enabled,4)||enabled!=1 ||
       !PE_parse_boot_argn("cpus",&cpus,4)||!umi_cpu_boot_valid(cpus))return;
    thread_t t; if(kernel_thread_start(keysEntry,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);
}
