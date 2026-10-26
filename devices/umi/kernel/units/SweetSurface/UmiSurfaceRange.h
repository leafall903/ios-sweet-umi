#pragma once
#include <stdint.h>
static inline bool umi_surface_task_range(uint64_t address,uint64_t size,uint64_t page,uint64_t*first,uint64_t*length,uint32_t*delta) {
 if(!address||!size||size>(256ULL<<20)||!page||(page&(page-1))||page>65536)return false;
 if(address>UINT64_MAX-size || address+size>UINT64_MAX-(page-1))return false;
 *first=address&~(page-1);uint64_t end=(address+size+page-1)&~(page-1);*length=end-*first;*delta=(uint32_t)(address-*first);
 return *length && *length<=(256ULL<<20)+2*page;
}
