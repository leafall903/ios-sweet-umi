/* Shared production/test logic. No Apple private addresses in decision code. */
#ifndef UMI165_SNAPSHOT_LOGIC_H
#define UMI165_SNAPSHOT_LOGIC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define UMI165_SNAPSHOT_PNG 0L
#define UMI165_SNAPSHOT_JPEG 3L
static long umi165_snapshot_format(int has_snapshot, int valid_bool_method, int opaque) {
    return has_snapshot && valid_bool_method && opaque ? UMI165_SNAPSHOT_JPEG : UMI165_SNAPSHOT_PNG;
}
static int umi165_snapshot_guard(uintptr_t image_runtime, uintptr_t image_unslid,
    uintptr_t expected_imp, uintptr_t actual_imp, const char *actual_types,
    const unsigned char *actual_uuid, const unsigned char *expected_uuid,
    const unsigned char *actual_prefix, const unsigned char *expected_prefix) {
    if (!image_runtime || expected_imp < image_unslid ||
        expected_imp-image_unslid > UINTPTR_MAX-image_runtime) return 0;
    return actual_imp == image_runtime+(expected_imp-image_unslid) &&
        actual_types && !strcmp(actual_types,"q24@0:8@16") &&
        actual_uuid && expected_uuid && !memcmp(actual_uuid,expected_uuid,16) &&
        actual_prefix && expected_prefix && !memcmp(actual_prefix,expected_prefix,32);
}
/* Ops keep the one-method swap testable. On a racing third-party change, restore
 * the implementation actually displaced, never the stale preflight value. */
struct umi165_snapshot_swap_ops {
    void *ctx;
    uintptr_t (*get)(void *ctx);
    uintptr_t (*set)(void *ctx, uintptr_t implementation);
};
static int umi165_snapshot_swap(struct umi165_snapshot_swap_ops *ops,
    uintptr_t expected, uintptr_t replacement) {
    if (!ops || !ops->get || !ops->set || !expected || !replacement || expected==replacement ||
        ops->get(ops->ctx)!=expected) return 0;
    uintptr_t displaced=ops->set(ops->ctx,replacement);
    if (displaced==expected && ops->get(ops->ctx)==replacement) return 1;
    if (displaced && ops->get(ops->ctx)==replacement) (void)ops->set(ops->ctx,displaced);
    return 0;
}
#endif
