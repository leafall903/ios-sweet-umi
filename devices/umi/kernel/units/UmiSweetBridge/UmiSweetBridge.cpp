// Local iOS 16 prototype: ST frame queue -> built-in IOHIDFamily 1915 service.
// Hardware is opt-in with umi-fts=1; absent/zero performs no MMIO or power IO.
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "UmiSmpMode.h"
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include "UmiSweetFrameBridge.h"
extern "C" {
bool umi_touch_next(UmiTouchFrame *frame);
void native_touch_probe(void);
bool umi_hid_input_ready(void);
void umi_keys_start(void);
void sweet_hid_touch(int finger,int down,int x,int y);
extern volatile uint32_t native_touch_hid_reports;
// Compatibility diagnostics only. No IOHID2031 loader or edge vtable hook.
char boot_progress_ext[8][48];
volatile uint32_t native_hid_stage,native_hid_resource_found,native_hid_input_events[8],native_touch_edge_stats[4];
volatile int native_hid_load_status=-1,native_hid_matching_load_status=-1,native_hid_input_results[8];
}
static bool bridge_started;
static void bridge_worker(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"umi_hid16");
    for(unsigned n=0;n<3000 && !umi_hid_input_ready();++n) IOSleep(10);
    if(!umi_hid_input_ready()) { IOLog("UMI input: HID registration timeout; hardware untouched\n"); thread_terminate(current_thread()); return; }
    IOLog("UMI input: UFS ready and HID registered; starting native input\n");
    native_touch_probe();
    umi_keys_start();
    UmiTouchFrame delivered={};
    for(;;) {
        UmiTouchFrame next;
        for(unsigned n=0;n<8 && umi_touch_next(&next);++n) {
            unsigned attempted=umi_sweet_deliver(delivered,next,[](int id,int down,int x,int y){sweet_hid_touch(id,down,x,y);});
            native_touch_hid_reports=native_touch_hid_reports+attempted;
        }
        IOSleep(8);
    }
}
extern "C" void umi_sweet_touch_start(void) {
    unsigned enabled=0;
    if(!PE_parse_boot_argn("umi-fts",&enabled,sizeof(enabled)) || enabled!=1 || bridge_started)return;
    unsigned gpi=0,cpus=0,root=0,smmu=0;
    if(!PE_parse_boot_argn("umi-fts-gpi",&gpi,4)||gpi!=1 ||
       !PE_parse_boot_argn("cpus",&cpus,4)||!umi_cpu_boot_valid(cpus) ||
       !PE_parse_boot_argn("umi-rootfs",&root,4)||root!=1 ||
       !PE_parse_boot_argn("umi-smmu",&smmu,4)||smmu!=1)return;
    thread_t thread;
    if(kernel_thread_start(bridge_worker,nullptr,&thread)!=KERN_SUCCESS)return;
    bridge_started=true;thread_deallocate(thread);
}
