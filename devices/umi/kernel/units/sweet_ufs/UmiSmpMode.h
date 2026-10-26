#pragma once
#include <stdint.h>
#include <stdbool.h>
static bool umi_cpu_mode_valid(uint32_t cpus,uint32_t gui,uint32_t aff) {
 return cpus==1 ? gui==0 : (cpus==4 || cpus==8) && gui==1 && aff==cpus-1;
}
static bool umi_cpu_boot_valid(uint32_t cpus) {
 uint32_t gui=0,aff=UINT32_MAX;
 PE_parse_boot_argn("umi-smp-gui",&gui,4);PE_parse_boot_argn("sweetaff1max",&aff,4);
 return umi_cpu_mode_valid(cpus,gui,aff);
}
