/* Single-user compatibility hook, with version guard and positive runtime evidence.
 * Adapted from local umi-compat-candidate, ios-topaz, and HackiOS inject.c.
 * This does not repair WidgetKit rendering or Metal. No filesystem installation occurs here.
 */
#include <objc/runtime.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "probe_log.h"
#include "target_guard.h"
extern const void *_dyld_get_shared_cache_range(size_t *length);
static const uintptr_t cache_base = 0x180000000ULL;
static const uintptr_t method_address = 0x1c3ea6298ULL;
static const unsigned char expected[16] = {0xff,0xc3,0x00,0xd1,0xf4,0x4f,0x01,0xa9,0xfd,0x7b,0x02,0xa9,0xfd,0x83,0x00,0x91};
static atomic_flag said = ATOMIC_FLAG_INIT;
static BOOL no_personas(id self, SEL selector) {
    (void)self; (void)selector;
    if (!atomic_flag_test_and_set_explicit(&said, memory_order_relaxed)) umi_ios165_log("hook_called", "personasAreSupported returns NO");
    return NO;
}
__attribute__((constructor)) static void umi_v20_container_fix(void) {
    const char *name = getprogname();
    umi_ios165_log("constructor_enter", name);
    if (!name || (strcmp(name, "containermanagerd") && strcmp(name, "containermanagerd_system"))) {
        umi_ios165_log("skip_process", "unexpected executable"); return;
    }
    size_t length = 0;
    uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length);
    uintptr_t delta = method_address - cache_base;
    if (!umi165_cache_matches((const void*)base,length) || length < sizeof expected || delta > length - sizeof expected || base > UINTPTR_MAX - delta) {
        umi_ios165_log("skip_cache_range", "20F66 method outside returned range"); return;
    }
    Class cls = objc_getClass("MCMUserIdentityCache");
    Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
    IMP original = method ? method_getImplementation(method) : NULL;
    const unsigned char mcm_uuid[16]={0x89,0xca,0x33,0xee,0x58,0xc4,0x3d,0x58,0x89,0x2a,0x06,0xad,0x42,0x37,0xb8,0x3c};
    const char *types=method?method_getTypeEncoding(method):NULL;
    if (!original || !types || strcmp(types,"B16@0:8") || !umi165_image_matches((const void*)original,mcm_uuid) || (uintptr_t)original != base + delta || memcmp((const void *)original, expected, sizeof expected)) {
        umi_ios165_log("skip_version_guard", "class/method/address/16-byte prefix mismatch"); return;
    }
    method_setImplementation(method, (IMP)no_personas);
    if (method_getImplementation(method) != (IMP)no_personas) {
        umi_ios165_log("hook_not_active", "IMP readback mismatch"); return;
    }
    umi_ios165_log("hook_enabled", "20F66 expected original matched; replacement IMP read back");
}
